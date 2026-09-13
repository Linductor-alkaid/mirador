#include <mirador/color_convert.hpp>

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

/// Writable pixel-row accessor for freshly allocated destination memory.
struct MutableView {
    std::byte* data;
    int64_t row_stride_bytes;
};

/// Byte positions of the R, G, B and A channels inside one pixel; `a < 0` marks a
/// layout without alpha.
struct ChannelLayout {
    int8_t r;
    int8_t g;
    int8_t b;
    int8_t a;
};

ChannelLayout layout_of(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::kRgb8:
            return {0, 1, 2, -1};
        case PixelFormat::kBgr8:
            return {2, 1, 0, -1};
        case PixelFormat::kRgba8:
            return {0, 1, 2, 3};
        case PixelFormat::kBgra8:
            return {2, 1, 0, 3};
        case PixelFormat::kGray8:
        case PixelFormat::kNv12:
            break;
    }
    return {-1, -1, -1, -1};
}

uint8_t clamp_to_byte(int32_t value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

void copy_rows(const ImageView& src, MutableView dst) {
    const int64_t row_bytes = static_cast<int64_t>(src.width) * bytes_per_pixel(src.format);
    for (int32_t y = 0; y < src.height; ++y) {
        std::memcpy(dst.data + static_cast<int64_t>(y) * dst.row_stride_bytes,
                    src.data + static_cast<int64_t>(y) * src.row_stride_bytes, static_cast<size_t>(row_bytes));
    }
    if (src.format == PixelFormat::kNv12) {
        const int32_t chroma_height = (src.height + 1) / 2;
        const int64_t chroma_row_bytes = src.width;
        for (int32_t y = 0; y < chroma_height; ++y) {
            std::memcpy(dst.data + src.height * dst.row_stride_bytes + static_cast<int64_t>(y) * chroma_row_bytes,
                        src.secondary_plane.data + static_cast<int64_t>(y) * src.secondary_plane.row_stride_bytes,
                        static_cast<size_t>(chroma_row_bytes));
        }
    }
}

void copy_luma_rows(const ImageView& src, MutableView dst) {
    // NV12 luma rows are `width` bytes, exactly matching a kGray8 destination.
    const int64_t row_bytes = src.width;
    for (int32_t y = 0; y < src.height; ++y) {
        std::memcpy(dst.data + static_cast<int64_t>(y) * dst.row_stride_bytes,
                    src.data + static_cast<int64_t>(y) * src.row_stride_bytes, static_cast<size_t>(row_bytes));
    }
}

void interleaved_to_gray(const ImageView& src, MutableView dst) {
    const ChannelLayout src_layout = layout_of(src.format);
    const int8_t r = src_layout.r;
    const int8_t g = src_layout.g;
    const int8_t b = src_layout.b;
    const int64_t src_bpp = bytes_per_pixel(src.format);
    for (int32_t y = 0; y < src.height; ++y) {
        const std::byte* src_row = src.data + static_cast<int64_t>(y) * src.row_stride_bytes;
        std::byte* dst_row = dst.data + static_cast<int64_t>(y) * dst.row_stride_bytes;
        for (int32_t x = 0; x < src.width; ++x) {
            const auto red = static_cast<int32_t>(src_row[x * src_bpp + r]);
            const auto green = static_cast<int32_t>(src_row[x * src_bpp + g]);
            const auto blue = static_cast<int32_t>(src_row[x * src_bpp + b]);
            const auto luma = static_cast<uint8_t>((77 * red + 150 * green + 29 * blue + 128) >> 8);
            dst_row[x] = static_cast<std::byte>(luma);
        }
    }
}

void gray_to_interleaved(const ImageView& src, MutableView dst, const ChannelLayout& dst_layout, int64_t dst_bpp) {
    for (int32_t y = 0; y < src.height; ++y) {
        const std::byte* src_row = src.data + static_cast<int64_t>(y) * src.row_stride_bytes;
        std::byte* dst_row = dst.data + static_cast<int64_t>(y) * dst.row_stride_bytes;
        for (int32_t x = 0; x < src.width; ++x) {
            const auto gray = src_row[x];
            dst_row[x * dst_bpp + dst_layout.r] = gray;
            dst_row[x * dst_bpp + dst_layout.g] = gray;
            dst_row[x * dst_bpp + dst_layout.b] = gray;
            if (dst_layout.a >= 0) {
                dst_row[x * dst_bpp + dst_layout.a] = static_cast<std::byte>(255);
            }
        }
    }
}

void interleaved_to_interleaved(const ImageView& src, MutableView dst, const ChannelLayout& dst_layout,
                                int64_t dst_bpp) {
    const ChannelLayout src_layout = layout_of(src.format);
    const int64_t src_bpp = bytes_per_pixel(src.format);
    for (int32_t y = 0; y < src.height; ++y) {
        const std::byte* src_row = src.data + static_cast<int64_t>(y) * src.row_stride_bytes;
        std::byte* dst_row = dst.data + static_cast<int64_t>(y) * dst.row_stride_bytes;
        for (int32_t x = 0; x < src.width; ++x) {
            const std::byte* src_pixel = src_row + x * src_bpp;
            std::byte* dst_pixel = dst_row + x * dst_bpp;
            dst_pixel[dst_layout.r] = src_pixel[src_layout.r];
            dst_pixel[dst_layout.g] = src_pixel[src_layout.g];
            dst_pixel[dst_layout.b] = src_pixel[src_layout.b];
            if (dst_layout.a >= 0) {
                dst_pixel[dst_layout.a] = src_layout.a >= 0 ? src_pixel[src_layout.a] : static_cast<std::byte>(255);
            }
        }
    }
}

void nv12_to_interleaved(const ImageView& src, MutableView dst, const ChannelLayout& dst_layout, int64_t dst_bpp) {
    for (int32_t y = 0; y < src.height; ++y) {
        const std::byte* luma_row = src.data + static_cast<int64_t>(y) * src.row_stride_bytes;
        const std::byte* chroma_row =
            src.secondary_plane.data + static_cast<int64_t>(y / 2) * src.secondary_plane.row_stride_bytes;
        std::byte* dst_row = dst.data + static_cast<int64_t>(y) * dst.row_stride_bytes;
        for (int32_t x = 0; x < src.width; ++x) {
            const auto y_value = static_cast<int32_t>(luma_row[x]);
            const int64_t chroma_offset = static_cast<int64_t>(x / 2) * 2;
            const auto u_value = static_cast<int32_t>(chroma_row[chroma_offset]) - 128;
            const auto v_value = static_cast<int32_t>(chroma_row[chroma_offset + 1]) - 128;
            std::byte* dst_pixel = dst_row + x * dst_bpp;
            dst_pixel[dst_layout.r] = static_cast<std::byte>(clamp_to_byte(y_value + ((1436 * v_value) >> 10)));
            dst_pixel[dst_layout.g] =
                static_cast<std::byte>(clamp_to_byte(y_value - ((352 * u_value + 731 * v_value) >> 10)));
            dst_pixel[dst_layout.b] = static_cast<std::byte>(clamp_to_byte(y_value + ((1815 * u_value) >> 10)));
            if (dst_layout.a >= 0) {
                dst_pixel[dst_layout.a] = static_cast<std::byte>(255);
            }
        }
    }
}

}  // namespace

