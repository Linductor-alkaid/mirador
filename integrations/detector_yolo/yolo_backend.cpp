#include "yolo_backend.hpp"

#include "ncnn_runtime.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/detection_postprocess.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/letterbox.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mirador::integrations {
namespace {

Status precheck(const mirador::ImageView& image, const mirador::ExecutionContext& context) {
    if (image.format != mirador::PixelFormat::kRgb8) {
        return Status{ErrorCode::kUnsupportedFormat, "YOLO reference backend accepts kRgb8 prepared views only"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled before inference"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached before inference"};
    }
    return Status::success();
}

/// YOLO normalization: x / 255 with pad-0 letterbox (unlike PP-OCR's
/// x/127.5 - 1). Polls cancellation per channel plane.
Result<void> normalize_yolo(NcnnTensor& tensor, const mirador::ExecutionContext& context) {
    const size_t plane = static_cast<size_t>(tensor.width) * tensor.height;
    for (int c = 0; c < tensor.channels; ++c) {
        if (c % 8 == 0 && is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled while normalizing"};
        }
        float* plane_data = tensor.data.data() + static_cast<size_t>(c) * plane;
        for (size_t i = 0; i < plane; ++i) {
            plane_data[i] = plane_data[i] / 255.0F;
        }
    }
    return Result<void>{Status::success()};
}

/// Maps a model-space box back to prepared-image space through the inverse
/// letterbox transform and clips it to the image bounds.
Result<mirador::RectF> unletterbox_bounds(const mirador::RectF& bounds, const mirador::Transform2D& to_model,
                                          const mirador::ImageView& prepared) {
    const auto inverse_transform = mirador::inverse(to_model);
    if (!inverse_transform.ok()) {
        return inverse_transform.status();
    }
    const mirador::PointF top_left =
        mirador::transform_point(inverse_transform.value(), mirador::PointF{bounds.x, bounds.y});
    const mirador::PointF bottom_right = mirador::transform_point(
        inverse_transform.value(), mirador::PointF{bounds.x + bounds.width, bounds.y + bounds.height});
    mirador::RectF mapped;
    mapped.x = std::max(0.0F, std::min(top_left.x, bottom_right.x));
    mapped.y = std::max(0.0F, std::min(top_left.y, bottom_right.y));
    mapped.width = std::min(static_cast<float>(prepared.width), std::max(top_left.x, bottom_right.x)) - mapped.x;
    mapped.height = std::min(static_cast<float>(prepared.height), std::max(top_left.y, bottom_right.y)) - mapped.y;
    if (mapped.width <= 0.0F || mapped.height <= 0.0F) {
        return Status{ErrorCode::kCoordinateTransform, "detection box fell outside the prepared image"};
    }
    return mapped;
}

/// Decodes YOLOv5-layout rows into prepared-space candidates (score filter,
/// candidate budget, inverse-letterbox mapping). Split from `detect` to keep
/// both under the complexity bound.
Result<std::vector<DetectionRegion>> decode_proposals(const YoloDetectorOptions& options,
                                                      const mirador::DetectionRequest& request,
                                                      const NcnnTensor& proposals,
                                                      const mirador::LetterboxResult& letterboxed,
                                                      const mirador::ImageView& prepared,
                                                      const mirador::ExecutionContext& context) {
    const int32_t row_width = 5 + options.num_classes;
    const float* rows = proposals.data.data();
    std::vector<DetectionRegion> candidates;
    for (int32_t r = 0; r < proposals.height; ++r) {
        if (r % 512 == 0 && is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled while decoding proposals"};
        }
        const float* row = rows + static_cast<int64_t>(r) * row_width;
        float best_class = row[5];
        int32_t best_id = 0;
        for (int32_t c = 1; c < options.num_classes; ++c) {
            if (row[5 + c] > best_class) {
                best_class = row[5 + c];
                best_id = c;
            }
        }
        const float score = options.use_objectness ? row[4] * best_class : best_class;
        if (score < request.min_confidence) {
            continue;
        }
        if (static_cast<int32_t>(candidates.size()) == options.max_candidates) {
            return Status{ErrorCode::kBudgetExceeded, "proposal candidates exceed max_candidates"};
        }

        const float cx = row[0];
        const float cy = row[1];
        const float half_w = row[2] / 2.0F;
        const float half_h = row[3] / 2.0F;
        auto mapped =
            unletterbox_bounds(RectF{cx - half_w, cy - half_h, row[2], row[3]}, letterboxed.transform, prepared);
        if (!mapped.ok()) {
            continue;  // box lived entirely in padding; deterministic drop
        }
        DetectionRegion region;
        region.bounds = mapped.take_value();
        region.class_id = best_id;
        if (static_cast<size_t>(best_id) < options.class_names.size()) {
            region.label = options.class_names[static_cast<size_t>(best_id)];
        }
        region.confidence = score;
        candidates.push_back(std::move(region));
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled after decoding"};
    }
    return candidates;
}

BackendInfo make_info(const YoloModelIdentity& identity) {
    BackendInfo info;
    info.name = "yolo-ncnn-reference";
    info.implementation_version = "0.1.0";
    info.model_id = identity.model_id;
    info.model_revision = identity.model_revision;
    info.accepted_formats = {mirador::PixelFormat::kRgb8};
    info.thread_safe = false;
    return info;
}

}  // namespace

