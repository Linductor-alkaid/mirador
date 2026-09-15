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

Result<NcnnTensor> NcnnRuntime::run(const std::string& input_blob, const NcnnTensor& input,
                                    const std::string& output_blob, const ExecutionContext& context) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "runtime moved-from"};
    }
    if (input_blob.empty() || output_blob.empty()) {
        return Status{ErrorCode::kInvalidArgument, "blob names are required"};
    }
    const size_t input_plane = static_cast<size_t>(input.width) * input.height;
    if (input.width <= 0 || input.height <= 0 || input.channels <= 0 ||
        input.data.size() != input_plane * static_cast<size_t>(input.channels)) {
        return Status{ErrorCode::kInvalidArgument, "input tensor shape and data size mismatch"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled before ncnn forward"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached before ncnn forward"};
    }
    if (!has_blob(impl_->net, input_blob)) {
        return Status{ErrorCode::kInvalidArgument, "unknown input blob '" + input_blob + "'"};
    }
    if (!has_blob(impl_->net, output_blob)) {
        return Status{ErrorCode::kInvalidArgument, "unknown output blob '" + output_blob + "'"};
    }

    // Copy into an ncnn-owned Mat so plane layout (cstep alignment) is ncnn's
    // own choice; the caller tensor stays planar-packed and untouched.
    const ncnn::Mat in_mat(input.width, input.height, input.channels);
    const size_t input_plane_bytes = input_plane * sizeof(float);
    auto* in_base = static_cast<float*>(in_mat.data);
    for (int c = 0; c < input.channels; ++c) {
        std::memcpy(in_base + static_cast<size_t>(c) * in_mat.cstep,
                    input.data.data() + static_cast<size_t>(c) * input_plane, input_plane_bytes);
    }

    ncnn::Extractor extractor = impl_->net.create_extractor();
    if (const int ret = extractor.input(input_blob.c_str(), in_mat); ret != 0) {
        return ncnn_status(ErrorCode::kBackendFailure, "setting input blob failed", ret);
    }
    // Single forward: atomic, cannot honor cancellation mid-flight; bounds are
    // the caller's responsibility (class contract).
    ncnn::Mat out_mat;
    if (const int ret = extractor.extract(output_blob.c_str(), out_mat); ret != 0) {
        return ncnn_status(ErrorCode::kBackendFailure, "extracting output blob failed", ret);
    }

    // Copy planes back compactly (plane-major, no cstep padding gaps).
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
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled after ncnn forward"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached after ncnn forward"};
    }
    return output;
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
