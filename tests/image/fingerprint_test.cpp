#include <mirador/fingerprint.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::dhash_9x8;
using mirador::ErrorCode;
using mirador::fingerprint;
using mirador::fingerprint_similarity;
using mirador::hamming_distance;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::PixelFormat;

constexpr int64_t kBudget = 1 << 20;

ImageView gray_view_from(const std::vector<std::byte>& storage, int64_t stride, int32_t width, int32_t height) {
    ImageView view;
    view.data = storage.data();
    view.width = width;
    view.height = height;
    view.row_stride_bytes = stride;
    view.format = PixelFormat::kGray8;
    return view;
}

TEST(DHash, MatchesDocumentedBitRule) {
    // Row content controls the row's 8 bits exactly: an increasing row sets all
    // bits, a decreasing row none, and a constant row none (strict comparison).
    std::vector<std::byte> storage(72, std::byte{0});
    for (int32_t y = 0; y < 8; ++y) {
        for (int32_t x = 0; x < 9; ++x) {
            const int32_t value = (y % 2 == 0) ? x * 28 : (8 - x) * 28;  // ramp up / down
            storage[static_cast<size_t>(y) * 9 + x] = static_cast<std::byte>(value);
        }
    }
    const ImageView view = gray_view_from(storage, 9, 9, 8);

    const auto hash_result = dhash_9x8(view);
    ASSERT_TRUE(hash_result.ok());
    const uint64_t hash = hash_result.value();
    for (int32_t y = 0; y < 8; ++y) {
        const uint64_t row_mask = 0xFFULL << (y * 8);
        const uint64_t row_bits = (hash & row_mask) >> (y * 8);
        EXPECT_EQ(row_bits, y % 2 == 0 ? 0xFFULL : 0x00ULL) << "row " << y;
    }
}

TEST(DHash, RejectsNonGrayAndWrongSizes) {
    const std::vector<std::byte> storage(512, std::byte{0});

    auto wrong_size = dhash_9x8(gray_view_from(storage, 16, 8, 8));
    ASSERT_FALSE(wrong_size.ok());
    EXPECT_EQ(wrong_size.status().code(), ErrorCode::kInvalidArgument);

    ImageView rgba = gray_view_from(storage, 16, 9, 8);
    rgba.format = PixelFormat::kRgba8;
    const auto wrong_format = dhash_9x8(rgba);
    ASSERT_FALSE(wrong_format.ok());
    EXPECT_EQ(wrong_format.status().code(), ErrorCode::kInvalidArgument);

    const auto invalid_view = dhash_9x8(ImageView{});
    ASSERT_FALSE(invalid_view.ok());
    EXPECT_EQ(invalid_view.status().code(), ErrorCode::kInvalidArgument);
}

TEST(Fingerprint, HammingDistanceAndSimilarityEndsAndMiddle) {
    EXPECT_EQ(hamming_distance(0, 0), 0);
    EXPECT_EQ(hamming_distance(0, ~uint64_t{0}), 64);
    EXPECT_EQ(hamming_distance(0b0001, 0b0011), 1);
    EXPECT_DOUBLE_EQ(fingerprint_similarity(12345, 12345), 1.0);
    EXPECT_DOUBLE_EQ(fingerprint_similarity(0, ~uint64_t{0}), 0.0);
    EXPECT_DOUBLE_EQ(fingerprint_similarity(0b0001, 0b0011), 63.0 / 64.0);
}

/// Scene value in [0, 255]: soft horizontal ramp plus a vertical wave; smooth so
/// a one-pixel shift keeps most horizontal comparisons intact.
int32_t scene_value(int32_t x, int32_t y, int32_t width, int32_t height) {
    const int32_t ramp = x * 255 / (width > 1 ? width - 1 : 1);
    const int32_t wave = (y * 30 / (height > 1 ? height - 1 : 1));
    return (ramp + wave) / 2 + 40;
}

/// Test-owned RGBA scene with an explicit (possibly padded) row stride.
struct SceneImage {
    std::vector<std::byte> bytes;
    ImageView view;
};

SceneImage make_scene_rgba(int32_t width, int32_t height, int64_t stride, int32_t shift) {
    SceneImage scene;
    scene.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{0});
    std::byte* bytes = scene.bytes.data();
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const int32_t value = scene_value(x + shift, y, width, height);
            const auto offset = static_cast<int64_t>(y) * stride + static_cast<int64_t>(x) * 4;
            bytes[offset + 0] = static_cast<std::byte>(value);
            bytes[offset + 1] = static_cast<std::byte>(value);
            bytes[offset + 2] = static_cast<std::byte>(value);
            bytes[offset + 3] = static_cast<std::byte>(255);
        }
    }
    scene.view.data = scene.bytes.data();
    scene.view.width = width;
    scene.view.height = height;
    scene.view.row_stride_bytes = stride;
    scene.view.format = PixelFormat::kRgba8;
    return scene;
}

