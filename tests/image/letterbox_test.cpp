// M3-04: unit tests for the pixel-space letterbox combinator (design sections
// 7, 14; DEC-014 reference adaptation scope).

#include <cstddef>
#include <cstdint>
#include <mirador/image_buffer.hpp>
#include <mirador/letterbox.hpp>
#include <mirador/resize.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>
#include "mirador/geometry.hpp"
#include "mirador/image_view.hpp"
#include "mirador/pixel_format.hpp"
#include "mirador/status.hpp"
#include "mirador/transform.hpp"

namespace {

using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::LetterboxRequest;
using mirador::PixelFormat;
using mirador::PointF;

/// Test-owned gray ramp buffer plus a free projection to a valid view.
struct GrayImage {
    std::vector<uint8_t> pixels;
    int32_t width = 0;
    int32_t height = 0;
};

[[nodiscard]] ImageView gray_view(const GrayImage& image) {
    ImageView v;
    v.data = reinterpret_cast<const std::byte*>(image.pixels.data());
    v.width = image.width;
    v.height = image.height;
    v.row_stride_bytes = image.width;
    v.format = PixelFormat::kGray8;
    return v;
}

[[nodiscard]] GrayImage make_ramp(int32_t width, int32_t height) {
    GrayImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            image.pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                static_cast<uint8_t>((x * 7 + y * 13) % 256);
        }
    }
    return image;
}

TEST(Letterbox, SameAspectRatioFillsWithoutPadding) {
    const GrayImage src = make_ramp(8, 8);
    LetterboxRequest request;
    request.dst_width = 4;
    request.dst_height = 4;
    request.pad_value = 42;
    const auto result = mirador::letterbox(gray_view(src), request, 4096);
    ASSERT_TRUE(result.ok()) << result.status().message();

    EXPECT_EQ(result.value().resized_width, 4);
    EXPECT_EQ(result.value().resized_height, 4);
    ASSERT_EQ(result.value().buffer.format(), PixelFormat::kGray8);
    ASSERT_EQ(result.value().buffer.width(), 4);
    ASSERT_EQ(result.value().buffer.height(), 4);

    // The content covers the whole destination; no padded byte remains.
    const auto expected = mirador::resize_area(gray_view(src), 4, 4, 4096);
    ASSERT_TRUE(expected.ok());
    EXPECT_EQ(std::memcmp(result.value().buffer.view().data, expected.value().view().data, 16), 0);

    // Forward transform scales by 0.5; inverse recovers exactly.
    const PointF model = mirador::transform_point(result.value().transform, PointF{4.0F, 6.0F});
    EXPECT_NEAR(model.x, 2.0, 1e-6);
    EXPECT_NEAR(model.y, 3.0, 1e-6);
    const auto back = mirador::inverse(result.value().transform);
    ASSERT_TRUE(back.ok());
    const PointF recovered = mirador::transform_point(back.value(), model);
    EXPECT_NEAR(recovered.x, 4.0, 1e-6);
    EXPECT_NEAR(recovered.y, 6.0, 1e-6);
}

TEST(Letterbox, CentersContentAndFillsPadValue) {
    const GrayImage src = make_ramp(8, 4);
    LetterboxRequest request;
    request.dst_width = 8;
    request.dst_height = 8;
    request.pad_value = 7;
    const auto result = mirador::letterbox(gray_view(src), request, 4096);
    ASSERT_TRUE(result.ok()) << result.status().message();
    // scale = min(1, 2) = 1: content 8x4 centered vertically (rows 2..5).
    ASSERT_EQ(result.value().resized_width, 8);
    ASSERT_EQ(result.value().resized_height, 4);

    const ImageView view = result.value().buffer.view();
    const auto byte_at = [view](int32_t x, int32_t y) {
        return *(view.data + static_cast<int64_t>(y) * view.row_stride_bytes + x);
    };
    for (int32_t x = 0; x < 8; ++x) {
        EXPECT_EQ(byte_at(x, 0), std::byte{7});
        EXPECT_EQ(byte_at(x, 1), std::byte{7});
        EXPECT_EQ(byte_at(x, 6), std::byte{7});
        EXPECT_EQ(byte_at(x, 7), std::byte{7});
    }
    const auto direct = mirador::resize_area(gray_view(src), 8, 4, 4096);
    ASSERT_TRUE(direct.ok());
    const ImageView direct_view = direct.value().view();
    for (int32_t y = 0; y < 4; ++y) {
        for (int32_t x = 0; x < 8; ++x) {
            EXPECT_EQ(byte_at(x, y + 2), *(direct_view.data + y * direct_view.row_stride_bytes + x));
        }
    }

    const PointF mapped = mirador::transform_point(result.value().transform, PointF{0.0F, 0.0F});
    EXPECT_NEAR(mapped.x, 0.0, 1e-6);
    EXPECT_NEAR(mapped.y, 2.0, 1e-6);
}

namespace {

/// Builds the RGB source view of the MultiChannel scenario (4x2, tight-ish stride 12).
[[nodiscard]] ImageView make_rgb_source(std::vector<uint8_t>& rgb) {
    rgb.assign(48U, 0);
    for (size_t i = 0; i < rgb.size(); ++i) {
        rgb[i] = static_cast<uint8_t>(i % 251);
    }
    ImageView src;
    src.data = reinterpret_cast<const std::byte*>(rgb.data());
    src.width = 4;
    src.height = 2;
    src.row_stride_bytes = 12;
    src.format = PixelFormat::kRgb8;
    return src;
}

[[nodiscard]] mirador::LetterboxRequest rgb_request() {
    LetterboxRequest request;
    request.dst_width = 8;
    request.dst_height = 8;
    request.pad_value = 200;
    return request;
}

}  // namespace

