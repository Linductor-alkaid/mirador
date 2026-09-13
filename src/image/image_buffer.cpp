#include <mirador/image_buffer.hpp>

#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace mirador {

Result<ImageBuffer> ImageBuffer::create(PixelFormat format, int32_t width, int32_t height, int64_t max_bytes) noexcept {
    if (!is_valid(format) || width < 1 || height < 1 || width > kMaxImageDimension || height > kMaxImageDimension) {
        return Status(ErrorCode::kInvalidArgument, "ImageBuffer::create: invalid format or dimensions");
    }
    // Bounded by kMaxImageDimension^2 * 4 (about 1.7e10), which fits int64_t.
    const int64_t primary_bytes = static_cast<int64_t>(width) * bytes_per_pixel(format) * height;
    int64_t chroma_bytes = 0;
    if (format == PixelFormat::kNv12) {
        // DEC-007 (frozen): chroma rows hold ceil(width / 2) UV pairs, so odd
        // widths need one extra byte per row.
        const int64_t chroma_row_bytes = static_cast<int64_t>(width) + (width % 2);
        chroma_bytes = chroma_row_bytes * ((height + 1) / 2);
    }
    const int64_t total_bytes = primary_bytes + chroma_bytes;
    if (total_bytes > max_bytes) {
        return Status(ErrorCode::kBudgetExceeded, "ImageBuffer::create: request exceeds the declared byte budget");
    }
    ImageBuffer buffer;
    try {
        buffer.data_.resize(static_cast<size_t>(total_bytes));  // value-initialized to zero
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "ImageBuffer::create: allocation failed");
    }
    buffer.width_ = width;
    buffer.height_ = height;
    buffer.format_ = format;
    buffer.byte_size_ = total_bytes;
    return {std::move(buffer)};
}

ImageView ImageBuffer::view() const noexcept {
    if (empty()) {
        return ImageView{};
    }
    ImageView view;
    view.data = data_.data();
    view.width = width_;
    view.height = height_;
    view.row_stride_bytes = row_stride_bytes();
    view.format = format_;
    view.rotation = Rotation::k0;
    if (format_ == PixelFormat::kNv12) {
        const int64_t primary_bytes = static_cast<int64_t>(width_) * height_;
        const auto chroma_stride = static_cast<int64_t>(width_) + (width_ % 2);
        view.secondary_plane = ImagePlane{data_.data() + primary_bytes, chroma_stride};
    }
    return view;
}

}  // namespace mirador
