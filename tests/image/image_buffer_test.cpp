#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

using mirador::bytes_per_pixel;
using mirador::ErrorCode;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::kMaxImageDimension;
using mirador::PixelFormat;
using mirador::Rotation;
using mirador::validate;

TEST(ImageBuffer, CreateTightPackedLayout) {
    auto buffer = ImageBuffer::create(PixelFormat::kRgba8, 4, 3, 4096);
    ASSERT_TRUE(buffer.ok());
    EXPECT_EQ(buffer.value().byte_size(), int64_t{4} * 4 * 3);
    EXPECT_EQ(buffer.value().row_stride_bytes(), 16);
    EXPECT_FALSE(buffer.value().empty());

    const ImageView view = buffer.value().view();
    EXPECT_TRUE(validate(view).ok());
    EXPECT_EQ(view.width, 4);
    EXPECT_EQ(view.height, 3);
    EXPECT_EQ(view.format, PixelFormat::kRgba8);
    EXPECT_EQ(view.rotation, Rotation::k0);
    EXPECT_EQ(view.secondary_plane.data, nullptr);
    // Zero-initialized so downstream readers never observe indeterminate bytes.
    for (int64_t offset = 0; offset < view.row_stride_bytes * 3; ++offset) {
        ASSERT_EQ(view.data[offset], std::byte{0}) << "offset " << offset;
    }
}

TEST(ImageBuffer, Nv12LayoutFollowsDec007) {
    auto buffer = ImageBuffer::create(PixelFormat::kNv12, 7, 9, 4096);
    ASSERT_TRUE(buffer.ok());
    // Luma rows 7 * 9 = 63 bytes, chroma rows width * ceil(9 / 2) = 35 bytes.
    EXPECT_EQ(buffer.value().byte_size(), 63 + 35);

    const ImageView view = buffer.value().view();
    EXPECT_TRUE(validate(view).ok());
    EXPECT_EQ(view.row_stride_bytes, 7);
    ASSERT_NE(view.secondary_plane.data, nullptr);
    EXPECT_EQ(view.secondary_plane.data, view.data + 63);
    EXPECT_EQ(view.secondary_plane.row_stride_bytes, 7);
}

TEST(ImageBuffer, EnforcesExplicitByteBudget) {
    const auto too_small = ImageBuffer::create(PixelFormat::kRgba8, 4, 3, 47);
    ASSERT_FALSE(too_small.ok());
    EXPECT_EQ(too_small.status().code(), ErrorCode::kBudgetExceeded);

    const auto exact = ImageBuffer::create(PixelFormat::kRgba8, 4, 3, 48);
    EXPECT_TRUE(exact.ok());

    const auto nv12_too_small = ImageBuffer::create(PixelFormat::kNv12, 7, 9, 97);
    ASSERT_FALSE(nv12_too_small.ok());
    EXPECT_EQ(nv12_too_small.status().code(), ErrorCode::kBudgetExceeded);
}

TEST(ImageBuffer, RejectsInvalidRequestsBeforeAllocating) {
    const auto zero_width = ImageBuffer::create(PixelFormat::kRgb8, 0, 4, 4096);
    ASSERT_FALSE(zero_width.ok());
    EXPECT_EQ(zero_width.status().code(), ErrorCode::kInvalidArgument);

    const auto negative_height = ImageBuffer::create(PixelFormat::kRgb8, 4, -1, 4096);
    ASSERT_FALSE(negative_height.ok());
    EXPECT_EQ(negative_height.status().code(), ErrorCode::kInvalidArgument);

    const auto beyond_limit = ImageBuffer::create(PixelFormat::kGray8, 4, kMaxImageDimension + 1, INT64_MAX);
    ASSERT_FALSE(beyond_limit.ok());
    EXPECT_EQ(beyond_limit.status().code(), ErrorCode::kInvalidArgument);

    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): intentionally out of range
    const auto bad_format = ImageBuffer::create(static_cast<PixelFormat>(99), 4, 4, 4096);
    ASSERT_FALSE(bad_format.ok());
    EXPECT_EQ(bad_format.status().code(), ErrorCode::kInvalidArgument);
}

TEST(ImageBuffer, MoveOnlyWithValidViewAfterMove) {
    auto buffer = ImageBuffer::create(PixelFormat::kBgr8, 3, 2, 4096);
    ASSERT_TRUE(buffer.ok());
    const auto* original_data = buffer.value().view().data;

    const ImageBuffer moved = std::move(buffer.take_value());
    EXPECT_TRUE(moved.empty() == false);
    EXPECT_EQ(moved.view().data, original_data);
    EXPECT_EQ(moved.byte_size(), int64_t{3} * 3 * 2);
    EXPECT_TRUE(validate(moved.view()).ok());
}

TEST(ImageBuffer, EmptyBufferProjectsInvalidView) {
    const ImageBuffer buffer;
    EXPECT_TRUE(buffer.empty());
    const auto* data = buffer.view().data;
    EXPECT_EQ(data, nullptr);
    EXPECT_FALSE(validate(buffer.view()).ok());
    EXPECT_EQ(buffer.byte_size(), 0);
    EXPECT_EQ(buffer.width(), 0);
    EXPECT_EQ(bytes_per_pixel(buffer.format()), 1);  // default format is defined
}

}  // namespace
