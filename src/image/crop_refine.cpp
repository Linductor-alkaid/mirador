#include <mirador/crop_refine.hpp>

#include <mirador/backend_info.hpp>
#include <mirador/color_convert.hpp>
#include <mirador/crop.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/transform.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace mirador {
namespace {

bool is_valid(const CropRefineParams& params) noexcept {
    if (!(params.expand_ratio >= 0.0F) || params.expand_ratio > 4.0F || !std::isfinite(params.expand_ratio)) {
        return false;
    }
    if (params.refine_target_side < 0 || params.refine_target_side > 4096) {
        return false;
    }
    if (!(params.refine_confidence_below >= 0.0) || params.refine_confidence_below > 1.0 ||
        !std::isfinite(params.refine_confidence_below) || !(params.small_box_area_ratio >= 0.0) ||
        params.small_box_area_ratio > 1.0 || !std::isfinite(params.small_box_area_ratio)) {
        return false;
    }
    if (params.max_refine_candidates < 0 || !(params.min_refined_confidence >= 0.0F) ||
        params.min_refined_confidence >= 1.0F || !std::isfinite(params.min_refined_confidence)) {
        return false;
    }
    if (params.per_crop_budget_bytes < 1024) {
        return false;
    }
    return true;
}

struct CandidateRef {
    size_t index = 0;
    float confidence = 0.0F;
};

RectI crop_roi(const RectF& box, float expand_ratio, int32_t source_width, int32_t source_height) noexcept {
    const double expand_x = static_cast<double>(expand_ratio) * box.width;
    const double expand_y = static_cast<double>(expand_ratio) * box.height;
    const double left = std::max(0.0, static_cast<double>(box.x) - expand_x);
    const double top = std::max(0.0, static_cast<double>(box.y) - expand_y);
    const double right = std::min<double>(source_width, box.x + box.width + expand_x);
    const double bottom = std::min<double>(source_height, box.y + box.height + expand_y);
    const int32_t x0 = std::min(static_cast<int32_t>(std::floor(left)), source_width - 1);
    const int32_t y0 = std::min(static_cast<int32_t>(std::floor(top)), source_height - 1);
    const int32_t x1 = std::max(x0 + 1, std::min(static_cast<int32_t>(std::ceil(right)), source_width));
    const int32_t y1 = std::max(y0 + 1, std::min(static_cast<int32_t>(std::ceil(bottom)), source_height));
    return RectI{x0, y0, x1 - x0, y1 - y0};
}

/// A same-format copy through ImageBuffer so every candidate carries an owning
/// buffer with one uniform treatment (keeps lifetimes simple in the combinator).
[[nodiscard]] Result<ImageBuffer> copy_to_buffer(const ImageView& view, int64_t budget) noexcept {
    return convert_color(view, view.format, budget);
}

Result<ImageBuffer> prepare_crop(const ImageView& cropped, const CropRefineParams& params) noexcept {
    if (params.refine_target_side == 0) {
        return copy_to_buffer(cropped, params.per_crop_budget_bytes);
    }
    const int32_t long_side = std::max(cropped.width, cropped.height);
    if (long_side == params.refine_target_side) {
        return copy_to_buffer(cropped, params.per_crop_budget_bytes);
    }
    const double scale = static_cast<double>(params.refine_target_side) / static_cast<double>(long_side);
    const int32_t dst_width =
        std::clamp(static_cast<int32_t>(std::floor(cropped.width * scale + 0.5)), 1, kMaxImageDimension);
    const int32_t dst_height =
        std::clamp(static_cast<int32_t>(std::floor(cropped.height * scale + 0.5)), 1, kMaxImageDimension);
    return resize_area(cropped, dst_width, dst_height, params.per_crop_budget_bytes);
}

}  // namespace

