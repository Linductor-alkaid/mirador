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

/// Destination-axis coverage table for one dimension: every destination index
/// owns a contiguous source range starting at `begin[d]` with
/// `offsets[d + 1] - offsets[d]` coverage weights inside the flattened array. A
/// source row/column can back several destination rows/columns when upscaling,
/// so weights must be keyed per destination, not per source.
struct CoverageAxis {
    std::vector<int64_t> begin{};
    std::vector<int64_t> offsets{0};
    std::vector<int64_t> weights{};
};

/// Builds the axis table for `dst` destinations over `src_count` sources: the
/// weights are source coverages scaled by `dst`, so they sum per destination
/// index to exactly `src_count` and no floating point is involved. The
/// flattened size stays within `src_count + dst` because each destination
/// range is shorter than `src_count / dst + 1` entries.
CoverageAxis build_coverage_axis(int32_t src_count, int32_t dst) {
    CoverageAxis axis;
    axis.begin.assign(static_cast<size_t>(dst), 0);
    axis.offsets.assign(static_cast<size_t>(dst) + 1, 0);
    axis.weights.reserve(static_cast<size_t>(src_count) + static_cast<size_t>(dst));
    for (int32_t d = 0; d < dst; ++d) {
        const int64_t s0 = static_cast<int64_t>(d) * src_count / dst;
        const int64_t s1 = (static_cast<int64_t>(d + 1) * src_count + dst - 1) / dst;
        axis.begin[static_cast<size_t>(d)] = s0;
        for (int64_t s = s0; s < s1; ++s) {
            axis.weights.push_back(std::min(static_cast<int64_t>(d + 1) * src_count, (s + 1) * dst) -
                                   std::max(static_cast<int64_t>(d) * src_count, s * dst));
        }
        axis.offsets[static_cast<size_t>(d) + 1] = static_cast<int64_t>(axis.weights.size());
    }
    return axis;
}

/// Resamples one plane of 1-byte samples with exact area weights. `src_bpp`/
/// `dst_bpp` are the pixel strides (bytes per pixel) of the surrounding buffers;
/// `channel_count` channels starting at offset 0 of each pixel are resampled.
/// Weights are source coverages scaled by (dw, dh), so their sum per destination
/// pixel is exactly sw * sh and no floating point is involved. Coverage weights
/// are precomputed per destination row/column (upscaled source rows/columns
/// repeat across every destination they back), which keeps the hot loop free of
/// min/max while producing bit-identical results to the direct formulas. May
/// allocate; throws only on allocation failure (the caller maps that to
/// kBudgetExceeded).
void resize_plane(const std::byte* src, int64_t src_stride, int64_t src_bpp, int32_t sw, int32_t sh, std::byte* dst,
                  int64_t dst_stride, int64_t dst_bpp, int32_t dw, int32_t dh, int32_t channel_count) {
    const int64_t total_weight = static_cast<int64_t>(sw) * sh;
    const int64_t half = total_weight / 2;
    const CoverageAxis cols = build_coverage_axis(sw, dw);
    const CoverageAxis rows = build_coverage_axis(sh, dh);
    for (int32_t dy = 0; dy < dh; ++dy) {
        const int64_t row_base = rows.offsets[static_cast<size_t>(dy)];
        const int64_t row_weights = rows.offsets[static_cast<size_t>(dy) + 1] - row_base;
        std::byte* dst_row = dst + static_cast<int64_t>(dy) * dst_stride;
        for (int32_t dx = 0; dx < dw; ++dx) {
            const int64_t col_base = cols.offsets[static_cast<size_t>(dx)];
            const int64_t col_weights = cols.offsets[static_cast<size_t>(dx) + 1] - col_base;
            std::array<int64_t, static_cast<size_t>(kMaxChannels)> acc{};
            for (int64_t j = 0; j < row_weights; ++j) {
                const int64_t oy = rows.weights[static_cast<size_t>(row_base + j)];
                const std::byte* src_row = src + (rows.begin[static_cast<size_t>(dy)] + j) * src_stride;
                for (int64_t i = 0; i < col_weights; ++i) {
                    const int64_t weight = oy * cols.weights[static_cast<size_t>(col_base + i)];
                    const std::byte* pixel = src_row + (cols.begin[static_cast<size_t>(dx)] + i) * src_bpp;
                    for (int32_t c = 0; c < channel_count; ++c) {
                        acc[c] += weight * static_cast<int32_t>(pixel[c]);
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
