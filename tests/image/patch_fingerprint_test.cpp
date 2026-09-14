// M3-09: unit tests for the visual patch fingerprint extraction (design
// section 12, DEC-014).

#include <mirador/image_buffer.hpp>
#include <mirador/patch_fingerprint.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::ImageView;
using mirador::PatchFingerprintParams;
using mirador::PixelFormat;

TEST(PatchFingerprint, EqualContentYieldsEqualFingerprintsAcrossStride) {
    const int32_t side = 16;
    std::vector<uint8_t> tight(static_cast<size_t>(side) * side);
    for (size_t i = 0; i < tight.size(); ++i) {
        tight[i] = static_cast<uint8_t>(i * 17 % 251);
    }
    std::vector<uint8_t> padded(static_cast<size_t>(side) * (side + 8), 0xAA);
    for (int32_t y = 0; y < side; ++y) {
        for (int32_t x = 0; x < side; ++x) {
            padded[static_cast<size_t>(y) * (side + 8) + x] = tight[static_cast<size_t>(y) * side + x];
        }
    }

    ImageView a;
    a.data = reinterpret_cast<const std::byte*>(tight.data());
    a.width = side;
    a.height = side;
    a.row_stride_bytes = side;
    a.format = PixelFormat::kGray8;
    ImageView b = a;
    b.data = reinterpret_cast<const std::byte*>(padded.data());
    b.row_stride_bytes = side + 8;

    PatchFingerprintParams params;
    params.thumb_side = side;
    const auto first = mirador::make_visual_patch_fingerprint(a, params, 1 << 20);
    const auto second = mirador::make_visual_patch_fingerprint(b, params, 1 << 20);
    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(first.value().thumb_width, side);
    EXPECT_EQ(first.value().thumb_height, side);
    EXPECT_EQ(first.value().thumbnail_gray.size(), static_cast<size_t>(side) * side);
}

TEST(PatchFingerprint, DifferentContentChangesTheHash) {
    // Two distinct patterns: a rising ramp and its reverse (uniform images
    // would share the same all-equal dHash by definition).
    std::vector<uint8_t> dark(24 * 24);
    std::vector<uint8_t> bright(24 * 24);
    for (int32_t y = 0; y < 24; ++y) {
        for (int32_t x = 0; x < 24; ++x) {
            dark[static_cast<size_t>(y) * 24 + x] = static_cast<uint8_t>((x * 10 + y * 3) % 256);
            bright[static_cast<size_t>(y) * 24 + x] = static_cast<uint8_t>(255 - (x * 10 + y * 3) % 256);
        }
    }
    auto view_of = [](std::vector<uint8_t>& pixels) {
        ImageView v;
        v.data = reinterpret_cast<const std::byte*>(pixels.data());
        v.width = 24;
        v.height = 24;
        v.row_stride_bytes = 24;
        v.format = PixelFormat::kGray8;
        return v;
    };
    PatchFingerprintParams params;
    const auto dark_fp = mirador::make_visual_patch_fingerprint(view_of(dark), params, 1 << 20);
    const auto bright_fp = mirador::make_visual_patch_fingerprint(view_of(bright), params, 1 << 20);
    ASSERT_TRUE(dark_fp.ok());
    ASSERT_TRUE(bright_fp.ok());
    EXPECT_NE(dark_fp.value().content_hash, bright_fp.value().content_hash);
    EXPECT_NE(dark_fp.value().dhash, bright_fp.value().dhash);
}

TEST(PatchFingerprint, ColorInputConvertsToGray) {
    std::vector<uint8_t> rgb(8 * 8 * 3);
    for (size_t i = 0; i < rgb.size(); ++i) {
        rgb[i] = static_cast<uint8_t>(i % 256);
    }
    ImageView view;
    view.data = reinterpret_cast<const std::byte*>(rgb.data());
    view.width = 8;
    view.height = 8;
    view.row_stride_bytes = 24;
    view.format = PixelFormat::kRgb8;

    PatchFingerprintParams params;
    params.thumb_side = 8;
    const auto fp = mirador::make_visual_patch_fingerprint(view, params, 1 << 20);
    ASSERT_TRUE(fp.ok()) << fp.status().message();
    // The documented BT.601 luma formula over the actual channel bytes.
    for (int32_t y = 0; y < 8; ++y) {
        for (int32_t x = 0; x < 8; ++x) {
            const size_t offset = static_cast<size_t>(y) * 24 + x * 3;
            const int expected = (77 * rgb[offset] + 150 * rgb[offset + 1] + 29 * rgb[offset + 2] + 128) >> 8;
            EXPECT_EQ(std::to_integer<uint8_t>(fp.value().thumbnail_gray[static_cast<size_t>(y) * 8 + x]), expected);
        }
    }
}

TEST(PatchFingerprint, ExplicitErrors) {
    PatchFingerprintParams bad_side;
    bad_side.thumb_side = 4;

    std::vector<uint8_t> pixels(16 * 16, 7);
    ImageView view;
    view.data = reinterpret_cast<const std::byte*>(pixels.data());
    view.width = 16;
    view.height = 16;
    view.row_stride_bytes = 16;
    view.format = PixelFormat::kGray8;

    ASSERT_EQ(mirador::make_visual_patch_fingerprint(view, bad_side, 1 << 20).status().code(),
              ErrorCode::kInvalidArgument);
    ASSERT_EQ(mirador::make_visual_patch_fingerprint(ImageView{}, PatchFingerprintParams{}, 1 << 20).status().code(),
              ErrorCode::kInvalidArgument);
    ASSERT_EQ(mirador::make_visual_patch_fingerprint(view, PatchFingerprintParams{}, 16).status().code(),
              ErrorCode::kBudgetExceeded);

    // NV12 works through its luma plane.
    auto nv12 = mirador::ImageBuffer::create(PixelFormat::kNv12, 16, 16, 8192);
    ASSERT_TRUE(nv12.ok());
    const auto luma_fp = mirador::make_visual_patch_fingerprint(nv12.value().view(), PatchFingerprintParams{}, 1 << 20);
    ASSERT_TRUE(luma_fp.ok()) << luma_fp.status().message();
}

}  // namespace