TEST(Fingerprint, StableAcrossStridesForSameContent) {
    const SceneImage tight = make_scene_rgba(64, 48, int64_t{64} * 4, 0);
    const SceneImage loose = make_scene_rgba(64, 48, int64_t{64} * 4 + 7, 0);  // padded rows

    const auto tight_hash = fingerprint(tight.view);
    const auto loose_hash = fingerprint(loose.view);
    ASSERT_TRUE(tight_hash.ok());
    ASSERT_TRUE(loose_hash.ok());
    EXPECT_EQ(tight_hash.value(), loose_hash.value());
}

TEST(Fingerprint, DistinguishesInvertedContent) {
    const SceneImage scene = make_scene_rgba(64, 48, int64_t{64} * 4, 0);
    SceneImage inverted = make_scene_rgba(64, 48, int64_t{64} * 4, 0);
    std::byte* bytes = inverted.bytes.data();
    for (int32_t y = 0; y < 48; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
            const int32_t value = 255 - scene_value(x, y, 64, 48);
            const auto offset = static_cast<int64_t>(y) * 256 + static_cast<int64_t>(x) * 4;
            bytes[offset + 0] = static_cast<std::byte>(value);
            bytes[offset + 1] = static_cast<std::byte>(value);
            bytes[offset + 2] = static_cast<std::byte>(value);
            bytes[offset + 3] = static_cast<std::byte>(255);
        }
    }

    const auto scene_hash = fingerprint(scene.view);
    const auto inverted_hash = fingerprint(inverted.view);
    ASSERT_TRUE(scene_hash.ok());
    ASSERT_TRUE(inverted_hash.ok());
    // A pure inversion flips every horizontal comparison of the monotone ramp.
    EXPECT_EQ(hamming_distance(scene_hash.value(), inverted_hash.value()), 64);
    EXPECT_DOUBLE_EQ(fingerprint_similarity(scene_hash.value(), inverted_hash.value()), 0.0);
}

TEST(Fingerprint, SmallShiftKeepsSimilarityHigh) {
    const SceneImage original = make_scene_rgba(64, 48, int64_t{64} * 4, 0);
    const SceneImage shifted = make_scene_rgba(64, 48, int64_t{64} * 4, 3);

    const auto original_hash = fingerprint(original.view);
    const auto shifted_hash = fingerprint(shifted.view);
    ASSERT_TRUE(original_hash.ok());
    ASSERT_TRUE(shifted_hash.ok());
    EXPECT_GE(fingerprint_similarity(original_hash.value(), shifted_hash.value()), 0.7);
}

TEST(Fingerprint, AcceptsGrayAndNv12Inputs) {
    // Gray path: the scene resizes directly.
    auto gray = ImageBuffer::create(PixelFormat::kGray8, 64, 48, kBudget);
    ASSERT_TRUE(gray.ok());
    ImageBuffer gray_buffer = gray.take_value();
    std::byte* bytes = gray_buffer.data();
    for (int32_t y = 0; y < 48; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
            bytes[static_cast<int64_t>(y) * 64 + x] = static_cast<std::byte>(scene_value(x, y, 64, 48));
        }
    }
    const auto gray_hash = fingerprint(gray_buffer.view());
    ASSERT_TRUE(gray_hash.ok());

    // NV12 path: luma-only thumbnail, same luma content.
    auto nv12 = ImageBuffer::create(PixelFormat::kNv12, 64, 48, kBudget);
    ASSERT_TRUE(nv12.ok());
    ImageBuffer nv12_buffer = nv12.take_value();
    std::byte* nv12_bytes = nv12_buffer.data();
    for (int32_t y = 0; y < 48; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
            nv12_bytes[static_cast<int64_t>(y) * 64 + x] = static_cast<std::byte>(scene_value(x, y, 64, 48));
        }
    }
    const auto nv12_hash = fingerprint(nv12_buffer.view());
    ASSERT_TRUE(nv12_hash.ok());
    EXPECT_EQ(gray_hash.value(), nv12_hash.value());

    const auto invalid = fingerprint(ImageView{});
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
