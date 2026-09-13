#include <mirador/color_convert.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::bytes_per_pixel;
using mirador::convert_color;
using mirador::ErrorCode;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::PixelFormat;
using mirador::validate;

constexpr int64_t kBudget = 1 << 20;

struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
    friend bool operator==(const Rgba& lhs, const Rgba& rhs) = default;
};

bool has_alpha(PixelFormat format) {
    return format == PixelFormat::kRgba8 || format == PixelFormat::kBgra8;
}

void write_pixel(std::byte* base, int64_t stride, PixelFormat format, int32_t x, int32_t y, const Rgba& pixel) {
    std::byte* row = base + static_cast<int64_t>(y) * stride;
    switch (format) {
        case PixelFormat::kRgb8:
            row[x * 3 + 0] = static_cast<std::byte>(pixel.r);
            row[x * 3 + 1] = static_cast<std::byte>(pixel.g);
            row[x * 3 + 2] = static_cast<std::byte>(pixel.b);
            return;
        case PixelFormat::kBgr8:
            row[x * 3 + 0] = static_cast<std::byte>(pixel.b);
            row[x * 3 + 1] = static_cast<std::byte>(pixel.g);
            row[x * 3 + 2] = static_cast<std::byte>(pixel.r);
            return;
        case PixelFormat::kRgba8:
            row[x * 4 + 0] = static_cast<std::byte>(pixel.r);
            row[x * 4 + 1] = static_cast<std::byte>(pixel.g);
            row[x * 4 + 2] = static_cast<std::byte>(pixel.b);
            row[x * 4 + 3] = static_cast<std::byte>(pixel.a);
            return;
        case PixelFormat::kBgra8:
            row[x * 4 + 0] = static_cast<std::byte>(pixel.b);
            row[x * 4 + 1] = static_cast<std::byte>(pixel.g);
            row[x * 4 + 2] = static_cast<std::byte>(pixel.r);
            row[x * 4 + 3] = static_cast<std::byte>(pixel.a);
            return;
        case PixelFormat::kGray8:
        case PixelFormat::kNv12:
            break;
    }
    FAIL() << "write_pixel used with non-interleaved format";
}

Rgba read_pixel(const ImageView& view, int32_t x, int32_t y) {
    const std::byte* row = view.data + static_cast<int64_t>(y) * view.row_stride_bytes;
    switch (view.format) {
        case PixelFormat::kRgb8:
            return {std::to_integer<uint8_t>(row[x * 3 + 0]), std::to_integer<uint8_t>(row[x * 3 + 1]),
                    std::to_integer<uint8_t>(row[x * 3 + 2]), 255};
        case PixelFormat::kBgr8:
            return {std::to_integer<uint8_t>(row[x * 3 + 2]), std::to_integer<uint8_t>(row[x * 3 + 1]),
                    std::to_integer<uint8_t>(row[x * 3 + 0]), 255};
        case PixelFormat::kRgba8:
            return {std::to_integer<uint8_t>(row[x * 4 + 0]), std::to_integer<uint8_t>(row[x * 4 + 1]),
                    std::to_integer<uint8_t>(row[x * 4 + 2]), std::to_integer<uint8_t>(row[x * 4 + 3])};
        case PixelFormat::kBgra8:
            return {std::to_integer<uint8_t>(row[x * 4 + 2]), std::to_integer<uint8_t>(row[x * 4 + 1]),
                    std::to_integer<uint8_t>(row[x * 4 + 0]), std::to_integer<uint8_t>(row[x * 4 + 3])};
        case PixelFormat::kGray8:
        case PixelFormat::kNv12:
            break;
    }
    ADD_FAILURE() << "read_pixel used with non-interleaved format";
    return {};
}

/// Test-owned interleaved source image: writable storage plus a read-only view.
struct TestImage {
    std::vector<std::byte> bytes;
    int32_t width = 0;
    int32_t height = 0;
    int64_t row_stride_bytes = 0;
    PixelFormat format = PixelFormat::kRgb8;
};

ImageView view_of(const TestImage& image) {
    ImageView image_view;
    image_view.data = image.bytes.data();
    image_view.width = image.width;
    image_view.height = image.height;
    image_view.row_stride_bytes = image.row_stride_bytes;
    image_view.format = image.format;
    return image_view;
}

TestImage make_filled_interleaved(PixelFormat format, int32_t width, int32_t height, int64_t stride,
                                  const std::vector<Rgba>& pixels) {
    TestImage image;
    image.width = width;
    image.height = height;
    image.row_stride_bytes = stride;
    image.format = format;
    image.bytes.assign(static_cast<size_t>(stride) * height, std::byte{0});
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            write_pixel(image.bytes.data(), stride, format, x, y, pixels.at(static_cast<size_t>(y) * width + x));
        }
    }
    return image;
}

