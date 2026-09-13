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
    const int64_t chroma_bytes = format == PixelFormat::kNv12 ? static_cast<int64_t>(width) * ((height + 1) / 2) : 0;
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
        view.secondary_plane = ImagePlane{data_.data() + static_cast<int64_t>(width_) * height_, width_};
    }
    return view;
}

}  // namespace mirador
