#ifndef MIRADOR_PIXEL_FORMAT_HPP
#define MIRADOR_PIXEL_FORMAT_HPP

#include <cstdint>

namespace mirador {

/// Pixel layouts accepted by the public input model (design section 6). Multi-plane
/// formats are represented through ImagePlane (DEC-007 draft); no format assumes a
/// single contiguous plane.
enum class PixelFormat : uint8_t {
    kGray8,
    kRgb8,
    kBgr8,
    kRgba8,
    kBgra8,
    kNv12,
};

/// Orientation applied to the raw capture when producing an image view. Values are the
/// rotation in degrees; coordinates recover to the raw frame through Transform2D
/// (design section 7).
enum class Rotation : uint16_t { k0 = 0, k90 = 90, k180 = 180, k270 = 270 };

/// True when the value is one of the defined PixelFormat enumerators.
[[nodiscard]] bool is_valid(PixelFormat format) noexcept;

/// True when the value is one of the defined Rotation enumerators.
[[nodiscard]] bool is_valid(Rotation rotation) noexcept;

/// Number of planes used to store the image: 1 for packed formats, 2 for NV12
/// (DEC-007 draft). Returns 0 for undefined format values.
[[nodiscard]] int32_t plane_count(PixelFormat format) noexcept;

/// Bytes per pixel of the primary (index 0) plane: 1 for kGray8/kNv12 (Y), 3 for
/// kRgb8/kBgr8, 4 for kRgba8/kBgra8. Returns 0 for undefined format values.
[[nodiscard]] int32_t bytes_per_pixel(PixelFormat format) noexcept;

/// Minimum row stride in bytes for `plane_index` of a plane `width` pixels wide.
/// NV12 chroma rows hold `ceil(width / 2)` interleaved UV pairs, so their minimum
/// stride is `width + (width % 2)` (DEC-007, frozen in M1). Returns -1 for invalid
/// format, plane index or width. Strides in Mirador are non-negative; bottom-up
/// buffers with negative stride must be normalized by the platform adapter before
/// wrapping.
[[nodiscard]] int64_t min_row_stride_bytes(PixelFormat format, int32_t plane_index, int32_t width) noexcept;

}  // namespace mirador

#endif  // MIRADOR_PIXEL_FORMAT_HPP