TestImage make_filled_interleaved(PixelFormat format, int32_t width, int32_t height, const std::vector<Rgba>& pixels) {
    const auto stride = static_cast<int64_t>(width) * bytes_per_pixel(format);
    return make_filled_interleaved(format, width, height, stride, pixels);
}

TEST(ColorConvert, SameFormatCopyHonorsLooseStrides) {
    const TestImage src = make_filled_interleaved(
        PixelFormat::kRgb8, 3, 2, 10,
        {Rgba{10, 20, 30}, Rgba{11, 21, 31}, Rgba{12, 22, 32}, Rgba{13, 23, 33}, Rgba{14, 24, 34}, Rgba{15, 25, 35}});
    auto dst = convert_color(view_of(src), PixelFormat::kRgb8, kBudget);
    ASSERT_TRUE(dst.ok());

    const ImageView dst_view = dst.value().view();
    EXPECT_TRUE(validate(dst_view).ok());
    for (int32_t i = 0; i < 6; ++i) {
        const Rgba pixel = read_pixel(dst_view, i % 3, i / 3);
        EXPECT_EQ(pixel.r, static_cast<uint8_t>(10 + i)) << "pixel " << i;
        EXPECT_EQ(pixel.g, static_cast<uint8_t>(20 + i)) << "pixel " << i;
        EXPECT_EQ(pixel.b, static_cast<uint8_t>(30 + i)) << "pixel " << i;
    }
}

TEST(ColorConvert, SameFormatNv12CopyIncludesChroma) {
    auto src = ImageBuffer::create(PixelFormat::kNv12, 4, 3, kBudget);
    ASSERT_TRUE(src.ok());
    ImageBuffer src_buffer = src.take_value();
    std::byte* src_bytes = src_buffer.data();
    const auto luma_bytes = int64_t{4} * 3;
    for (int64_t i = 0; i < luma_bytes; ++i) {
        src_bytes[i] = static_cast<std::byte>(i);
    }
    for (int64_t i = 0; i < 8; ++i) {  // chroma rows: width * ceil(3 / 2) = 8 bytes
        src_bytes[luma_bytes + i] = static_cast<std::byte>(100 + i);
    }

    auto dst = convert_color(src_buffer.view(), PixelFormat::kNv12, kBudget);
    ASSERT_TRUE(dst.ok());
    const std::byte* dst_bytes = dst.value().view().data;
    for (int64_t i = 0; i < luma_bytes; ++i) {
        EXPECT_EQ(dst_bytes[i], static_cast<std::byte>(i)) << "luma byte " << i;
    }
    for (int64_t i = 0; i < 8; ++i) {
        EXPECT_EQ(dst_bytes[luma_bytes + i], static_cast<std::byte>(100 + i)) << "chroma byte " << i;
    }
}

TEST(ColorConvert, ColorToGrayUsesDocumentedBt601Luma) {
    // Luma constants from the header formula: R->77, G->149, B->29, white->255.
    const TestImage src = make_filled_interleaved(
        PixelFormat::kRgba8, 4, 1,
        {Rgba{255, 0, 0, 255}, Rgba{0, 255, 0, 255}, Rgba{0, 0, 255, 255}, Rgba{255, 255, 255, 255}});
    auto dst = convert_color(view_of(src), PixelFormat::kGray8, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(dst.value().format(), PixelFormat::kGray8);

    const std::byte* row = dst.value().view().data;
    EXPECT_EQ(std::to_integer<uint8_t>(row[0]), 77);
    EXPECT_EQ(std::to_integer<uint8_t>(row[1]), 149);
    EXPECT_EQ(std::to_integer<uint8_t>(row[2]), 29);
    EXPECT_EQ(std::to_integer<uint8_t>(row[3]), 255);
}

TEST(ColorConvert, OddWidthLooseStrideToGray) {
    std::vector<Rgba> pixels;
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 5; ++x) {
            pixels.push_back(Rgba{static_cast<uint8_t>(x * 10), static_cast<uint8_t>(y * 10), 0, 255});
        }
    }
    const TestImage src = make_filled_interleaved(PixelFormat::kBgr8, 5, 3, 17, pixels);

    auto dst = convert_color(view_of(src), PixelFormat::kGray8, kBudget);
    ASSERT_TRUE(dst.ok());
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 5; ++x) {
            const int32_t expected = (77 * (x * 10) + 150 * (y * 10) + 128) >> 8;
            const std::byte* row = dst.value().view().data + y * dst.value().row_stride_bytes();
            EXPECT_EQ(std::to_integer<uint8_t>(row[x]), expected) << "x=" << x << " y=" << y;
        }
    }
}

