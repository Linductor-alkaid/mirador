#ifndef MIRADOR_IMAGE_CONNECTED_COMPONENTS_H_
#define MIRADOR_IMAGE_CONNECTED_COMPONENTS_H_

// Internal helper shared by the DB postprocess and contour box recovery
// reference components (M3-06). Not a public header: only src/image/*.cpp may
// include it.

#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <vector>

namespace mirador::image_internal {

/// One 8-connected foreground component of a grayscale view. Bounds are the
/// inclusive min/max pixel indices; `value_sum` carries the sum of the pixel
/// values so callers can compute mean scores without re-scanning.
struct Component {
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t max_x = -1;
    int32_t max_y = -1;
    int64_t pixel_count = 0;
    int64_t value_sum = 0;

    [[nodiscard]] int64_t width() const noexcept { return static_cast<int64_t>(max_x) - min_x + 1; }
    [[nodiscard]] int64_t height() const noexcept { return static_cast<int64_t>(max_y) - min_y + 1; }
};

/// Labels the 8-connected components of `gray` over pixels with
/// `value >= threshold`, scanning top-to-bottom, left-to-right with a
/// deterministic FIFO flood fill. The visited bitmap costs one bit per pixel
/// and is checked against `work_budget_bytes` before allocation (RULE-06).
///
/// Errors: kInvalidArgument for an invalid view, kUnsupportedFormat for a
/// non-gray view, kBudgetExceeded when the bitmap exceeds the budget or on
/// internal allocation failure. Never throws.
[[nodiscard]] Result<std::vector<Component>> find_components(const ImageView& gray, uint8_t threshold,
                                                             int64_t work_budget_bytes) noexcept;

}  // namespace mirador::image_internal

#endif  // MIRADOR_IMAGE_CONNECTED_COMPONENTS_H_
