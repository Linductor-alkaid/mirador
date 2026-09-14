#include <mirador/resize.hpp>

#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace mirador {
namespace {

constexpr int32_t kMaxChannels = 4;

/// Resamples one plane of 1-byte samples with exact area weights. `src_bpp`/
/// `dst_bpp` are the pixel strides (bytes per pixel) of the surrounding buffers;
/// `channel_count` channels starting at offset 0 of each pixel are resampled.
/// Weights are source coverages scaled by (dw, dh), so their sum per destination
/// pixel is exactly sw * sh and no floating point is involved. Coverage weights
/// are precomputed per source row/column (each belongs to exactly one
/// destination row/column), which keeps the hot loop free of min/max while
/// producing bit-identical results to the direct formulas. May allocate; throws
/// only on allocation failure (the caller maps that to kBudgetExceeded).
void resize_plane(const std::byte* src, int64_t src_stride, int64_t src_bpp, int32_t sw, int32_t sh, std::byte* dst,
                  int64_t dst_stride, int64_t dst_bpp, int32_t dw, int32_t dh, int32_t channel_count) {
    const int64_t total_weight = static_cast<int64_t>(sw) * sh;
    const int64_t half = total_weight / 2;
    std::vector<int64_t> col_weight(static_cast<size_t>(sw));
    std::vector<int64_t> row_weight(static_cast<size_t>(sh));
    for (int32_t dx = 0; dx < dw; ++dx) {
        const int64_t sx0 = static_cast<int64_t>(dx) * sw / dw;
        const int64_t sx1 = (static_cast<int64_t>(dx + 1) * sw + dw - 1) / dw;
        for (int64_t sx = sx0; sx < sx1; ++sx) {
            col_weight[static_cast<size_t>(sx)] = std::min(static_cast<int64_t>(dx + 1) * sw, (sx + 1) * dw) -
                                                  std::max(static_cast<int64_t>(dx) * sw, sx * dw);
        }
    }
    for (int32_t dy = 0; dy < dh; ++dy) {
        const int64_t sy0 = static_cast<int64_t>(dy) * sh / dh;
        const int64_t sy1 = (static_cast<int64_t>(dy + 1) * sh + dh - 1) / dh;
        for (int64_t sy = sy0; sy < sy1; ++sy) {
            row_weight[static_cast<size_t>(sy)] = std::min(static_cast<int64_t>(dy + 1) * sh, (sy + 1) * dh) -
                                                  std::max(static_cast<int64_t>(dy) * sh, sy * dh);
        }
    }
    for (int32_t dy = 0; dy < dh; ++dy) {
        const int64_t sy0 = static_cast<int64_t>(dy) * sh / dh;
        const int64_t sy1 = (static_cast<int64_t>(dy + 1) * sh + dh - 1) / dh;
        std::byte* dst_row = dst + static_cast<int64_t>(dy) * dst_stride;
        for (int32_t dx = 0; dx < dw; ++dx) {
            const int64_t sx0 = static_cast<int64_t>(dx) * sw / dw;
            const int64_t sx1 = (static_cast<int64_t>(dx + 1) * sw + dw - 1) / dw;
            std::array<int64_t, static_cast<size_t>(kMaxChannels)> acc{};
            for (int64_t sy = sy0; sy < sy1; ++sy) {
                const int64_t oy = row_weight[static_cast<size_t>(sy)];
                const std::byte* src_row = src + sy * src_stride;
                for (int64_t sx = sx0; sx < sx1; ++sx) {
                    const int64_t weight = oy * col_weight[static_cast<size_t>(sx)];
                    for (int32_t c = 0; c < channel_count; ++c) {
                        acc[c] += weight * static_cast<int32_t>(src_row[sx * src_bpp + c]);
                    }
                }
            }
            for (int32_t c = 0; c < channel_count; ++c) {
                const auto value = static_cast<int32_t>((acc[c] + half) / total_weight);
                dst_row[static_cast<int64_t>(dx) * dst_bpp + c] = static_cast<std::byte>(std::clamp(value, 0, 255));
            }
        }
    }
}

void copy_rows(const ImageView& src, std::byte* dst, int64_t dst_stride, int64_t dst_bpp) {
    const int64_t row_bytes = static_cast<int64_t>(src.width) * dst_bpp;
    for (int32_t y = 0; y < src.height; ++y) {
        std::memcpy(dst + static_cast<int64_t>(y) * dst_stride,
                    src.data + static_cast<int64_t>(y) * src.row_stride_bytes, static_cast<size_t>(row_bytes));
    }
}

}  // namespace

Result<ImageBuffer> resize_area(const ImageView& src, int32_t dst_width, int32_t dst_height,
                                int64_t max_bytes) noexcept {
    if (const auto valid = validate(src); !valid.ok()) {
        return valid.status();
    }
    if (dst_width < 1 || dst_height < 1 || dst_width > kMaxImageDimension || dst_height > kMaxImageDimension) {
        return Status(ErrorCode::kInvalidArgument,
                      "resize_area: destination sizes must be within [1, kMaxImageDimension]");
    }

    // NV12 thumbnails resample the luma plane only; every other format is kept.
    const PixelFormat dst_format = src.format == PixelFormat::kNv12 ? PixelFormat::kGray8 : src.format;
    auto dst = ImageBuffer::create(dst_format, dst_width, dst_height, max_bytes);
    if (!dst.ok()) {
        return dst.status();
    }
    ImageBuffer dst_buffer = dst.take_value();
    const int64_t dst_stride = dst_buffer.row_stride_bytes();
    std::byte* dst_data = dst_buffer.data();

    const int32_t channel_count = bytes_per_pixel(src.format);  // NV12 luma: 1 sample per pixel
    if (src.width == dst_width && src.height == dst_height) {
        copy_rows(src, dst_data, dst_stride, channel_count);
        return {std::move(dst_buffer)};
    }
    try {
        resize_plane(src.data, src.row_stride_bytes, channel_count, src.width, src.height, dst_data, dst_stride,
                     channel_count, dst_width, dst_height, channel_count);
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "resize_area: weight table allocation failed");
    }
    return {std::move(dst_buffer)};
}

}  // namespace mirador
