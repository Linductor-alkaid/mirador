#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::kMaxImageDimension;
using mirador::make_scale;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::resize_area;
using mirador::transform_rect;
using mirador::validate;

constexpr int64_t kBudget = 1 << 20;

/// Tight gray view over caller-owned storage filled by `fill`.
ImageView make_gray_view(std::vector<std::byte>& storage, int32_t width, int32_t height,
                         const std::vector<int32_t>& values) {
    storage.assign(values.size(), std::byte{0});
    for (size_t i = 0; i < values.size(); ++i) {
        storage[i] = static_cast<std::byte>(values[i]);
    }
    ImageView src;
    src.data = storage.data();
    src.width = width;
    src.height = height;
    src.row_stride_bytes = width;
    src.format = PixelFormat::kGray8;
    return src;
}

TEST(ResizeArea, DownscaleByTwoAveragesExactBlocks) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 4, 2, {10, 20, 30, 40, 50, 60, 70, 80});

    auto dst = resize_area(src, 2, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    const ImageView view = dst.value().view();
    ASSERT_TRUE(validate(view).ok());
    EXPECT_EQ(view.format, PixelFormat::kGray8);
    // Each destination pixel is the mean of its 2x2 source block.
    EXPECT_EQ(std::to_integer<int32_t>(view.data[0]), 35);
    EXPECT_EQ(std::to_integer<int32_t>(view.data[1]), 55);
}

TEST(ResizeArea, NonIntegerRatioUsesExactCoverageWeights) {
    // 3x1 -> 2x1: destination 0 covers [0, 1.5) with weights 2:1 on the columns,
    // destination 1 covers [1.5, 3) with weights 1:2.
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 3, 1, {30, 60, 90});

    auto dst = resize_area(src, 2, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(std::to_integer<int32_t>(dst.value().view().data[0]), 40);  // (2*30 + 60) / 3
    EXPECT_EQ(std::to_integer<int32_t>(dst.value().view().data[1]), 80);  // (60 + 2*90) / 3
}

TEST(ResizeArea, UpscaleReplicatesCoverageExactly) {
    // 2x1 -> 4x1: destination pixels fully inside a source column replicate it.
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 2, 1, {10, 20});

    auto dst = resize_area(src, 4, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    const std::vector<int32_t> expected = {10, 10, 20, 20};
    for (int32_t x = 0; x < 4; ++x) {
        EXPECT_EQ(std::to_integer<int32_t>(dst.value().view().data[x]), expected[x]) << "x=" << x;
    }
}

/// Collapses an image to its single-pixel 1x1 area average.
int32_t single_pixel_average(const ImageView& src) {
    auto dst = resize_area(src, 1, 1, kBudget);
    EXPECT_TRUE(dst.ok());
    if (!dst.ok()) {
        return -1;
    }
    return std::to_integer<int32_t>(dst.value().view().data[0]);
}

TEST(ResizeArea, RoundsHalfUpOnFractionalAverages) {
    // 2x1 -> 1x1 of {1, 2}: average is 1.5 and rounds up to 2; {0, 1} averages
    // 0.5 and rounds up to 1; {1, 1} is exact and stays 1.
    std::vector<std::byte> storage;
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {1, 2})), 2);
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {0, 1})), 1);
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {1, 1})), 1);
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {254, 255})), 255);  // clamped
}

TEST(ResizeArea, OutputIsStrideInvariant) {
    const std::vector<int32_t> kValues = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    std::vector<std::byte> tight_storage;
    const ImageView tight = make_gray_view(tight_storage, 4, 3, kValues);

    // Same logical pixels on a loose stride of 7 (minimum is 4).
    std::vector<std::byte> loose_storage(21, std::byte{0});  // 3 loose rows of 7 bytes
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            loose_storage[static_cast<size_t>(y) * 7 + x] = static_cast<std::byte>(kValues[y * 4 + x]);
        }
    }
    ImageView loose;
    loose.data = loose_storage.data();
    loose.width = 4;
    loose.height = 3;
    loose.row_stride_bytes = 7;
    loose.format = PixelFormat::kGray8;

    auto from_tight = resize_area(tight, 2, 2, kBudget);
    auto from_loose = resize_area(loose, 2, 2, kBudget);
    ASSERT_TRUE(from_tight.ok());
    ASSERT_TRUE(from_loose.ok());
    for (int32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(from_tight.value().view().data[i], from_loose.value().view().data[i]) << "byte " << i;
    }
}

TEST(ResizeArea, SameSizeRequestIsACopy) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 3, 2, {5, 10, 15, 20, 25, 30});

    auto dst = resize_area(src, 3, 2, kBudget);
    ASSERT_TRUE(dst.ok());
    for (int32_t i = 0; i < 6; ++i) {
        EXPECT_EQ(dst.value().view().data[i], static_cast<std::byte>(5 * (i + 1))) << "byte " << i;
    }
}

