#ifndef MIRADOR_RESIZE_HPP
#define MIRADOR_RESIZE_HPP

#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador {

/// Resamples `src` into a newly allocated packed buffer of `dst_width` x
/// `dst_height` using exact box (area-average) weights in pure integer
/// arithmetic (design section 11: deterministic gray/color downscaling).
///
/// Destination format: `kNv12` sources resample their luma plane into a `kGray8`
/// result (thumbnails only need luma; chroma is dropped); every other valid
/// format keeps the source format. Each destination pixel is the coverage-weighted
/// average of the source pixels it overlaps, with weights scaled to integers by
/// the destination size — so every ratio, including upsampling and non-integer
/// ratios, is bit-identical across compilers. Same-size requests are row copies.
/// The per-pixel average rounds half up: `(sum + sw*sh/2) / (sw*sh)`.
///
/// Coordinate recovery is the caller-side `Transform2D::make_scale(sw / dw,
/// sh / dh)` composition (RULE-05).
///
/// Errors: kInvalidArgument (invalid view, destination sizes outside
/// [1, kMaxImageDimension]), kBudgetExceeded (budget/allocation, checked before
/// allocation, RULE-06). Never throws.
[[nodiscard]] Result<ImageBuffer> resize_area(const ImageView& src, int32_t dst_width, int32_t dst_height,
                                              int64_t max_bytes) noexcept;

}  // namespace mirador

#endif  // MIRADOR_RESIZE_HPP