Result<std::vector<DetectionRegion>> refine_small_detections(const ImageView& source, DetectorBackend* backend,
                                                             std::span<const DetectionRegion> initial,
                                                             const CropRefineParams& params,
                                                             const ExecutionContext& context) {
    if (!is_valid(params)) {
        return Status(ErrorCode::kInvalidArgument, "crop-refine parameters out of range");
    }
    if (!validate(source).ok()) {
        return Status(ErrorCode::kInvalidArgument, "invalid source view");
    }
    if (source.format == PixelFormat::kNv12) {
        return Status(ErrorCode::kUnsupportedFormat, "crop-refine does not support NV12 yet (chroma loss)");
    }
    if (backend == nullptr) {
        return Status(ErrorCode::kBackendUnavailable, "no detector backend provided");
    }
    const BackendInfo info = backend->info();
    if (!validate(info).ok()) {
        return Status(ErrorCode::kBackendUnavailable, "detector backend reported invalid capabilities");
    }
    if (is_cancelled(context)) {
        return Status(ErrorCode::kCancelled, "cancelled before refinement");
    }
    if (deadline_reached(context)) {
        return Status(ErrorCode::kTimeout, "deadline reached before refinement");
    }

    // Select candidates: below the confidence bar or below the area bar.
    const double source_area = static_cast<double>(source.width) * source.height;
    std::vector<CandidateRef> candidates;
    for (size_t i = 0; i < initial.size(); ++i) {
        const DetectionRegion& region = initial[i];
        const double area = static_cast<double>(region.bounds.width) * region.bounds.height;
        const bool low_confidence = static_cast<double>(region.confidence) < params.refine_confidence_below;
        const bool small = params.small_box_area_ratio > 0.0 && area / source_area < params.small_box_area_ratio;
        if (low_confidence || small) {
            candidates.push_back(CandidateRef{i, region.confidence});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const CandidateRef& a, const CandidateRef& b) {
        if (a.confidence != b.confidence) {
            return a.confidence < b.confidence;
        }
        return a.index < b.index;
    });
    if (candidates.size() > static_cast<size_t>(params.max_refine_candidates)) {
        candidates.resize(static_cast<size_t>(params.max_refine_candidates));  // explicit deterministic drop
    }

    std::vector<DetectionRegion> output(initial.begin(), initial.end());
    DetectionRequest request;
    request.min_confidence = params.backend_min_confidence;
    request.backend_params = params.backend_params;

    for (const CandidateRef candidate : candidates) {
        if (is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "cancelled during refinement");
        }
        if (deadline_reached(context)) {
            return Status(ErrorCode::kTimeout, "deadline reached during refinement");
        }
        const RectI roi = crop_roi(initial[candidate.index].bounds, params.expand_ratio, source.width, source.height);
        auto cropped_result = crop(source, roi, params.per_crop_budget_bytes);
        if (!cropped_result.ok()) {
            return Status(cropped_result.status().code(),
                          std::string("crop-refine crop failed: ") + cropped_result.status().message());
        }
        const ImageView cropped_view = cropped_result.value().view();

        // Format gating: convert to the first backend-accepted format.
        ImageView prepared = cropped_view;
        std::optional<ImageBuffer> converted;
        if (std::find(info.accepted_formats.begin(), info.accepted_formats.end(), cropped_view.format) ==
            info.accepted_formats.end()) {
            bool reachable = false;
            for (const PixelFormat accepted : info.accepted_formats) {
                auto converted_buffer = convert_color(cropped_view, accepted, params.per_crop_budget_bytes);
                if (converted_buffer.ok()) {
                    converted.emplace(converted_buffer.take_value());
                    prepared = converted->view();
                    reachable = true;
                    break;
                }
            }
            if (!reachable) {
                return Status(ErrorCode::kUnsupportedFormat, "no backend-accepted format reachable from the crop");
            }
        }

        auto prepared_buffer = prepare_crop(prepared, params);
        if (!prepared_buffer.ok()) {
            return Status(prepared_buffer.status().code(),
                          std::string("crop-refine prepare failed: ") + prepared_buffer.status().message());
        }
        const ImageView model_view = prepared_buffer.value().view();

        // Exact inverse of crop -> (convert) -> resize, applied to model-space boxes.
        const auto to_cropped = make_crop(RectF{static_cast<float>(roi.x), static_cast<float>(roi.y),
                                                static_cast<float>(roi.width), static_cast<float>(roi.height)},
                                          CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
        const auto to_model = make_scale(static_cast<double>(model_view.width) / static_cast<double>(roi.width),
                                         static_cast<double>(model_view.height) / static_cast<double>(roi.height),
                                         CoordinateSpaceId::kCropped, CoordinateSpaceId::kModelInput);
        auto forward = compose(to_cropped, to_model);
        if (!forward.ok()) {
            return Status(forward.status().code(),
                          std::string("crop-refine transform failed: ") + forward.status().message());
        }
        auto backward = inverse(forward.value());
        if (!backward.ok()) {
            return Status(backward.status().code(),
                          std::string("crop-refine inverse failed: ") + backward.status().message());
        }

        auto detected = backend->detect(model_view, request, context);
        if (!detected.ok()) {
            return Status(detected.status().code(),
                          std::string("crop-refine backend failed: ") + detected.status().message());
        }

        // Replace the candidate slot with the recovered refined results.
        std::vector<DetectionRegion> refined;
        for (const DetectionRegion& found : detected.value()) {
            if (found.confidence < params.min_refined_confidence) {
                continue;
            }
            DetectionRegion recovered = found;
            recovered.bounds = transform_rect(backward.value(), found.bounds);
            refined.push_back(std::move(recovered));
        }

        std::vector<DetectionRegion> merged;
        for (size_t i = 0; i < output.size(); ++i) {
            if (i == candidate.index) {
                for (const DetectionRegion& region : refined) {
                    merged.push_back(region);
                }
            } else {
                merged.push_back(output[i]);
            }
        }
        output = std::move(merged);
    }
    return output;
}

}  // namespace mirador