Result<ImageBuffer> convert_color(const ImageView& src, PixelFormat dst_format, int64_t max_bytes) noexcept {
    if (const auto valid = validate(src); !valid.ok()) {
        return valid.status();
    }
    if (!is_valid(dst_format)) {
        return Status(ErrorCode::kInvalidArgument, "convert_color: undefined destination format");
    }

    // Classify the conversion before allocating so unsupported combinations fail
    // without allocating anything.
    const bool same_format = src.format == dst_format;
    const bool to_gray = dst_format == PixelFormat::kGray8;
    if (!same_format && dst_format == PixelFormat::kNv12) {
        return Status(ErrorCode::kUnsupportedFormat, "convert_color: conversion into NV12 is not supported yet");
    }

    auto dst = ImageBuffer::create(dst_format, src.width, src.height, max_bytes);
    if (!dst.ok()) {
        return dst.status();
    }
    ImageBuffer dst_buffer = dst.take_value();
    const MutableView mutable_dst{dst_buffer.data(), dst_buffer.row_stride_bytes()};

    if (same_format) {
        copy_rows(src, mutable_dst);
        return {std::move(dst_buffer)};
    }
    if (to_gray) {
        if (src.format == PixelFormat::kNv12) {
            copy_luma_rows(src, mutable_dst);
        } else {
            interleaved_to_gray(src, mutable_dst);
        }
        return {std::move(dst_buffer)};
    }
    const ChannelLayout dst_layout = layout_of(dst_format);
    const int64_t dst_bpp = bytes_per_pixel(dst_format);
    if (src.format == PixelFormat::kGray8) {
        gray_to_interleaved(src, mutable_dst, dst_layout, dst_bpp);
    } else if (src.format == PixelFormat::kNv12) {
        nv12_to_interleaved(src, mutable_dst, dst_layout, dst_bpp);
    } else {
        interleaved_to_interleaved(src, mutable_dst, dst_layout, dst_bpp);
    }
    return {std::move(dst_buffer)};
}

}  // namespace mirador
