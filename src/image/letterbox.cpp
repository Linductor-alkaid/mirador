#include <mirador/letterbox.hpp>

#include <mirador/resize.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace mirador {

Result<LetterboxResult> letterbox(const ImageView& src, const LetterboxRequest& request, int64_t max_bytes) noexcept {
    if (!validate(src).ok()) {
        return Status(ErrorCode::kInvalidArgument, "invalid source view");
    }
    if (src.format == PixelFormat::kNv12) {
        return Status(ErrorCode::kUnsupportedFormat, "letterbox does not support NV12 yet (chroma padding pending)");
    }
    if (request.dst_width < 1 || request.dst_width > kMaxImageDimension || request.dst_height < 1 ||
        request.dst_height > kMaxImageDimension) {
        return Status(ErrorCode::kInvalidArgument, "destination dimensions out of range");
    }

    const double scale = std::min(static_cast<double>(request.dst_width) / static_cast<double>(src.width),
                                  static_cast<double>(request.dst_height) / static_cast<double>(src.height));
    const auto clamp_size = [](double value, int32_t limit) {
        const auto rounded = static_cast<int64_t>(std::floor(value + 0.5));
        return static_cast<int32_t>(std::clamp<int64_t>(rounded, 1, static_cast<int64_t>(limit)));
    };
    const int32_t resized_width = clamp_size(static_cast<double>(src.width) * scale, request.dst_width);
    const int32_t resized_height = clamp_size(static_cast<double>(src.height) * scale, request.dst_height);
    const int32_t offset_x = (request.dst_width - resized_width) / 2;
    const int32_t offset_y = (request.dst_height - resized_height) / 2;

    // Budget: the resized intermediate and the padded destination must both fit.
    const int64_t destination_bytes = static_cast<int64_t>(request.dst_width) * request.dst_height *
                                      bytes_per_pixel(src.format);
    if (destination_bytes > max_bytes) {
        return Status(ErrorCode::kBudgetExceeded, "letterbox destination exceeds the byte budget");
    }
    auto resized_result = resize_area(src, resized_width, resized_height, max_bytes);
    if (!resized_result.ok()) {
        return Status(resized_result.status().code(),
                      std::string("letterbox resize failed: ") + resized_result.status().message());
    }
    ImageBuffer resized = resized_result.take_value();
    auto buffer_result = ImageBuffer::create(src.format, request.dst_width, request.dst_height, max_bytes);
    if (!buffer_result.ok()) {
        return Status(buffer_result.status().code(),
                      std::string("letterbox buffer failed: ") + buffer_result.status().message());
    }
    ImageBuffer buffer = buffer_result.take_value();

    const int64_t bpp = bytes_per_pixel(src.format);
    const int64_t row_bytes = static_cast<int64_t>(resized_width) * bpp;
    const int64_t src_stride = resized.row_stride_bytes();
    const int64_t dst_stride = buffer.row_stride_bytes();
    std::byte* dst = buffer.data();
    const std::byte* content = resized.data();
    // Fill with the pad value, then paste the centered content rows.
    std::memset(dst, static_cast<int>(request.pad_value), static_cast<size_t>(buffer.byte_size()));
    for (int32_t y = 0; y < resized_height; ++y) {
        std::memcpy(dst + static_cast<int64_t>(offset_y + y) * dst_stride + static_cast<int64_t>(offset_x) * bpp,
                    content + static_cast<int64_t>(y) * src_stride, static_cast<size_t>(row_bytes));
    }

    LetterboxResult result;
    result.buffer = std::move(buffer);
    result.resized_width = resized_width;
    result.resized_height = resized_height;
    // Exact forward mapping of the executed pixels: p_model = p_src * (rw/sw, rh/sh) + offset.
    auto scaled = make_scale(static_cast<double>(resized_width) / static_cast<double>(src.width),
                             static_cast<double>(resized_height) / static_cast<double>(src.height),
                             request.from_space, request.to_space);
    auto with_offset = compose(scaled, make_translation(static_cast<double>(offset_x), static_cast<double>(offset_y),
                                                        request.to_space, request.to_space));
    if (!with_offset.ok()) {
        return Status(with_offset.status().code(),
                      std::string("letterbox transform failed: ") + with_offset.status().message());
    }
    result.transform = with_offset.take_value();
    return result;
}

}  // namespace mirador