// Converts one ordered format pair and checks the first output pixel.
void verify_remap_pair(PixelFormat src_format, PixelFormat dst_format, const std::vector<Rgba>& pixels) {
    const TestImage src = make_filled_interleaved(src_format, 2, 2, pixels);
    auto dst = convert_color(view_of(src), dst_format, kBudget);
    ASSERT_TRUE(dst.ok());
    uint8_t expected_alpha = 255;
    if (has_alpha(dst_format) && has_alpha(src_format)) {
        expected_alpha = 40;  // alpha carried over from the source pixel
    }
    const Rgba pixel = read_pixel(dst.value().view(), 0, 0);
    EXPECT_EQ(pixel.r, 10);
    EXPECT_EQ(pixel.g, 20);
    EXPECT_EQ(pixel.b, 30);
    EXPECT_EQ(pixel.a, expected_alpha);
}

TEST(ColorConvert, InterleavedRemapMatrixPreservesChannels) {
    const std::array<PixelFormat, 4> kFormats = {PixelFormat::kRgb8, PixelFormat::kBgr8, PixelFormat::kRgba8,
                                                 PixelFormat::kBgra8};
    const std::vector<Rgba> kPixels = {Rgba{10, 20, 30, 40}, Rgba{50, 60, 70, 80}, Rgba{90, 100, 110, 120},
                                       Rgba{130, 140, 150, 160}};
    // Re-encode identical pixels through every source layout so each ordered pair
    // is exercised with the same logical content.
    for (const PixelFormat src_format : kFormats) {
        for (const PixelFormat dst_format : kFormats) {
            verify_remap_pair(src_format, dst_format, kPixels);
        }
    }
}

TEST(ColorConvert, GrayToInterleavedReplicatesAndFillsAlpha) {
    TestImage src;
    src.width = 2;
    src.height = 1;
    src.row_stride_bytes = 4;  // loose gray stride
    src.format = PixelFormat::kGray8;
    src.bytes.assign(4, std::byte{0});
    src.bytes[0] = static_cast<std::byte>(7);
    src.bytes[1] = static_cast<std::byte>(250);

    auto dst = convert_color(view_of(src), PixelFormat::kBgra8, kBudget);
    ASSERT_TRUE(dst.ok());
    const Rgba first = read_pixel(dst.value().view(), 0, 0);
    EXPECT_EQ(first.r, 7);
    EXPECT_EQ(first.g, 7);
    EXPECT_EQ(first.b, 7);
    EXPECT_EQ(first.a, 255);
}