/// 2x2 RGB on a padded stride: (0,0)=red, (1,0)=green, (0,1)=blue, (1,1)=white.
ImageView make_rgb_quad(std::vector<std::byte>& storage) {
    storage.assign(12, std::byte{0});  // 2 rows of stride 6
    const std::vector<std::vector<int32_t>> pixels = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int32_t p = 0; p < 4; ++p) {
        const auto base = static_cast<size_t>(p / 2) * 6 + static_cast<size_t>(p % 2) * 3;
        for (int32_t c = 0; c < 3; ++c) {
            storage[base + static_cast<size_t>(c)] = static_cast<std::byte>(pixels[p][c]);
        }
    }
    ImageView src;
    src.data = storage.data();
    src.width = 2;
    src.height = 2;
    src.row_stride_bytes = 6;
    src.format = PixelFormat::kRgb8;
    return src;
}

TEST(ResizeArea, InterleavedResizeAveragesChannelsIndependently) {
    std::vector<std::byte> storage;
    const ImageView src = make_rgb_quad(storage);
    ASSERT_TRUE(validate(src).ok());

    auto dst = resize_area(src, 1, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(dst.value().format(), PixelFormat::kRgb8);
    const std::byte* pixel = dst.value().view().data;
    // Averages are 127.5; the documented half-up rounding yields 128.
    EXPECT_EQ(std::to_integer<int32_t>(pixel[0]), 128);
    EXPECT_EQ(std::to_integer<int32_t>(pixel[1]), 128);
    EXPECT_EQ(std::to_integer<int32_t>(pixel[2]), 128);
}

TEST(ResizeArea, Nv12SourceProducesGrayThumbnail) {
    auto src = ImageBuffer::create(PixelFormat::kNv12, 4, 4, kBudget);
    ASSERT_TRUE(src.ok());
    ImageBuffer buffer = src.take_value();
    std::byte* bytes = buffer.data();
    for (int32_t i = 0; i < 16; ++i) {
        bytes[i] = static_cast<std::byte>(i * 4);
    }
    for (int32_t i = 0; i < 8; ++i) {  // chroma: stride 4 x ceil(4 / 2) rows = 8 bytes
        bytes[16 + i] = static_cast<std::byte>(200);
    }

    auto dst = resize_area(buffer.view(), 2, 2, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(dst.value().format(), PixelFormat::kGray8);
    // 2x2 luma block averages of the 4x4 luma plane.
    const std::byte* luma = dst.value().view().data;
    EXPECT_EQ(std::to_integer<int32_t>(luma[0]), (0 + 4 + 16 + 20) / 4);
    EXPECT_EQ(std::to_integer<int32_t>(luma[1]), (8 + 12 + 24 + 28) / 4);
}

TEST(ResizeArea, ScaleTransformRecoversSourceRect) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 8, 6, std::vector<int32_t>(48, 7));
    auto dst = resize_area(src, 4, 3, kBudget);
    ASSERT_TRUE(dst.ok());

    // RULE-05: the resize is exactly the scale (sw/dw, sh/dh) in coordinate terms.
    const auto scale = make_scale(8.0 / 4.0, 6.0 / 3.0, CoordinateSpaceId::kModelInput, CoordinateSpaceId::kOriented);
    const RectF recovered = transform_rect(scale, RectF{0.0F, 0.0F, 4.0F, 3.0F});
    EXPECT_NEAR(recovered.x, 0.0, 1e-3);
    EXPECT_NEAR(recovered.y, 0.0, 1e-3);
    EXPECT_NEAR(recovered.width, 8.0, 1e-3);
    EXPECT_NEAR(recovered.height, 6.0, 1e-3);
}

void expect_resize_rejected(const ImageView& src, int32_t w, int32_t h) {
    const auto result = resize_area(src, w, h, kBudget);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
}

TEST(ResizeArea, RejectsInvalidDestinationSizes) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 4, 4, std::vector<int32_t>(16, 1));
    expect_resize_rejected(src, 0, 2);
    expect_resize_rejected(src, 2, 0);
    expect_resize_rejected(src, -1, 2);
    expect_resize_rejected(src, kMaxImageDimension + 1, 2);
}

TEST(ResizeArea, RejectsSmallBudgetsAndInvalidViews) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 4, 4, std::vector<int32_t>(16, 1));

    const auto budget = resize_area(src, 2, 2, 3);  // needs 4 bytes
    ASSERT_FALSE(budget.ok());
    EXPECT_EQ(budget.status().code(), ErrorCode::kBudgetExceeded);

    const auto invalid_view = resize_area(ImageView{}, 2, 2, kBudget);
    ASSERT_FALSE(invalid_view.ok());
    EXPECT_EQ(invalid_view.status().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
