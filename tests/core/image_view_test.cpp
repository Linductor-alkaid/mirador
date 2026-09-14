#include <mirador/frame.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using mirador::bytes_per_pixel;
using mirador::ErrorCode;
using mirador::Frame;
using mirador::ImagePlane;
using mirador::ImageView;
using mirador::is_valid;
using mirador::kMaxImageDimension;
using mirador::min_row_stride_bytes;
using mirador::PixelFormat;
using mirador::plane_count;
using mirador::Rotation;
using mirador::validate;

TEST(PixelFormat, TraitsMatchDesignLayouts) {
    EXPECT_EQ(plane_count(PixelFormat::kGray8), 1);
    EXPECT_EQ(plane_count(PixelFormat::kRgb8), 1);
    EXPECT_EQ(plane_count(PixelFormat::kBgr8), 1);
    EXPECT_EQ(plane_count(PixelFormat::kRgba8), 1);
    EXPECT_EQ(plane_count(PixelFormat::kBgra8), 1);
    EXPECT_EQ(plane_count(PixelFormat::kNv12), 2);

    EXPECT_EQ(bytes_per_pixel(PixelFormat::kGray8), 1);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::kRgb8), 3);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::kBgr8), 3);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::kRgba8), 4);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::kBgra8), 4);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::kNv12), 1);
}

TEST(PixelFormat, MinimumStrideHandlesOddWidths) {
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kGray8, 0, 7), 7);
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kRgb8, 0, 7), 21);
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kBgra8, 0, 7), 28);
    // NV12 chroma rows hold ceil(7 / 2) = 4 UV pairs, i.e. 8 bytes (DEC-007).
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kNv12, 0, 7), 7);
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kNv12, 1, 7), 8);
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kNv12, 1, 8), 8);
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kRgb8, 1, 7), -1);
    EXPECT_EQ(min_row_stride_bytes(PixelFormat::kGray8, 0, 0), -1);
}

TEST(PixelFormat, RejectsUndefinedEnumValues) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): intentionally out of range
    const auto bad_format = static_cast<PixelFormat>(200);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): intentionally out of range
    const auto bad_rotation = static_cast<Rotation>(45);
    EXPECT_FALSE(is_valid(bad_format));
    EXPECT_FALSE(is_valid(bad_rotation));
    EXPECT_EQ(plane_count(bad_format), 0);
    EXPECT_EQ(bytes_per_pixel(bad_format), 0);
    EXPECT_TRUE(is_valid(Rotation::k0));
    EXPECT_TRUE(is_valid(Rotation::k270));
}

ImageView make_rgba_view(const std::vector<std::byte>& storage, int32_t width, int32_t height, int64_t stride) {
    ImageView view;
    view.data = storage.data();
    view.width = width;
    view.height = height;
    view.row_stride_bytes = stride;
    view.format = PixelFormat::kRgba8;
    return view;
}

TEST(ImageView, AcceptsTightAndLooseStrides) {
    const std::vector<std::byte> storage(4096);
    const auto tight = make_rgba_view(storage, 4, 4, int64_t{4} * 4);
    EXPECT_TRUE(validate(tight).ok());
    const auto loose = make_rgba_view(storage, 4, 4, int64_t{5} * 4);
    EXPECT_TRUE(validate(loose).ok());
}

TEST(ImageView, ValidationMatrix) {
    const std::vector<std::byte> storage(4096);
    const auto expect_invalid = [](const ImageView& view, const char* reason_part) {
        const auto result = validate(view);
        ASSERT_FALSE(result.ok()) << reason_part;
        EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument) << reason_part;
    };

    auto null_data = make_rgba_view(storage, 4, 4, 16);
    null_data.data = nullptr;
    expect_invalid(null_data, "null data");

    expect_invalid(make_rgba_view(storage, 0, 4, 16), "zero width");
    expect_invalid(make_rgba_view(storage, 4, 0, 16), "zero height");
    expect_invalid(make_rgba_view(storage, -4, 4, 16), "negative width");

    auto oversized = make_rgba_view(storage, 4, kMaxImageDimension + 1, 16);
    expect_invalid(oversized, "height beyond maximum");

    expect_invalid(make_rgba_view(storage, 8, 4, int64_t{4} * 4 - 1), "stride below minimum");

    auto bad_format = make_rgba_view(storage, 4, 4, 16);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): intentionally out of range
    bad_format.format = static_cast<PixelFormat>(99);
    expect_invalid(bad_format, "unknown format");

    auto bad_rotation = make_rgba_view(storage, 4, 4, 16);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): intentionally out of range
    bad_rotation.rotation = static_cast<Rotation>(45);
    expect_invalid(bad_rotation, "unknown rotation");
}

TEST(ImageView, Nv12PlaneRulesPerDec007) {
    const std::vector<std::byte> luma(1024);
    const std::vector<std::byte> chroma(1024);

    // Odd height: chroma rows are ceil(height / 2); odd width needs ceil(7 / 2)
    // UV pairs = 8 chroma bytes per row (DEC-007, frozen in M1).
    ImageView nv12;
    nv12.data = luma.data();
    nv12.width = 7;
    nv12.height = 9;
    nv12.row_stride_bytes = 7;
    nv12.format = PixelFormat::kNv12;
    nv12.secondary_plane = ImagePlane{chroma.data(), 8};
    EXPECT_TRUE(validate(nv12).ok());

    auto missing_chroma = nv12;
    missing_chroma.secondary_plane = ImagePlane{};
    const auto result = validate(missing_chroma);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);

    auto small_chroma_stride = nv12;
    small_chroma_stride.secondary_plane = ImagePlane{chroma.data(), 7};
    EXPECT_FALSE(validate(small_chroma_stride).ok());

    auto loose = nv12;
    loose.row_stride_bytes = 32;
    loose.secondary_plane = ImagePlane{chroma.data(), 32};
    EXPECT_TRUE(validate(loose).ok());
}

TEST(ImageView, SinglePlaneFormatMustNotCarrySecondaryPlane) {
    const std::vector<std::byte> storage(4096);
    auto view = make_rgba_view(storage, 4, 4, 16);
    view.secondary_plane = ImagePlane{storage.data(), 16};
    const auto result = validate(view);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
}

TEST(Frame, CopiesShareOwnerLifetime) {
    const auto owner = std::make_shared<const std::vector<std::byte>>(4096);
    Frame frame;
    frame.image.data = owner->data();
    frame.image.width = 4;
    frame.image.height = 4;
    frame.image.row_stride_bytes = 16;
    frame.sequence = 7;
    frame.source_id = "android-main-display";
    frame.owner = owner;

    const Frame copy = frame;
    ASSERT_EQ(owner.use_count(), 3);  // local + original + copy
    frame.owner.reset();
    EXPECT_EQ(copy.owner.use_count(), 2);
    EXPECT_EQ(copy.image.data, owner->data());
    EXPECT_EQ(copy.sequence, 7U);
    EXPECT_EQ(copy.source_id, "android-main-display");
}

TEST(Frame, DefaultFrameIsValidatedThroughItsImage) {
    const Frame frame;
    const auto result = validate(frame.image);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
