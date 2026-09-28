#include "ncnn_runtime.hpp"

#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <blob.h>
#include <mat.h>
#include <net.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mirador::integrations {

namespace {

/// ncnn returns 0 on success; anything else is a load/extract failure with a
/// negative layer-level error code (net.h contract).
Status ncnn_status(ErrorCode code, const char* what, int ret) {
    return Status{code, std::string{what} + " (ncnn error " + std::to_string(ret) + ")"};
}

/// Blob lookup through the public accessor; ncnn::Net::find_blob_index_by_name
/// is protected in the pinned release (net.h).
bool has_blob(const ncnn::Net& net, const std::string& name) {
    const std::vector<ncnn::Blob>& blobs = net.blobs();
    return std::any_of(blobs.begin(), blobs.end(), [&name](const ncnn::Blob& blob) { return blob.name == name; });
}

}  // namespace

struct NcnnRuntime::Impl {
    ncnn::Net net;
    int num_threads = 1;
};

NcnnRuntime::NcnnRuntime() noexcept = default;

NcnnRuntime::NcnnRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

NcnnRuntime::NcnnRuntime(NcnnRuntime&& other) noexcept = default;
NcnnRuntime& NcnnRuntime::operator=(NcnnRuntime&& other) noexcept = default;

NcnnRuntime::~NcnnRuntime() = default;

Result<NcnnRuntime> NcnnRuntime::create(const NcnnRuntimeOptions& options) {
    if (options.param_path.empty() || options.bin_path.empty()) {
        return Status{ErrorCode::kInvalidArgument, "ncnn param and bin paths are required"};
    }
    if (options.num_threads < 1) {
        return Status{ErrorCode::kInvalidArgument, "num_threads must be >= 1"};
    }
    auto impl = std::make_unique<Impl>();
    impl->num_threads = options.num_threads;
    impl->net.opt.num_threads = options.num_threads;
    // The documented tensor surface of this wrapper is planar CHW float
    // (cstep copied out compactly). ncnn's packing layout would fold small
    // channel counts into fewer, wider channels (elempack), which the plain
    // tensor cannot represent — a multi-blob chain (M7-12 NanoTrack head) fed
    // from such a folded tensor reinterprets the interleaved planes as a
    // different channel count. Unpacked blobs keep the tensor surface exact;
    // the reference backends trade ncnn's packed kernels for that fidelity.
    impl->net.opt.use_packing_layout = false;
    // Models must come from explicit caller paths (DEC-015); a failed load is
    // surfaced, never retried from another source.
    if (const int ret = impl->net.load_param(options.param_path.c_str()); ret != 0) {
        return ncnn_status(ErrorCode::kBackendUnavailable, "loading param failed", ret);
    }
    if (const int ret = impl->net.load_model(options.bin_path.c_str()); ret != 0) {
        return ncnn_status(ErrorCode::kBackendUnavailable, "loading model failed", ret);
    }
    return NcnnRuntime{std::move(impl)};
}

int NcnnRuntime::num_threads() const noexcept {
    return impl_ != nullptr ? impl_->num_threads : 0;
}

