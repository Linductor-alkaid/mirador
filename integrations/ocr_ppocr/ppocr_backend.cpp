#include "ppocr_backend.hpp"

#include "ncnn_runtime.hpp"
#include "ppocr_internal.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/crop.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/text_postprocess.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mirador::integrations {

using ppocr_detail::make_det_canvas;
using ppocr_detail::make_info;
using ppocr_detail::normalize_ppocr;
using ppocr_detail::precheck;
using ppocr_detail::run_rec_line;
using ppocr_detail::swap_planes_to_bgr;
using ppocr_detail::unpad_bounds;

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
        return Status{ErrorCode::kBackendFailure, "det model must output a 1 x canvas_h x canvas_w probability map"};
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

    auto charset = ppocr_detail::load_charset(options.charset_path);
    if (!charset.ok()) {
        return charset.status();
    }

    PpOcrRecBackend backend;
    backend.options_ = options;
    backend.runtime_ = std::move(runtime).take_value();
    backend.charset_ = charset.take_value();
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

    auto line = run_rec_line(options_, charset_, runtime_, prepared_image, context);
    if (!line.ok()) {
        return line.status();
    }
    auto decoded = std::move(line).take_value();
    if (decoded.confidence < request.min_confidence) {
        return std::vector<TextRegion>{};
    }

    TextRegion region;
    region.bounds =
        RectF{0.0F, 0.0F, static_cast<float>(prepared_image.width), static_cast<float>(prepared_image.height)};
    region.utf8_text = std::move(decoded.utf8_text);
    region.confidence = decoded.confidence;
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
