#ifndef MIRADOR_CROP_HPP
#define MIRADOR_CROP_HPP

#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador {

/// Crops `roi` out of `src` into a newly allocated packed buffer of the same
/// format (design section 11). The ROI is interpreted in the presented coordinate
/// space of the view (post-rotation); callers recover raw-frame coordinates by
/// composing `make_rotation` with `make_crop` and inverting the chain (RULE-05).
/// The output buffer is a k0 presentation of the cropped region.
///
/// NV12: the chroma plane is cropped with the standard 2:1 subsampling rule —
/// chroma origin `(roi.x / 2, roi.y / 2)` and size
/// `ceil(roi.width / 2) x ceil(roi.height / 2)`, whatever the parity of the ROI.
///
/// Errors: kInvalidArgument (invalid view, empty or out-of-bounds ROI),
/// kBudgetExceeded (budget/allocation, checked before allocation, RULE-06).
/// Never throws.
[[nodiscard]] Result<ImageBuffer> crop(const ImageView& src, const RectI& roi, int64_t max_bytes) noexcept;

}  // namespace mirador

#endif  // MIRADOR_CROP_HPP