Result<YoloDetectorBackend> YoloDetectorBackend::create(const YoloDetectorOptions& options) {
    if (options.num_classes < 1) {
        return Status{ErrorCode::kInvalidArgument, "num_classes must be >= 1"};
    }
    if (options.input_side < 64 || options.input_side > 1024 || (options.input_side % 32) != 0) {
        return Status{ErrorCode::kInvalidArgument, "input_side must be a multiple of 32 in [64, 1024]"};
    }
    if (options.max_candidates < 1) {
        return Status{ErrorCode::kInvalidArgument, "max_candidates must be >= 1"};
    }
    NcnnRuntimeOptions runtime_options;
    runtime_options.param_path = options.param_path;
    runtime_options.bin_path = options.bin_path;
    runtime_options.num_threads = options.num_threads;
    auto runtime = NcnnRuntime::create(runtime_options);
    if (!runtime.ok()) {
        return runtime.status();
    }
    YoloDetectorBackend backend;
    backend.options_ = options;
    backend.runtime_ = std::move(runtime).take_value();
    backend.info_ = make_info(options.identity);
    return backend;
}

BackendInfo YoloDetectorBackend::info() const {
    return info_;
}

mirador::Result<std::vector<mirador::DetectionRegion>> YoloDetectorBackend::detect(
    const mirador::ImageView& prepared_image, const mirador::DetectionRequest& request,
    const mirador::ExecutionContext& context) {
    if (Status status = precheck(prepared_image, context); !status.ok()) {
        return status;
    }

    mirador::LetterboxRequest letterbox_request;
    letterbox_request.dst_width = options_.input_side;
    letterbox_request.dst_height = options_.input_side;
    letterbox_request.pad_value = 0;
    auto letterboxed = mirador::letterbox(prepared_image, letterbox_request, options_.work_budget_bytes);
    if (!letterboxed.ok()) {
        return letterboxed.status();
    }

    auto input_result = pack_image(letterboxed.value().buffer.view(), context);
    if (!input_result.ok()) {
        return input_result.status();
    }
    NcnnTensor input = std::move(input_result).take_value();
    if (const Result<void> normalized = normalize_yolo(input, context); !normalized.ok()) {
        return normalized.status();
    }

    auto output = runtime_.run("x", input, "out", context);
    if (!output.ok()) {
        return output.status();
    }
    const NcnnTensor& proposals = output.value();
    const int32_t row_width = 5 + options_.num_classes;
    // Frozen YOLOv5 single-tensor contract (class contract): channels 1,
    // width 5+C, height = proposal count.
    if (proposals.channels != 1 || proposals.height < 1 || proposals.width != row_width ||
        proposals.data.size() != static_cast<size_t>(proposals.height) * row_width) {
        return Status{ErrorCode::kBackendFailure,
                      "YOLO model output must be CHW with channels=1, width=5+classes, height=proposal count"};
    }

    // Decode surviving rows into prepared-space candidates. The candidate
    // budget is an explicit RULE-06 bound: overflow is a loud error, never a
    // silent drop.
    auto candidates = decode_proposals(options_, request, proposals, letterboxed.value(), prepared_image, context);
    if (!candidates.ok()) {
        return candidates.status();
    }

    return mirador::nms(candidates.value(), options_.nms);
}

}  // namespace mirador::integrations
