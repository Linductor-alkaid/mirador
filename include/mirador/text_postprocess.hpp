#ifndef MIRADOR_TEXT_POSTPROCESS_HPP
#define MIRADOR_TEXT_POSTPROCESS_HPP

#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <vector>

namespace mirador {

/// DB (Differentiable Binarization) post-processing, AABB reference adaptation
/// (design section 13, DEC-014): binarizes a text-probability map, groups the
/// 8-connected foreground pixels into components and emits one axis-aligned
/// box per component, expanded by the DB unclip offset
/// `offset = area * unclip_ratio / perimeter` per side (clamped to the map
/// bounds). Components smaller than `min_box_pixels` or with a mean foreground
/// value below `min_mean_score` (0-1 scale of the byte value) are dropped.
/// Output is in map pixel space, ordered by the deterministic scan (top row,
/// then left column of each component). The input is never modified (RULE-04).
///
/// Explicitly out of scope until the first real backend needs it (DEC-014):
/// rotated quadrilaterals, polygon output, per-character results —
/// `utf8_text` stays empty and `polygon` stays empty in this reference.
///
/// Errors: kInvalidArgument (invalid view, out-of-range parameters),
/// kUnsupportedFormat (non-gray view), kBudgetExceeded (more than
/// `max_boxes` components, or the visited bitmap exceeding the work budget).
/// Never throws.
struct DbPostprocessParams {
    /// Foreground: probability byte >= threshold, in [1, 255].
    uint8_t binarize_threshold = 128;
    /// DB unclip expansion ratio (per-side offset = area * ratio / perimeter).
    /// In [0, 10].
    double unclip_ratio = 1.5;
    /// Components with fewer foreground pixels are dropped. >= 0.
    int32_t min_box_pixels = 4;
    /// Components whose mean foreground value (0-1 scale) is below this are
    /// dropped. In [0, 1].
    double min_mean_score = 0.5;
    /// Maximum number of returned boxes; scanning past the cap fails with
    /// kBudgetExceeded. In [1, 65535].
    int32_t max_boxes = 256;
    /// Budget for the visited bitmap (one bit per map pixel). >= 1024.
    int64_t work_budget_bytes = int64_t{4} * 1024 * 1024;
};

[[nodiscard]] Result<std::vector<TextRegion>> db_postprocess_aabb(const ImageView& probability_map,
                                                                  const DbPostprocessParams& params) noexcept;

/// Contour box recovery (design section 13, DEC-014): the axis-aligned bounding
/// box of every 8-connected component of a binary map (pixels with
/// `value >= foreground_threshold`). No score, no expansion — the exact pixel
/// bounds, in map pixel space, deterministic scan order.
///
/// Errors: as `db_postprocess_aabb` (kBudgetExceeded also for more than
/// `max_boxes` components). Never throws.
struct ContourBoxParams {
    /// Foreground: byte >= threshold, in [1, 255].
    uint8_t foreground_threshold = 1;
    /// Components with fewer foreground pixels are dropped. >= 0.
    int32_t min_box_pixels = 1;
    /// Maximum number of returned boxes. In [1, 65535].
    int32_t max_boxes = 1024;
    /// Budget for the visited bitmap (one bit per map pixel). >= 1024.
    int64_t work_budget_bytes = int64_t{4} * 1024 * 1024;
};

[[nodiscard]] Result<std::vector<RectF>> recover_contour_boxes(const ImageView& binary,
                                                               const ContourBoxParams& params) noexcept;

}  // namespace mirador

#endif  // MIRADOR_TEXT_POSTPROCESS_HPP
