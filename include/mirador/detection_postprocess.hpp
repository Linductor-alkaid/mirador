#ifndef MIRADOR_DETECTION_POSTPROCESS_HPP
#define MIRADOR_DETECTION_POSTPROCESS_HPP

#include <mirador/detector_backend.hpp>
#include <mirador/geometry.hpp>
#include <mirador/result.hpp>

#include <span>
#include <string>
#include <vector>

namespace mirador {

/// Intersection-over-union of two rectangles in pixel-area coordinates
/// (double arithmetic). Degenerate rectangles (non-positive area) produce 0.
[[nodiscard]] double intersection_over_union(const RectF& first, const RectF& second) noexcept;

/// Greedy NMS over detection proposals (design section 14, M3-05 reference
/// component). Candidates are visited by descending confidence (ties broken by
/// input index); a candidate is suppressed when it overlaps a kept proposal
/// with `intersection_over_union` strictly greater than `iou_threshold` and,
/// with `class_aware`, the same `class_id`. Output preserves the visited
/// (score-descending) order and never modifies the input (RULE-04).
///
/// `max_output` caps the number of kept proposals; the surplus is dropped
/// silently only because the cap is an explicit caller bound (RULE-06: use a
/// bounded input or raise the cap when you need the rest).
///
/// Errors: kInvalidArgument for `iou_threshold` outside [0, 1], negative
/// `max_output`, or NaN/inf coordinates and confidences in the input. Never
/// throws.
struct NmsParams {
    float iou_threshold = 0.5F;
    bool class_aware = true;
    int32_t max_output = 0;  ///< 0 = keep everything that survives suppression
};

[[nodiscard]] Result<std::vector<DetectionRegion>> nms(std::span<const DetectionRegion> regions,
                                                       const NmsParams& params);

/// Inclusive confidence/category filter for detection proposals (design
/// section 14). A proposal is kept when
/// - `confidence >= min_confidence` (always active; 0 keeps everything),
/// - `class_ids` is empty or contains the proposal's `class_id`, and
/// - `labels` is empty or contains the proposal's `label` exactly.
/// Output preserves input order. Errors: kInvalidArgument for a negative
/// `min_confidence` or NaN/inf inputs. Never throws.
struct DetectionFilterParams {
    float min_confidence = 0.0F;
    std::vector<int32_t> class_ids;
    std::vector<std::string> labels;
};

[[nodiscard]] Result<std::vector<DetectionRegion>> filter_detections(std::span<const DetectionRegion> regions,
                                                                     const DetectionFilterParams& params);

}  // namespace mirador

#endif  // MIRADOR_DETECTION_POSTPROCESS_HPP