namespace {

/// Per-entry input validation: non-empty blob name and tensor shape/data
/// consistency.
Status check_input_tensors(const std::vector<NcnnNamedTensor>& inputs) {
    for (const NcnnNamedTensor& entry : inputs) {
        if (entry.blob.empty()) {
            return Status{ErrorCode::kInvalidArgument, "blob names are required"};
        }
        const size_t input_plane = static_cast<size_t>(entry.tensor.width) * entry.tensor.height;
        if (entry.tensor.width <= 0 || entry.tensor.height <= 0 || entry.tensor.channels <= 0 ||
            entry.tensor.data.size() != input_plane * static_cast<size_t>(entry.tensor.channels)) {
            return Status{ErrorCode::kInvalidArgument, "input tensor shape and data size mismatch"};
        }
    }
    return Status::success();
}

bool has_duplicate(const std::vector<std::string>& names) {
    for (size_t i = 0; i < names.size(); ++i) {
        for (size_t j = i + 1; j < names.size(); ++j) {
            if (names[i] == names[j]) {
                return true;
            }
        }
    }
    return false;
}

/// Blob-existence check through the public accessor.
Status check_blobs_exist(const ncnn::Net& net, const std::vector<std::string>& input_names,
                         const std::vector<std::string>& output_blobs) {
    for (const std::string& name : input_names) {
        if (!has_blob(net, name)) {
            return Status{ErrorCode::kInvalidArgument, "unknown input blob '" + name + "'"};
        }
    }
    for (const std::string& name : output_blobs) {
        if (!has_blob(net, name)) {
            return Status{ErrorCode::kInvalidArgument, "unknown output blob '" + name + "'"};
        }
    }
    return Status::success();
}

/// Checks every run_multi argument before any forward work: non-empty blob
/// names, tensor shape/data consistency, uniqueness within the call, blob
/// existence in the net, and the entry cancellation/deadline checks.
Status check_run_multi_args(const ncnn::Net& net, const std::vector<NcnnNamedTensor>& inputs,
                            const std::vector<std::string>& output_blobs, const ExecutionContext& context) {
    if (inputs.empty() || output_blobs.empty()) {
        return Status{ErrorCode::kInvalidArgument, "at least one input and output blob are required"};
    }
    if (const Status checked = check_input_tensors(inputs); !checked.ok()) {
        return checked;
    }
    std::vector<std::string> input_names;
    input_names.reserve(inputs.size());
    for (const NcnnNamedTensor& entry : inputs) {
        input_names.push_back(entry.blob);
    }
    if (has_duplicate(input_names) || has_duplicate(output_blobs)) {
        return Status{ErrorCode::kInvalidArgument, "blob names must be unique within one call"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled before ncnn forward"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached before ncnn forward"};
    }
    return check_blobs_exist(net, input_names, output_blobs);
}

/// Copies the caller tensors into ncnn-owned Mats so plane layout (cstep
/// alignment) is ncnn's own choice; the caller tensors stay planar-packed and
/// untouched.
std::vector<ncnn::Mat> build_input_mats(const std::vector<NcnnNamedTensor>& inputs) {
    std::vector<ncnn::Mat> in_mats;
    in_mats.reserve(inputs.size());
    for (const NcnnNamedTensor& entry : inputs) {
        const size_t input_plane = static_cast<size_t>(entry.tensor.width) * entry.tensor.height;
        ncnn::Mat mat(entry.tensor.width, entry.tensor.height, entry.tensor.channels);
        const auto* src = entry.tensor.data.data();
        auto* dst_base = static_cast<float*>(mat.data);
        for (int c = 0; c < entry.tensor.channels; ++c) {
            std::memcpy(dst_base + static_cast<size_t>(c) * mat.cstep, src + static_cast<size_t>(c) * input_plane,
                        input_plane * sizeof(float));
        }
        in_mats.push_back(std::move(mat));
    }
    return in_mats;
}

/// Copies planes back compactly (plane-major, no cstep padding gaps).
std::vector<NcnnTensor> copy_output_planes(const std::vector<ncnn::Mat>& out_mats) {
    std::vector<NcnnTensor> outputs;
    outputs.reserve(out_mats.size());
    for (const ncnn::Mat& out_mat : out_mats) {
        const size_t output_plane = static_cast<size_t>(out_mat.w) * out_mat.h;
        NcnnTensor output;
        output.width = out_mat.w;
        output.height = out_mat.h;
        output.channels = out_mat.c;
        output.data.resize(output_plane * static_cast<size_t>(out_mat.c));
        const auto* out_base = static_cast<const float*>(out_mat.data);
        for (int c = 0; c < out_mat.c; ++c) {
            std::memcpy(output.data.data() + static_cast<size_t>(c) * output_plane,
                        out_base + static_cast<size_t>(c) * out_mat.cstep, output_plane * sizeof(float));
        }
        outputs.push_back(std::move(output));
    }
    return outputs;
}

}  // namespace

