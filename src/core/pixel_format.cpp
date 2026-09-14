#include <mirador/pixel_format.hpp>

#include <cstdint>

namespace mirador {

bool is_valid(PixelFormat format) noexcept {
    return static_cast<uint8_t>(format) <= static_cast<uint8_t>(PixelFormat::kNv12);
}

bool is_valid(Rotation rotation) noexcept {
    switch (rotation) {
        case Rotation::k0:
        case Rotation::k90:
        case Rotation::k180:
        case Rotation::k270:
            return true;
    }
    return false;
}

int32_t plane_count(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::kGray8:
        case PixelFormat::kRgb8:
        case PixelFormat::kBgr8:
        case PixelFormat::kRgba8:
        case PixelFormat::kBgra8:
            return 1;
        case PixelFormat::kNv12:
            return 2;
    }
    return 0;
}

int32_t bytes_per_pixel(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::kGray8:
        case PixelFormat::kNv12:
            return 1;
        case PixelFormat::kRgb8:
        case PixelFormat::kBgr8:
            return 3;
        case PixelFormat::kRgba8:
        case PixelFormat::kBgra8:
            return 4;
    }
    return 0;
}

int64_t min_row_stride_bytes(PixelFormat format, int32_t plane_index, int32_t width) noexcept {
    if (width <= 0 || !is_valid(format) || plane_index < 0 || plane_index >= plane_count(format)) {
        return -1;
    }
    if (plane_index == 0) {
        return static_cast<int64_t>(width) * bytes_per_pixel(format);
    }
    // Only NV12 reaches here with plane_count == 2; a chroma row holds
    // ceil(width / 2) interleaved UV pairs, so odd widths need one extra byte.
    return static_cast<int64_t>(width) + (width % 2);
}

}  // namespace mirador
