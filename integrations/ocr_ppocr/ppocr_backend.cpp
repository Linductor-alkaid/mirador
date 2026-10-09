#include "ppocr_backend.hpp"

#include "ctc_decode.hpp"
#include "ncnn_runtime.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/crop.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/text_postprocess.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace mirador::integrations {
namespace {

constexpr int32_t kRecBlankClass = 0;
constexpr size_t kMaxCharsetEntries = 65536;
/// Border value of the reference detection canvas (ppocrv5.cpp pads the /32
/// border with the YOLO-style constant 114 before ImageNet normalization).
constexpr std::uint8_t kDetPadValue = 114;

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

/// CHW float tensor in, planar-normalized in place: v / 127.5 - 1 (the
/// PP-OCR convention for pad-0 letterboxed inputs). Polls cancellation.
Result<void> normalize_ppocr(NcnnTensor& tensor, const ExecutionContext& context) {
    const size_t plane = static_cast<size_t>(tensor.width) * tensor.height;
    for (int c = 0; c < tensor.channels; ++c) {
        if (c % 8 == 0 && is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled while normalizing"};
        }
        float* plane_data = tensor.data.data() + static_cast<size_t>(c) * plane;
        for (size_t i = 0; i < plane; ++i) {
            plane_data[i] = plane_data[i] / 127.5F - 1.0F;
        }
    }
    return Result<void>{Status::success()};
}

/// Swaps the packed RGB planes to BGR: the reference deployment (and the
/// PaddleOCR export behind it) consumes cv BGR planes.
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

/// Maps a canvas-space box back to prepared-image space through the inverse
/// pad/scale transform of the detection canvas and clips it to the image.
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

/// Builds the reference detection canvas: the prepared image is never
/// upscaled, resized so its longer side fits `limit` (exact area weights),
/// and copied into a /32-aligned canvas whose border is filled with 114
/// (ppocrv5.cpp `detect`: pad to the stride multiple, near-centered split).
struct DetCanvas {
    ImageBuffer buffer;
    float scale = 1.0F;  // image -> canvas
    int32_t pad_left = 0;
    int32_t pad_top = 0;
};

Result<DetCanvas> make_det_canvas(const ImageView& image, int32_t limit, int64_t budget) {
    const int32_t longer_side = std::max(image.width, image.height);
    DetCanvas canvas;
    canvas.scale = longer_side > limit ? static_cast<float>(limit) / static_cast<float>(longer_side) : 1.0F;
    const auto resized_w = std::max(1, static_cast<int32_t>(std::lround(static_cast<double>(image.width) * canvas.scale)));
    const auto resized_h = std::max(1, static_cast<int32_t>(std::lround(static_cast<double>(image.height) * canvas.scale)));
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

}  // namespace

Result<PpOcrDetBackend> PpOcrDetBackend::create(const PpOcrDetOptions& options) {
    if (options.det_side < 32 || options.det_side > 4096 || (options.det_side % 32) != 0) {
        return Status{ErrorCode::kInvalidArgument, "det_side must be a multiple of 32 in [32, 4096]"};
    }
    NcnnRuntimeOptions runtime_options;
    runtime_options.param_path = options.param_path;
    runtime_options.bin_path = options.bin_path;
    runtime_options.num_threads = options.num_threads;
    auto runtime = NcnnRuntime::create(runtime_options);
    if (!runtime.ok()) {
        return runtime.status();
    }
    PpOcrDetBackend backend;
    backend.options_ = options;
    backend.runtime_ = std::move(runtime).take_value();
    backend.info_ = make_info("ppocr-det-ncnn-reference", options.identity);
    return backend;
}

BackendInfo PpOcrDetBackend::info() const {
    return info_;
}

Result<std::vector<TextRegion>> PpOcrDetBackend::recognize(const ImageView& prepared_image, const OcrRequest& request,
                                                           const ExecutionContext& context) {
    if (Status status = precheck(prepared_image, context); !status.ok()) {
        return status;
    }

    auto canvas = make_det_canvas(prepared_image, options_.det_side, options_.work_budget_bytes);
    if (!canvas.ok()) {
        return canvas.status();
    }
    const int32_t canvas_w = canvas.value().buffer.width();
    const int32_t canvas_h = canvas.value().buffer.height();

    auto input_result = pack_image(canvas.value().buffer.view(), context);
    if (!input_result.ok()) {
        return input_result.status();
    }
    NcnnTensor input = std::move(input_result).take_value();
    // The reference canvas normalizes to x/127.5 - 1 (any exported ImageNet
    // pair lives inside the converted graph); planes go out as BGR.
    if (const Result<void> swapped = swap_planes_to_bgr(input); !swapped.ok()) {
        return swapped.status();
    }
    if (const Result<void> normalized = normalize_ppocr(input, context); !normalized.ok()) {
        return normalized.status();
    }

    auto output = runtime_.run("x", input, "out", context);
    if (!output.ok()) {
        return output.status();
    }
    const NcnnTensor& probability = output.value();
    if (probability.channels != 1 || probability.width != canvas_w || probability.height != canvas_h) {
        return Status{ErrorCode::kBackendFailure,
                      "det model must output a 1 x canvas_h x canvas_w probability map"};
    }

    // Probability floats -> Gray8 bytes for the M3 DB postprocess chain.
    const size_t plane = static_cast<size_t>(probability.width) * probability.height;
    std::vector<uint8_t> probability_bytes(plane);
    for (size_t i = 0; i < plane; ++i) {
        const float value = probability.data[i];
        const auto scaled = static_cast<int>(std::lround(value * 255.0F));
        probability_bytes[i] = static_cast<uint8_t>(std::clamp(scaled, 0, 255));
    }
    ImageView probability_view;
    probability_view.data = reinterpret_cast<const std::byte*>(probability_bytes.data());
    probability_view.width = probability.width;
    probability_view.height = probability.height;
    probability_view.row_stride_bytes = probability.width;
    probability_view.format = PixelFormat::kGray8;

    auto boxes = db_postprocess_aabb(probability_view, options_.postprocess);
    if (!boxes.ok()) {
        return boxes.status();
    }

    std::vector<TextRegion> regions;
    regions.reserve(boxes.value().size());
    for (const TextRegion& box : boxes.value()) {
        if (box.confidence < request.min_confidence) {
            continue;
        }
        auto mapped = unpad_bounds(box.bounds, canvas.value().scale, canvas.value().pad_left, canvas.value().pad_top,
                                   prepared_image);
        if (!mapped.ok()) {
            continue;  // box lived entirely in padding; deterministic drop
        }
        TextRegion region;
        region.bounds = mapped.take_value();
        region.confidence = box.confidence;
        regions.push_back(std::move(region));
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled after det inference"};
    }
    return regions;
}

Result<PpOcrRecBackend> PpOcrRecBackend::create(const PpOcrRecOptions& options) {
    if (options.rec_height < 16 || options.rec_height > 128 || options.rec_width < 32 || options.rec_width > 1024) {
        return Status{ErrorCode::kInvalidArgument, "rec input size out of range"};
    }
    NcnnRuntimeOptions runtime_options;
    runtime_options.param_path = options.param_path;
    runtime_options.bin_path = options.bin_path;
    runtime_options.num_threads = options.num_threads;
    auto runtime = NcnnRuntime::create(runtime_options);
    if (!runtime.ok()) {
        return runtime.status();
    }

    std::vector<std::string> charset;
    std::ifstream dictionary(options.charset_path);
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

    PpOcrRecBackend backend;
    backend.options_ = options;
    backend.runtime_ = std::move(runtime).take_value();
    backend.charset_ = std::move(charset);
    backend.info_ = make_info("ppocr-rec-ncnn-reference", options.identity);
    return backend;
}

BackendInfo PpOcrRecBackend::info() const {
    return info_;
}

Result<std::vector<TextRegion>> PpOcrRecBackend::recognize(const ImageView& prepared_image, const OcrRequest& request,
                                                           const ExecutionContext& context) {
    if (Status status = precheck(prepared_image, context); !status.ok()) {
        return status;
    }

    // Reference rec preprocessing: resize the line to rec_height with the
    // aspect ratio preserved (no padding — the model is fully convolutional
    // and CTC drops the implied trailing blanks); rec_width caps the line
    // width for budget bounds (ppocrv5.cpp `get_rotate_crop_image`).
    const int64_t scaled_width =
        static_cast<int64_t>(prepared_image.width) * static_cast<int64_t>(options_.rec_height);
    const auto line_width = std::clamp<int32_t>(
        static_cast<int32_t>(std::lround(static_cast<double>(scaled_width) / prepared_image.height)), 1,
        options_.rec_width);
    auto line_image = resize_area(prepared_image, line_width, options_.rec_height, options_.work_budget_bytes);
    if (!line_image.ok()) {
        return line_image.status();
    }
    auto input_result = pack_image(line_image.value().view(), context);
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
    auto output = runtime_.run("x", input, "out", context);
    if (!output.ok()) {
        return output.status();
    }

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
    std::string text;
    for (const int32_t class_index : line.value().class_indices) {
        if (class_index < 1 || static_cast<size_t>(class_index) > charset_.size()) {
            return Status{ErrorCode::kBackendFailure, "model emitted a class outside the charset"};
        }
        text += charset_[static_cast<size_t>(class_index) - 1];
    }
    if (line.value().confidence < request.min_confidence) {
        return std::vector<TextRegion>{};
    }

    TextRegion region;
    region.bounds =
        RectF{0.0F, 0.0F, static_cast<float>(prepared_image.width), static_cast<float>(prepared_image.height)};
    region.utf8_text = std::move(text);
    region.confidence = line.value().confidence;
    return std::vector<TextRegion>{std::move(region)};
}

Result<PpOcrBackend> PpOcrBackend::create(const PpOcrDetOptions& det_options, const PpOcrRecOptions& rec_options) {
    auto det = PpOcrDetBackend::create(det_options);
    if (!det.ok()) {
        return det.status();
    }
    auto rec = PpOcrRecBackend::create(rec_options);
    if (!rec.ok()) {
        return rec.status();
    }
    return PpOcrBackend{std::move(det).take_value(), std::move(rec).take_value()};
}

PpOcrBackend::PpOcrBackend(PpOcrDetBackend det, PpOcrRecBackend rec) noexcept
    : det_(std::move(det)), rec_(std::move(rec)) {
    info_ = make_info("ppocr-ncnn-reference", PpOcrModelIdentity{});
    info_.model_id = det_.info().model_id + "+" + rec_.info().model_id;
    info_.model_revision = det_.info().model_revision + "+" + rec_.info().model_revision;
}

BackendInfo PpOcrBackend::info() const {
    return info_;
}

Result<std::vector<TextRegion>> PpOcrBackend::recognize(const ImageView& prepared_image, const OcrRequest& request,
                                                        const ExecutionContext& context) {
    auto boxes = det_.recognize(prepared_image, request, context);
    if (!boxes.ok()) {
        return boxes.status();
    }

    // Deterministic reading order: top-to-bottom, then left-to-right.
    std::vector<TextRegion> ordered = std::move(boxes).take_value();
    std::sort(ordered.begin(), ordered.end(), [](const TextRegion& lhs, const TextRegion& rhs) {
        if (lhs.bounds.y != rhs.bounds.y) {
            return lhs.bounds.y < rhs.bounds.y;
        }
        return lhs.bounds.x < rhs.bounds.x;
    });

    const int64_t crop_budget = rec_.work_budget_bytes();
    for (TextRegion& box : ordered) {
        const auto crop_x = static_cast<int32_t>(std::lround(box.bounds.x));
        const auto crop_y = static_cast<int32_t>(std::lround(box.bounds.y));
        const auto crop_w = static_cast<int32_t>(std::lround(box.bounds.width));
        const auto crop_h = static_cast<int32_t>(std::lround(box.bounds.height));
        RectI roi;
        roi.x = std::clamp(crop_x, 0, static_cast<int32_t>(prepared_image.width) - 1);
        roi.y = std::clamp(crop_y, 0, static_cast<int32_t>(prepared_image.height) - 1);
        roi.width = std::clamp(crop_w, 1, static_cast<int32_t>(prepared_image.width) - roi.x);
        roi.height = std::clamp(crop_h, 1, static_cast<int32_t>(prepared_image.height) - roi.y);

        auto line_image = crop(prepared_image, roi, crop_budget);
        if (!line_image.ok()) {
            return line_image.status();
        }
        auto lines = rec_.recognize(line_image.value().view(), request, context);
        if (!lines.ok()) {
            return lines.status();
        }
        if (!lines.value().empty()) {
            box.utf8_text = lines.value().front().utf8_text;
            // Combined confidence: box score gates detection, line score the
            // reading; the product keeps both signals observable.
            box.confidence = box.confidence * lines.value().front().confidence;
        }
        if (is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled during rec stage"};
        }
    }
    return ordered;
}

}  // namespace mirador::integrations