Result<NcnnTensor> NcnnRuntime::run(const std::string& input_blob, const NcnnTensor& input,
                                    const std::string& output_blob, const ExecutionContext& context) const {
    auto outputs = run_multi({{input_blob, input}}, {output_blob}, context);
    if (!outputs.ok()) {
        return outputs.status();
    }
    auto values = std::move(outputs).take_value();
    return std::move(values.at(0));
}

Result<std::vector<NcnnTensor>> NcnnRuntime::run_multi(const std::vector<NcnnNamedTensor>& inputs,
                                                       const std::vector<std::string>& output_blobs,
                                                       const ExecutionContext& context) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "runtime moved-from"};
    }
    if (const Status checked = check_run_multi_args(impl_->net, inputs, output_blobs, context); !checked.ok()) {
        return checked;
    }

    const std::vector<ncnn::Mat> in_mats = build_input_mats(inputs);
    // One extractor per output blob: the pinned ncnn extractor resolves a
    // single graph output per extract call (a second extract on the same
    // extractor fails with -100 once another blob has been produced). Inputs
    // are re-set on every extractor, so each forward sees identical bytes and
    // the outputs stay mutually consistent. Each forward is atomic and cannot
    // honor cancellation mid-flight; bounds are the caller's responsibility
    // (class contract).
    std::vector<ncnn::Mat> out_mats;
    out_mats.reserve(output_blobs.size());
    for (const std::string& name : output_blobs) {
        ncnn::Extractor output_extractor = impl_->net.create_extractor();
        for (size_t j = 0; j < inputs.size(); ++j) {
            if (const int ret = output_extractor.input(inputs[j].blob.c_str(), in_mats[j]); ret != 0) {
                return ncnn_status(ErrorCode::kBackendFailure, "setting input blob failed", ret);
            }
        }
        ncnn::Mat out_mat;
        if (const int ret = output_extractor.extract(name.c_str(), out_mat); ret != 0) {
            return ncnn_status(ErrorCode::kBackendFailure, "extracting output blob failed", ret);
        }
        out_mats.push_back(std::move(out_mat));
    }

    auto outputs = copy_output_planes(out_mats);
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled after ncnn forward"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached after ncnn forward"};
    }
    return outputs;
}

Result<NcnnTensor> pack_image(const ImageView& image, const ExecutionContext& context) {
    if (const auto valid = validate(image); !valid.ok()) {
        return valid.status();
    }
    int channels = 0;
    switch (image.format) {
        case PixelFormat::kGray8:
            channels = 1;
            break;
        case PixelFormat::kRgb8:
            channels = 3;
            break;
        case PixelFormat::kRgba8:
            channels = 4;
            break;
        default:
            return Status{ErrorCode::kUnsupportedFormat, "pack_image supports Gray8, Rgb8 and Rgba8 only"};
    }

    NcnnTensor tensor;
    tensor.width = image.width;
    tensor.height = image.height;
    tensor.channels = channels;
    const size_t plane = static_cast<size_t>(image.width) * image.height;
    tensor.data.resize(plane * static_cast<size_t>(channels));
    const int64_t row_stride = image.row_stride_bytes;
    for (int32_t y = 0; y < image.height; ++y) {
        // Periodic cancellation poll on the packing loop (design section 20).
        if (y % 64 == 0 && is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled while packing image"};
        }
        const std::byte* row = image.data + static_cast<int64_t>(y) * row_stride;
        const size_t row_offset = static_cast<size_t>(y) * image.width;
        for (int32_t x = 0; x < image.width; ++x) {
            for (int c = 0; c < channels; ++c) {
                tensor.data[static_cast<size_t>(c) * plane + row_offset + static_cast<size_t>(x)] =
                    static_cast<float>(std::to_integer<int>(row[static_cast<int64_t>(x) * channels + c]));
            }
        }
    }
    return tensor;
}

}  // namespace mirador::integrations
