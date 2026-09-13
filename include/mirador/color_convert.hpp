#ifndef MIRADOR_COLOR_CONVERT_HPP
#define MIRADOR_COLOR_CONVERT_HPP

#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador {

/// Converts `src` into a newly allocated packed buffer of `dst_format` with the
/// same width/height (design section 11 preprocessing). `src` must pass
/// `validate()`; rotation metadata never affects pixel addressing: the source is
/// processed exactly as presented (rows start at `data + y * row_stride_bytes`).
///
/// Supported matrix (any other combination returns kUnsupportedFormat):
/// - identical formats: row-wise copy, including NV12 (both planes);
/// - interleaved color or NV12 -> kGray8: BT.601 full-range luma via the integer
///   formula `Y = (77*R + 150*G + 29*B + 128) >> 8`; NV12 copies its luma plane;
/// - kGray8 -> interleaved color: gray replicated to R=G=B, alpha set to 255;
/// - interleaved color -> interleaved color: channel remap, missing alpha set
///   to 255, extra alpha dropped;
/// - kNv12 -> interleaved color: BT.601 full-range YUV->RGB with integer
///   coefficients scaled by 2^10 — `r = y + (1436*v') >> 10`,
///   `g = y - ((352*u' + 731*v') >> 10)`, `b = y + (1815*u') >> 10` with
///   `u' = U - 128`, `v' = V - 128`, results clamped to [0, 255]; chroma is
///   sampled at pixel (x / 2, y / 2).
///
/// Anything -> kNv12 (except the identity copy) returns kUnsupportedFormat for
/// now: writing chroma requires a color-range decision that is still pending.
///
/// All kernels are pure integer arithmetic and write every destination byte, so
/// results are bit-identical across compilers and platforms. The request is checked
/// against `max_bytes` before allocation (RULE-06); failures return
/// kInvalidArgument (invalid view/format), kUnsupportedFormat (matrix above) or
/// kBudgetExceeded (budget/allocation). Never throws.
[[nodiscard]] Result<ImageBuffer> convert_color(const ImageView& src, PixelFormat dst_format,
                                                int64_t max_bytes) noexcept;

}  // namespace mirador

#endif  // MIRADOR_COLOR_CONVERT_HPP
