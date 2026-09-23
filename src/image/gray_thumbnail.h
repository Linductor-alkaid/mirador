#ifndef MIRADOR_SRC_IMAGE_GRAY_THUMBNAIL_H_
#define MIRADOR_SRC_IMAGE_GRAY_THUMBNAIL_H_

// Internal helper shared by the layered change detection (M1) and the global
// shift estimation primitive (M7-04): the square grayscale comparison
// thumbnail both build on, so they compare bit-identical pixel data at the
// same edge length. Not a public header: only src/image/*.cpp may include it.

#include <mirador/color_convert.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador::image_internal {

/// Resamples `src` into a newly owned square `size` x `size` grayscale buffer
/// using the exact deterministic pipeline of the change-detection thumbnails
/// (integer box area resample, then BT.601 full-range luma conversion when the
/// resampled format is not already kGray8; design section 11). `max_bytes`
/// bounds each internal allocation request before it happens (RULE-06).
///
/// Errors: kInvalidArgument for an invalid view or sizes outside
/// [1, kMaxImageDimension], kBudgetExceeded when a request exceeds `max_bytes`
/// or the allocation itself fails. Never throws.
[[nodiscard]] inline Result<ImageBuffer> gray_thumbnail(const ImageView& src, int32_t size,
                                                        int64_t max_bytes) noexcept {
    Result<ImageBuffer> small = resize_area(src, size, size, max_bytes);
    if (!small.ok()) {
        return small.status();
    }
    ImageBuffer thumb = small.take_value();
    if (thumb.format() == PixelFormat::kGray8) {
        return thumb;
    }
    return convert_color(thumb.view(), PixelFormat::kGray8, max_bytes);
}

}  // namespace mirador::image_internal

#endif  // MIRADOR_SRC_IMAGE_GRAY_THUMBNAIL_H_
