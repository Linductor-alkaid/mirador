#include "ppocr_internal.hpp"

#include "ctc_decode.hpp"
#include "ncnn_runtime.hpp"
#include "ppocr_backend.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace mirador::integrations::ppocr_detail {

Status precheck(const ImageView& image, const ExecutionContext& context) {
    if (image.format != PixelFormat::kRgb8) {
        return Status{ErrorCode::kUnsupportedFormat, "PP-OCR reference backends accept kRgb8 prepared views only"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled before inference"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached before inference"};
    }
    return Status::success();
}

Result<void> normalize_ppocr(NcnnTensor& tensor, const ExecutionContext& context) {
    const size_t plane = static_cast<size_t>(tensor.width) * tensor.height;
    for (int c = 0; c < tensor.channels; ++c) {
        if (c % 8 == 0 && is_cancelled(context)) {
            return Result<void>{Status{ErrorCode::kCancelled, "cancelled while normalizing"}};
        }
        float* plane_data = tensor.data.data() + static_cast<size_t>(c) * plane;
        for (size_t i = 0; i < plane; ++i) {
            plane_data[i] = plane_data[i] / 127.5F - 1.0F;
        }
    }
    return Result<void>{Status::success()};
}

Result<void> swap_planes_to_bgr(NcnnTensor& tensor) {
    if (tensor.channels != 3 || tensor.width * tensor.height == 0) {
        return Result<void>{Status::success()};
    }
    const size_t plane = static_cast<size_t>(tensor.width) * tensor.height;
    float* r = tensor.data.data();
    float* b = tensor.data.data() + 2 * plane;
    for (size_t i = 0; i < plane; ++i) {
        std::swap(r[i], b[i]);
    }
    return Result<void>{Status::success()};
}

Result<DetCanvas> make_det_canvas(const ImageView& image, int32_t limit, int64_t budget) {
    const int32_t longer_side = std::max(image.width, image.height);
    DetCanvas canvas;
    canvas.scale = longer_side > limit ? static_cast<float>(limit) / static_cast<float>(longer_side) : 1.0F;
    const auto resized_w =
        std::max(1, static_cast<int32_t>(std::lround(static_cast<double>(image.width) * canvas.scale)));
    const auto resized_h =
        std::max(1, static_cast<int32_t>(std::lround(static_cast<double>(image.height) * canvas.scale)));
    const auto canvas_w = static_cast<int32_t>((static_cast<int64_t>(resized_w) + 31) / 32 * 32);
    const auto canvas_h = static_cast<int32_t>((static_cast<int64_t>(resized_h) + 31) / 32 * 32);

    auto resized = resize_area(image, resized_w, resized_h, budget);
    if (!resized.ok()) {
        return resized.status();
    }
    auto created = ImageBuffer::create(PixelFormat::kRgb8, canvas_w, canvas_h, budget);
    if (!created.ok()) {
        return created.status();
    }
    canvas.buffer = created.take_value();
    const int64_t canvas_stride = canvas.buffer.row_stride_bytes();
    std::byte* canvas_data = canvas.buffer.data();
    for (int32_t y = 0; y < canvas_h; ++y) {
        std::memset(canvas_data + static_cast<int64_t>(y) * canvas_stride, kDetPadValue,
                    static_cast<size_t>(canvas_stride));
    }
    canvas.pad_left = (canvas_w - resized_w) / 2;
    canvas.pad_top = (canvas_h - resized_h) / 2;
    const ImageView resized_view = resized.value().view();
    for (int32_t y = 0; y < resized_h; ++y) {
        std::memcpy(canvas_data + static_cast<int64_t>(canvas.pad_top + y) * canvas_stride +
                        static_cast<int64_t>(canvas.pad_left) * 3,
                    resized_view.data + static_cast<int64_t>(y) * resized_view.row_stride_bytes,
                    static_cast<size_t>(resized_w) * 3);
    }
    return canvas;
}

Result<RectF> unpad_bounds(const RectF& bounds, float scale, int32_t pad_left, int32_t pad_top,
                           const ImageView& prepared) {
    const float left = (bounds.x - static_cast<float>(pad_left)) / scale;
    const float top = (bounds.y - static_cast<float>(pad_top)) / scale;
    const float right = (bounds.x + bounds.width - static_cast<float>(pad_left)) / scale;
    const float bottom = (bounds.y + bounds.height - static_cast<float>(pad_top)) / scale;
    RectF mapped;
    mapped.x = std::max(0.0F, std::min(left, right));
    mapped.y = std::max(0.0F, std::min(top, bottom));
    mapped.width = std::min(static_cast<float>(prepared.width), std::max(left, right)) - mapped.x;
    mapped.height = std::min(static_cast<float>(prepared.height), std::max(top, bottom)) - mapped.y;
    if (mapped.width <= 0.0F || mapped.height <= 0.0F) {
        return Status{ErrorCode::kCoordinateTransform, "det box fell outside the prepared image"};
    }
    return mapped;
}