TEST(ColorConvert, Nv12ToInterleavedAppliesDocumentedYuvMatrix) {
    auto src = ImageBuffer::create(PixelFormat::kNv12, 4, 2, kBudget);
    ASSERT_TRUE(src.ok());
    ImageBuffer src_buffer = src.take_value();
    std::byte* src_bytes = src_buffer.data();
    // (0,0) neutral chroma luma 255 -> white; (1,0) neutral chroma luma 0 -> black;
    // row 1 samples U=0, V=255 (u'=-128, v'=127) with luma 100: the header
    // coefficients give r=clamp(278)=255, g=100-46=54, b=clamp(-127)=0.
    const std::array<std::byte, 8> luma = {static_cast<std::byte>(255), static_cast<std::byte>(0),
                                           static_cast<std::byte>(100), static_cast<std::byte>(100),
                                           static_cast<std::byte>(100), static_cast<std::byte>(100),
                                           static_cast<std::byte>(100), static_cast<std::byte>(100)};
    // 4x2 NV12 has exactly one chroma row of 4 bytes: two UV pairs, one neutral
    // and one with U=0, V=255 (u'=-128, v'=127).
    const std::array<std::byte, 4> chroma = {static_cast<std::byte>(128), static_cast<std::byte>(128),
                                             static_cast<std::byte>(0), static_cast<std::byte>(255)};
    for (int32_t i = 0; i < 8; ++i) {
        src_bytes[i] = luma[i];  // luma plane is width * height = 8 bytes
    }
    for (int32_t i = 0; i < 4; ++i) {
        src_bytes[8 + i] = chroma[i];
    }

    auto dst = convert_color(src_buffer.view(), PixelFormat::kRgb8, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(read_pixel(dst.value().view(), 0, 0), (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(read_pixel(dst.value().view(), 1, 0), (Rgba{0, 0, 0, 255}));
    // The single chroma row serves both luma rows: x=2 samples the second pair
    // (U=0, V=255), x=0 samples the neutral pair.
    EXPECT_EQ(read_pixel(dst.value().view(), 2, 1), (Rgba{255, 54, 0, 255}));
    EXPECT_EQ(read_pixel(dst.value().view(), 3, 1), (Rgba{255, 54, 0, 255}));
    EXPECT_EQ(read_pixel(dst.value().view(), 0, 1), (Rgba{100, 100, 100, 255}));
}

TEST(ColorConvert, Nv12ToGrayCopiesLumaPlaneOnly) {
    auto src = ImageBuffer::create(PixelFormat::kNv12, 3, 3, kBudget);
    ASSERT_TRUE(src.ok());
    ImageBuffer src_buffer = src.take_value();
    std::byte* src_bytes = src_buffer.data();
    for (int32_t i = 0; i < 9; ++i) {
        src_bytes[i] = static_cast<std::byte>(i * 3);
        // Odd width 3: chroma rows hold 2 UV pairs = 4 bytes, 2 rows = 8 bytes.
        src_bytes[9 + (i % 8)] = static_cast<std::byte>(255);
    }

    auto dst = convert_color(src_buffer.view(), PixelFormat::kGray8, kBudget);
    ASSERT_TRUE(dst.ok());
    const ImageView dst_view = dst.value().view();
    for (int32_t i = 0; i < 9; ++i) {
        EXPECT_EQ(dst_view.data[i], static_cast<std::byte>(i * 3)) << "luma byte " << i;
    }
}

TEST(ColorConvert, OutputIsStrideInvariant) {
    const std::vector<Rgba> kPixels = {Rgba{200, 10, 10, 255},   Rgba{10, 200, 10, 255}, Rgba{10, 10, 200, 255},
                                       Rgba{250, 250, 0, 255},   Rgba{0, 250, 250, 255}, Rgba{250, 0, 250, 255},
                                       Rgba{100, 100, 100, 255}, Rgba{20, 40, 60, 255},  Rgba{80, 160, 240, 255},
                                       Rgba{255, 128, 0, 255},   Rgba{0, 255, 128, 255}, Rgba{128, 0, 255, 255}};
    const TestImage packed = make_filled_interleaved(PixelFormat::kRgb8, 4, 3, kPixels);
    const TestImage loose = make_filled_interleaved(PixelFormat::kRgb8, 4, 3, 19, kPixels);  // min is 12

    auto from_packed = convert_color(view_of(packed), PixelFormat::kGray8, kBudget);
    auto from_loose = convert_color(view_of(loose), PixelFormat::kGray8, kBudget);
    ASSERT_TRUE(from_packed.ok());
    ASSERT_TRUE(from_loose.ok());
    for (int32_t i = 0; i < 12; ++i) {
        EXPECT_EQ(from_packed.value().view().data[i], from_loose.value().view().data[i]) << "byte " << i;
    }
}

TEST(ColorConvert, UnsupportedIntoNv12ReportsUnsupportedFormat) {
    const TestImage src = make_filled_interleaved(PixelFormat::kRgba8, 2, 2, {Rgba{}, Rgba{}, Rgba{}, Rgba{}});
    const auto result = convert_color(view_of(src), PixelFormat::kNv12, kBudget);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kUnsupportedFormat);

    auto gray = ImageBuffer::create(PixelFormat::kGray8, 2, 2, kBudget);
    ASSERT_TRUE(gray.ok());
    const auto gray_to_nv12 = convert_color(gray.value().view(), PixelFormat::kNv12, kBudget);
    ASSERT_FALSE(gray_to_nv12.ok());
    EXPECT_EQ(gray_to_nv12.status().code(), ErrorCode::kUnsupportedFormat);
}

TEST(ColorConvert, RejectsInvalidInputsAndBudget) {
    const TestImage src = make_filled_interleaved(PixelFormat::kRgb8, 2, 2, {Rgba{}, Rgba{}, Rgba{}, Rgba{}});
    // Undefined destination format.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): intentionally out of range
    const auto bad_format = static_cast<PixelFormat>(99);
    const auto format_result = convert_color(view_of(src), bad_format, kBudget);
    ASSERT_FALSE(format_result.ok());
    EXPECT_EQ(format_result.status().code(), ErrorCode::kInvalidArgument);

    // Budget too small for a 2x2 gray result (4 bytes).
    const auto budget_result = convert_color(view_of(src), PixelFormat::kGray8, 3);
    ASSERT_FALSE(budget_result.ok());
    EXPECT_EQ(budget_result.status().code(), ErrorCode::kBudgetExceeded);

    const ImageView invalid;
    const auto invalid_result = convert_color(invalid, PixelFormat::kGray8, kBudget);
    ASSERT_FALSE(invalid_result.ok());
    EXPECT_EQ(invalid_result.status().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
