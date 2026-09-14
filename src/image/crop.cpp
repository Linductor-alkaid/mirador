#include <mirador/crop.hpp>

#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace mirador {
namespace {

/// Destination row accessors of the freshly allocated packed crop buffer.
struct MutableImageRows {
    std::byte* primary;
    int64_t primary_stride;
    int64_t chroma_stride;  // NV12 only, per DEC-007 even-rounded chroma rows
};

void crop_single_plane(const ImageView& src, const RectI& roi, const MutableImageRows& dst) {
    const int64_t row_bytes = static_cast<int64_t>(roi.width) * bytes_per_pixel(src.format);
    for (int32_t y = 0; y < roi.height; ++y) {
        const std::byte* src_row = src.data + static_cast<int64_t>(roi.y + y) * src.row_stride_bytes +
                                   static_cast<int64_t>(roi.x) * bytes_per_pixel(src.format);
        std::memcpy(dst.primary + static_cast<int64_t>(y) * dst.primary_stride, src_row,
                    static_cast<size_t>(row_bytes));
    }
}

void crop_nv12_chroma(const ImageView& src, const RectI& roi, const MutableImageRows& dst) {
    const int32_t chroma_x = roi.x / 2;
    const int32_t chroma_y = roi.y / 2;
    const int32_t chroma_width = (roi.width + 1) / 2;
    const int32_t chroma_height = (roi.height + 1) / 2;
    const int64_t chroma_plane_offset = static_cast<int64_t>(roi.height) * dst.primary_stride;
    for (int32_t y = 0; y < chroma_height; ++y) {
        const std::byte* src_row = src.secondary_plane.data +
                                   static_cast<int64_t>(chroma_y + y) * src.secondary_plane.row_stride_bytes + chroma_x;
        std::byte* dst_row = dst.primary + chroma_plane_offset + static_cast<int64_t>(y) * dst.chroma_stride;
        std::memcpy(dst_row, src_row, static_cast<size_t>(chroma_width));
    }
}

}  // namespace

Result<ImageBuffer> crop(const ImageView& src, const RectI& roi, int64_t max_bytes) noexcept {
    if (const auto valid = validate(src); !valid.ok()) {
        return valid.status();
    }
    if (!is_valid(roi) || roi.width <= 0 || roi.height <= 0 || roi.x < 0 || roi.y < 0) {
        return Status(ErrorCode::kInvalidArgument, "crop: ROI must be non-empty and usable");
    }
    if (!contains(RectI{0, 0, src.width, src.height}, roi)) {
        return Status(ErrorCode::kInvalidArgument, "crop: ROI extends outside the view");
    }

    auto dst = ImageBuffer::create(src.format, roi.width, roi.height, max_bytes);
    if (!dst.ok()) {
        return dst.status();
    }
    ImageBuffer dst_buffer = dst.take_value();
    // DEC-007 chroma rows hold ceil(width / 2) UV pairs: stride rounds up to even.
    const MutableImageRows dst_rows{dst_buffer.data(), dst_buffer.row_stride_bytes(),
                                    dst_buffer.row_stride_bytes() + (roi.width % 2)};
    crop_single_plane(src, roi, dst_rows);
    if (src.format == PixelFormat::kNv12) {
        crop_nv12_chroma(src, roi, dst_rows);
    }
    return {std::move(dst_buffer)};
}

}  // namespace mirador
