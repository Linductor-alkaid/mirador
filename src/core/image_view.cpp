#include <mirador/image_view.hpp>

#include <cstdint>

#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

namespace mirador {
namespace {

Status invalid(const char* reason) {
    return {ErrorCode::kInvalidArgument, reason};
}

}  // namespace

Result<void> validate(const ImageView& image) {
    if (!is_valid(image.format)) {
        return invalid("unknown pixel format");
    }
    if (!is_valid(image.rotation)) {
        return invalid("unknown rotation");
    }
    if (image.width <= 0 || image.height <= 0) {
        return invalid("dimensions must be positive");
    }
    if (image.width > kMaxImageDimension || image.height > kMaxImageDimension) {
        return invalid("dimensions exceed kMaxImageDimension");
    }
    if (image.data == nullptr) {
        return invalid("primary plane data is null");
    }
    const int64_t min_primary_stride = min_row_stride_bytes(image.format, 0, image.width);
    if (image.row_stride_bytes < min_primary_stride) {
        return invalid("primary plane stride below minimum for width and format");
    }
    if (plane_count(image.format) == 2) {
        if (image.secondary_plane.data == nullptr) {
            return invalid("multi-plane format requires a populated secondary plane");
        }
        const int64_t min_secondary_stride = min_row_stride_bytes(image.format, 1, image.width);
        if (image.secondary_plane.row_stride_bytes < min_secondary_stride) {
            return invalid("secondary plane stride below minimum for width and format");
        }
    } else if (image.secondary_plane.data != nullptr) {
        return invalid("single-plane format must not carry a secondary plane");
    }
    return Status::success();
}

}  // namespace mirador