BackendInfo make_info(const char* name, const PpOcrModelIdentity& identity) {
    BackendInfo info;
    info.name = name;
    info.implementation_version = "0.1.0";
    info.model_id = identity.model_id;
    info.model_revision = identity.model_revision;
    info.accepted_formats = {PixelFormat::kRgb8};
    info.thread_safe = false;
    return info;
}

Result<std::vector<std::string>> load_charset(const std::string& charset_path) {
    std::vector<std::string> charset;
    std::ifstream dictionary(charset_path);
    if (!dictionary.is_open()) {
        return Status{ErrorCode::kBackendUnavailable, "cannot open charset file"};
    }
    std::string entry;
    while (std::getline(dictionary, entry)) {
        if (!entry.empty() && entry.back() == '\r') {
            entry.pop_back();
        }
        if (entry.empty()) {
            continue;
        }
        if (charset.size() >= kMaxCharsetEntries) {
            return Status{ErrorCode::kBudgetExceeded, "charset file exceeds the entry budget"};
        }
        charset.push_back(entry);
    }
    if (charset.empty()) {
        return Status{ErrorCode::kBackendUnavailable, "charset file is empty"};
    }
    return charset;
}

Result<RecLineOutput> run_rec_line(const PpOcrRecOptions& options, const std::vector<std::string>& charset,
                                   const NcnnRuntime& runtime, const ImageView& line_image,
                                   const ExecutionContext& context, RecUnitTiming* timing) {
    // Stage split for the optional timing diagnostics: `stop()` ends one
    // phase; the destructor stops whatever is left, so error paths stay
    // accounted too.
    class UnitPhase {
    public:
        UnitPhase(RecUnitTiming* timing, int64_t RecUnitTiming::*slot) noexcept
            : timing_(timing), slot_(slot), begin_(std::chrono::steady_clock::now()) {}

        void stop() noexcept {
            if (timing_ != nullptr) {
                *timing_.*slot_ +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin_)
                        .count();
                timing_ = nullptr;
            }
        }
        ~UnitPhase() noexcept { stop(); }

        UnitPhase(const UnitPhase&) = delete;
        UnitPhase& operator=(const UnitPhase&) = delete;

    private:
        RecUnitTiming* timing_;
        int64_t RecUnitTiming::*slot_;
        std::chrono::steady_clock::time_point begin_;
    };
    UnitPhase preprocess_phase{timing, &RecUnitTiming::preprocess_ns};

    // Reference rec preprocessing: resize the line to rec_height with the
    // aspect ratio preserved (no padding — the model is fully convolutional
    // and CTC drops the implied trailing blanks); rec_width caps the line
    // width for budget bounds (ppocrv5.cpp `get_rotate_crop_image`).
    const int64_t scaled_width = static_cast<int64_t>(line_image.width) * static_cast<int64_t>(options.rec_height);
    const auto line_width = std::clamp<int32_t>(
        static_cast<int32_t>(std::lround(static_cast<double>(scaled_width) / line_image.height)), 1, options.rec_width);
    auto resized = resize_area(line_image, line_width, options.rec_height, options.work_budget_bytes);
    if (!resized.ok()) {
        return resized.status();
    }
    auto input_result = pack_image(resized.value().view(), context);
    if (!input_result.ok()) {
        return input_result.status();
    }
    NcnnTensor input = std::move(input_result).take_value();
    if (const Result<void> swapped = swap_planes_to_bgr(input); !swapped.ok()) {
        return swapped.status();
    }
    if (const Result<void> normalized = normalize_ppocr(input, context); !normalized.ok()) {
        return normalized.status();
    }
    preprocess_phase.stop();

    UnitPhase forward_phase{timing, &RecUnitTiming::forward_ns};
    auto output = runtime.run("x", input, "out", context);
    if (!output.ok()) {
        return output.status();
    }
    forward_phase.stop();

    UnitPhase decode_phase{timing, &RecUnitTiming::decode_ns};
    // Frozen output contract: channels = time steps, width = class count,
    // height 1 (planar CHW). Conversions with raw outputs in another layout
    // must reshape inside their param files (class contract).
    const NcnnTensor& scores = output.value();
    if (scores.height != 1 || scores.width < 2 || scores.channels < 1 ||
        scores.data.size() != static_cast<size_t>(scores.channels) * scores.width) {
        return Status{ErrorCode::kBackendFailure,
                      "rec model output must be CHW with channels=time steps and width=class count"};
    }

    auto line = ctc_decode_greedy(scores.data.data(), scores.channels, scores.width, kRecBlankClass);
    if (!line.ok()) {
        return line.status();
    }
    RecLineOutput decoded;
    for (const int32_t class_index : line.value().class_indices) {
        if (class_index < 1 || static_cast<size_t>(class_index) > charset.size()) {
            return Status{ErrorCode::kBackendFailure, "model emitted a class outside the charset"};
        }
        decoded.utf8_text += charset[static_cast<size_t>(class_index) - 1];
    }
    decoded.confidence = line.value().confidence;
    decode_phase.stop();
    return decoded;
}

}  // namespace mirador::integrations::ppocr_detail