TEST(Letterbox, MultiChannelPadRowsUsePadValueOnEveryChannel) {
    std::vector<uint8_t> rgb;
    const ImageView src = make_rgb_source(rgb);
    const auto result = mirador::letterbox(src, rgb_request(), 4096);
    ASSERT_TRUE(result.ok()) << result.status().message();

    // Scale 2, content 8x4 at offset (0, 2): rows 0-1 and 6-7 are padding.
    const ImageView view = result.value().buffer.view();
    for (const int32_t y : {0, 1, 6, 7}) {
        for (int32_t x = 0; x < 8; ++x) {
            for (int32_t c = 0; c < 3; ++c) {
                EXPECT_EQ(
                    *(view.data + static_cast<int64_t>(y) * view.row_stride_bytes + static_cast<int64_t>(x) * 3 + c),
                    std::byte{200});
            }
        }
    }
}

TEST(Letterbox, MultiChannelContentRowsReplicateSource) {
    std::vector<uint8_t> rgb;
    const ImageView src = make_rgb_source(rgb);
    const auto result = mirador::letterbox(src, rgb_request(), 4096);
    ASSERT_TRUE(result.ok()) << result.status().message();

    // Same-size area resize is a row copy: each source pixel appears four
    // times (2x2) at offset (0, 2).
    const ImageView view = result.value().buffer.view();
    for (int32_t y = 0; y < 2; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            for (int32_t c = 0; c < 3; ++c) {
                const size_t src_byte = static_cast<size_t>(y) * 12U + static_cast<size_t>(x) * 3U + c;
                const auto dst_byte = static_cast<size_t>(y * 2 + 2) * static_cast<size_t>(view.row_stride_bytes) +
                                      static_cast<size_t>(x * 2) * 3U + c;
                EXPECT_EQ(*(view.data + static_cast<int64_t>(dst_byte)), std::byte{rgb[src_byte]});
            }
        }
    }
}

TEST(Letterbox, TransformMatchesContinuousIdealWithinOnePixel) {
    const GrayImage src = make_ramp(6, 4);
    LetterboxRequest request;
    request.dst_width = 12;
    request.dst_height = 10;
    const auto result = mirador::letterbox(gray_view(src), request, 4096);
    ASSERT_TRUE(result.ok());
    const auto ideal =
        mirador::make_letterbox(6, 4, 12, 10, CoordinateSpaceId::kOriented, CoordinateSpaceId::kModelInput);
    ASSERT_TRUE(ideal.ok());
    for (const PointF& corner : {PointF{0.0F, 0.0F}, PointF{6.0F, 0.0F}, PointF{6.0F, 4.0F}, PointF{0.0F, 4.0F}}) {
        const PointF actual = mirador::transform_point(result.value().transform, corner);
        const PointF expected = mirador::transform_point(ideal.value(), corner);
        EXPECT_LE(std::fabs(actual.x - expected.x), 1.0);
        EXPECT_LE(std::fabs(actual.y - expected.y), 1.0);
    }
}

TEST(Letterbox, DeterministicAndInputPreserving) {
    const GrayImage src = make_ramp(9, 5);
    LetterboxRequest request;
    request.dst_width = 12;
    request.dst_height = 12;
    request.pad_value = 1;
    const std::vector<uint8_t> src_copy = src.pixels;
    const auto first = mirador::letterbox(gray_view(src), request, 8192);
    const auto second = mirador::letterbox(gray_view(src), request, 8192);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(first.value().buffer.byte_size(), second.value().buffer.byte_size());
    EXPECT_EQ(std::memcmp(first.value().buffer.view().data, second.value().buffer.view().data,
                          static_cast<size_t>(first.value().buffer.byte_size())),
              0);
    EXPECT_EQ(src.pixels, src_copy);  // RULE-04
}

TEST(Letterbox, ExplicitErrors) {
    const GrayImage src = make_ramp(8, 8);

    LetterboxRequest zero;
    zero.dst_width = 0;
    zero.dst_height = 4;
    ASSERT_EQ(mirador::letterbox(gray_view(src), zero, 4096).status().code(), ErrorCode::kInvalidArgument);

    LetterboxRequest huge;
    huge.dst_width = 70000;
    huge.dst_height = 4;
    ASSERT_EQ(mirador::letterbox(gray_view(src), huge, 1 << 30).status().code(), ErrorCode::kInvalidArgument);

    LetterboxRequest request;
    request.dst_width = 8;
    request.dst_height = 8;
    ASSERT_EQ(mirador::letterbox(gray_view(src), request, 16).status().code(), ErrorCode::kBudgetExceeded);

    ImageView const invalid;
    ASSERT_EQ(mirador::letterbox(invalid, request, 4096).status().code(), ErrorCode::kInvalidArgument);

    // NV12 rejected pending the chroma padding decision (DEC-014).
    auto nv12 = ImageBuffer::create(PixelFormat::kNv12, 8, 8, 4096);
    ASSERT_TRUE(nv12.ok());
    ASSERT_EQ(mirador::letterbox(nv12.value().view(), request, 4096).status().code(), ErrorCode::kUnsupportedFormat);
}

}  // namespace
