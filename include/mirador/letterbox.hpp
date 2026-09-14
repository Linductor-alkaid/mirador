#ifndef MIRADOR_LETTERBOX_HPP
#define MIRADOR_LETTERBOX_HPP

#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/transform.hpp>

#include <cstdint>

namespace mirador {

/// The pixel-space counterpart of `make_letterbox` (design sections 7, 14;
/// M3-04): resamples `src` by the letterbox scale into `(resized_width,
/// resized_height)`, centers it inside a `dst_width` x `dst_height` buffer and
/// fills the rest with `pad_value`. The result carries the exact
/// `Transform2D` of the executed pixel operation — derived from the actual
/// integer resized size and centering offset, not the continuous ideal — so
/// coordinate recovery through `inverse` matches the produced pixels
/// (RULE-05, DEC-014).
struct LetterboxResult {
    ImageBuffer buffer;      ///< dst-sized packed buffer in the source format
    Transform2D transform;   ///< `request.from_space` -> `request.to_space`, forward mapping
    int32_t resized_width = 0;   ///< the source content width inside the buffer
    int32_t resized_height = 0;  ///< the source content height inside the buffer
};

/// Letterbox request. `pad_value` fills every channel of every padded pixel
/// (including alpha); `from_space`/`to_space` label the returned transform.
struct LetterboxRequest {
    int32_t dst_width = 0;
    int32_t dst_height = 0;
    uint8_t pad_value = 0;
    CoordinateSpaceId from_space = CoordinateSpaceId::kOriented;
    CoordinateSpaceId to_space = CoordinateSpaceId::kModelInput;
};

/// Builds a letterboxed model input from a single-plane view. The resized
/// content size follows the deterministic rule `rw = clamp(floor(sw * scale +
/// 0.5), 1, dw)` with `scale = min(dw/sw, dh/sh)`; the content is centered at
/// `((dw - rw) / 2, (dh - rh) / 2)`. The source pixels are resampled with the
/// deterministic area kernel (`resize_area`), so equal inputs produce
/// bit-identical buffers. The input view is never modified (RULE-04).
///
/// NV12 is rejected with kUnsupportedFormat for now: chroma padding needs the
/// same color-range decision as writing NV12 in `convert_color` (DEC-014).
///
/// Errors: kInvalidArgument (invalid view, destination dimensions outside
/// [1, kMaxImageDimension]), kUnsupportedFormat (NV12), kBudgetExceeded
/// (`max_bytes` covers neither the intermediate nor the destination buffer,
/// checked before allocation, RULE-06). Never throws.
[[nodiscard]] Result<LetterboxResult> letterbox(const ImageView& src, const LetterboxRequest& request,
                                                int64_t max_bytes) noexcept;

}  // namespace mirador

#endif  // MIRADOR_LETTERBOX_HPP
