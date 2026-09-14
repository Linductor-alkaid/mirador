#include <mirador/change_detection.hpp>
#include <mirador/fingerprint.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using mirador::ChangeClassification;
using mirador::ChangeDetectionParams;
using mirador::ChangeReason;
using mirador::detect_change;
using mirador::ErrorCode;
using mirador::fingerprint;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::PixelFormat;
using mirador::RectI;

constexpr int64_t kBudget = 1 << 20;

/// Test-owned gray image with an explicit (possibly padded) row stride.
struct GrayImage {
    std::vector<std::byte> bytes;
    ImageView view;
};

GrayImage make_gray(int32_t width, int32_t height, int64_t stride, uint8_t fill) {
    GrayImage image;
    image.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{0});
    for (int32_t y = 0; y < height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * stride;
        std::fill(row, row + width, static_cast<std::byte>(fill));
    }
    image.view.data = image.bytes.data();
    image.view.width = width;
    image.view.height = height;
    image.view.row_stride_bytes = stride;
    image.view.format = PixelFormat::kGray8;
    return image;
}

void fill_gray_rect(GrayImage& image, const RectI& rect, uint8_t value) {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * image.view.row_stride_bytes;
        std::fill(row + rect.x, row + rect.x + rect.width, static_cast<std::byte>(value));
    }
}

/// Test-owned RGBA image (gray-valued pixels, opaque alpha) with explicit stride.
struct RgbaImage {
    std::vector<std::byte> bytes;
    ImageView view;
};

RgbaImage make_rgba(int32_t width, int32_t height, int64_t stride, uint8_t fill) {
    RgbaImage image;
    image.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{0});
    for (int32_t y = 0; y < height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * stride;
        for (int32_t x = 0; x < width; ++x) {
            row[static_cast<int64_t>(x) * 4 + 0] = static_cast<std::byte>(fill);
            row[static_cast<int64_t>(x) * 4 + 1] = static_cast<std::byte>(fill);
            row[static_cast<int64_t>(x) * 4 + 2] = static_cast<std::byte>(fill);
            row[static_cast<int64_t>(x) * 4 + 3] = static_cast<std::byte>(255);
        }
    }
    image.view.data = image.bytes.data();
    image.view.width = width;
    image.view.height = height;
    image.view.row_stride_bytes = stride;
    image.view.format = PixelFormat::kRgba8;
    return image;
}

void fill_rgba_rect(RgbaImage& image, const RectI& rect, uint8_t value) {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * image.view.row_stride_bytes;
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            row[static_cast<int64_t>(x) * 4 + 0] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 1] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 2] = static_cast<std::byte>(value);
        }
    }
}

TEST(DetectChange, UnchangedFramesEarlyExit) {
    // Same content, different layouts: the fingerprints must match, so the block
    // diff never runs.
    const RgbaImage previous = make_rgba(48, 32, int64_t{48} * 4, 70);
    const RgbaImage current = make_rgba(48, 32, int64_t{48} * 4 + 16, 70);

    const auto report = detect_change(previous.view, current.view, ChangeDetectionParams{});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kNone);
    EXPECT_EQ(report.value().reason, ChangeReason::kFingerprintEarlyExit);
    EXPECT_DOUBLE_EQ(report.value().frame_similarity, 1.0);
    EXPECT_DOUBLE_EQ(report.value().changed_area_ratio, 0.0);
    EXPECT_TRUE(report.value().changed_regions.empty());

    const auto previous_hash = fingerprint(previous.view);
    const auto current_hash = fingerprint(current.view);
    ASSERT_TRUE(previous_hash.ok());
    ASSERT_TRUE(current_hash.ok());
    EXPECT_EQ(report.value().previous_fingerprint, previous_hash.value());
    EXPECT_EQ(report.value().current_fingerprint, current_hash.value());

    const ChangeDetectionParams params;
    EXPECT_DOUBLE_EQ(report.value().thresholds.fingerprint_similarity, params.fingerprint_similarity_threshold);
    EXPECT_EQ(report.value().thresholds.block_diff, params.block_diff_threshold);
    EXPECT_DOUBLE_EQ(report.value().thresholds.global_area_ratio, params.global_area_ratio);
}

TEST(DetectChange, PartialChangeExactRoiOnGray) {
    const GrayImage previous = make_gray(64, 64, 64, 0);
    GrayImage current = make_gray(64, 64, 64, 0);
    fill_gray_rect(current, RectI{16, 24, 16, 16}, 255);

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;  // force the block diff layer

    const auto report = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kPartial);
    EXPECT_EQ(report.value().reason, ChangeReason::kBlockDiff);
    ASSERT_EQ(report.value().changed_regions.size(), 1U);
    EXPECT_EQ(report.value().changed_regions[0], (RectI{16, 24, 16, 16}));
    EXPECT_DOUBLE_EQ(report.value().changed_area_ratio, 4.0 / 64.0);
}

TEST(DetectChange, PartialChangeMapsBackToFrameCoordinates) {
    // 320x240 frame, thumbnail 64x64: one block covers 40x30 frame pixels. The
    // changed rect spans exactly blocks (2,2)..(3,3), so the merged ROI must be
    // the original rect.
    const RgbaImage previous = make_rgba(320, 240, int64_t{320} * 4, 30);
    RgbaImage current = make_rgba(320, 240, int64_t{320} * 4, 30);
    fill_rgba_rect(current, RectI{80, 60, 80, 60}, 230);

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;

    const auto report = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kPartial);
    ASSERT_EQ(report.value().changed_regions.size(), 1U);
    EXPECT_EQ(report.value().changed_regions[0], (RectI{80, 60, 80, 60}));
}

TEST(DetectChange, GlobalChangeWithDefaultParams) {
    // Left/right halves swapped: every block differs and the step edges flip the
    // fingerprint bits, so the default thresholds still reach the block layer.
    GrayImage previous = make_gray(64, 64, 64, 0);
    GrayImage current = make_gray(64, 64, 64, 0);
    for (int32_t y = 0; y < 64; ++y) {
        std::fill_n(previous.bytes.begin() + static_cast<int64_t>(y) * 64, 32, std::byte{255});
        std::fill_n(current.bytes.begin() + static_cast<int64_t>(y) * 64 + 32, 32, std::byte{255});
    }

    const auto report = detect_change(previous.view, current.view, ChangeDetectionParams{});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kGlobal);
    EXPECT_EQ(report.value().reason, ChangeReason::kBlockDiff);
    ASSERT_EQ(report.value().changed_regions.size(), 1U);
    EXPECT_EQ(report.value().changed_regions[0], (RectI{0, 0, 64, 64}));
    EXPECT_DOUBLE_EQ(report.value().changed_area_ratio, 1.0);
}

TEST(DetectChange, RotationLikeSizeSwapClassifiesGlobal) {
    // 64x32 vertical stripes vs 32x64 horizontal stripes (the k90 presented sizes
    // of one raw frame): block parity makes a diagonal checkerboard of changed
    // blocks that 8-connectivity merges into one full-frame ROI.
    GrayImage previous = make_gray(64, 32, 64, 0);
    for (int32_t y = 0; y < 32; ++y) {
        for (int32_t x = 0; x < 64; x += 16) {
            fill_gray_rect(previous, RectI{x, y, 8, 1}, 255);
        }
    }
    GrayImage current = make_gray(32, 64, 32, 0);
    for (int32_t y = 0; y < 64; y += 16) {
        fill_gray_rect(current, RectI{0, y, 32, 8}, 255);
    }

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;

    const auto report = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kGlobal);
    ASSERT_EQ(report.value().changed_regions.size(), 1U);
    EXPECT_EQ(report.value().changed_regions[0], (RectI{0, 0, 32, 64}));
    EXPECT_DOUBLE_EQ(report.value().changed_area_ratio, 0.5);
}

TEST(DetectChange, IgnoredRegionSuppressesConfinedAnimation) {
    const GrayImage previous = make_gray(64, 64, 64, 0);
    GrayImage current = make_gray(64, 64, 64, 0);
    fill_gray_rect(current, RectI{24, 24, 16, 16}, 255);

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;  // the fingerprint does change
    params.ignored_regions.push_back(RectI{16, 16, 32, 32});

    const auto report = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kNone);
    EXPECT_EQ(report.value().reason, ChangeReason::kBlockDiff);
    EXPECT_TRUE(report.value().changed_regions.empty());
    EXPECT_DOUBLE_EQ(report.value().changed_area_ratio, 0.0);
}

TEST(DetectChange, AnimationOutsideIgnoredRegionIsStillDetected) {
    const GrayImage previous = make_gray(64, 64, 64, 0);
    GrayImage current = make_gray(64, 64, 64, 0);
    fill_gray_rect(current, RectI{40, 40, 16, 16}, 255);

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;
    params.ignored_regions.push_back(RectI{0, 0, 32, 32});  // only partially overlapping frame

    const auto report = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kPartial);
    ASSERT_EQ(report.value().changed_regions.size(), 1U);
    EXPECT_EQ(report.value().changed_regions[0], (RectI{40, 40, 16, 16}));
}

TEST(DetectChange, FullyIgnoredFrameNeverChanges) {
    const GrayImage previous = make_gray(64, 64, 64, 0);
    GrayImage current = make_gray(64, 64, 64, 0);
    fill_gray_rect(current, RectI{0, 0, 64, 64}, 255);

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;
    params.ignored_regions.push_back(RectI{0, 0, 64, 64});

    const auto report = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kNone);
    EXPECT_TRUE(report.value().changed_regions.empty());
}

TEST(DetectChange, UpscaledThumbnailBlockMapping) {
    // Frame smaller than the thumbnail: each 8x8 thumbnail block maps back to a
    // 4x4 frame rect.
    const GrayImage previous = make_gray(32, 32, 32, 0);
    GrayImage current = make_gray(32, 32, 32, 0);
    fill_gray_rect(current, RectI{4, 4, 4, 4}, 255);

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;

    const auto report = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kPartial);
    ASSERT_EQ(report.value().changed_regions.size(), 1U);
    EXPECT_EQ(report.value().changed_regions[0], (RectI{4, 4, 4, 4}));
    EXPECT_DOUBLE_EQ(report.value().changed_area_ratio, 1.0 / 64.0);
}

TEST(DetectChange, Nv12LumaChangeIsDetected) {
    auto previous = ImageBuffer::create(PixelFormat::kNv12, 64, 64, kBudget);
    auto current = ImageBuffer::create(PixelFormat::kNv12, 64, 64, kBudget);
    ASSERT_TRUE(previous.ok());
    ASSERT_TRUE(current.ok());
    ImageBuffer previous_buffer = previous.take_value();
    ImageBuffer current_buffer = current.take_value();
    std::fill(previous_buffer.data(), previous_buffer.data() + previous_buffer.byte_size(),
              static_cast<std::byte>(128));
    std::fill(current_buffer.data(), current_buffer.data() + current_buffer.byte_size(), static_cast<std::byte>(128));
    const ImageView current_view = current_buffer.view();
    for (int32_t y = 24; y < 40; ++y) {
        std::byte* row = current_buffer.data() + static_cast<int64_t>(y) * current_view.row_stride_bytes;
        std::fill(row + 16, row + 32, static_cast<std::byte>(255));  // luma only
    }

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;

    const auto report = detect_change(previous_buffer.view(), current_view, params);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().classification, ChangeClassification::kPartial);
    ASSERT_EQ(report.value().changed_regions.size(), 1U);
    EXPECT_EQ(report.value().changed_regions[0], (RectI{16, 24, 16, 16}));
}

TEST(DetectChange, IgnoredRegionsUseFrameCoordinatesOnScaledFrames) {
    // Regression: the ignore check must compare frame-space block rects against
    // frame-space regions even when the frame is larger than the thumbnail
    // (320x240 frame, 64x64 thumbnail, 40x30-frame-pixel blocks).
    const RgbaImage previous = make_rgba(320, 240, int64_t{320} * 4, 30);
    RgbaImage current = make_rgba(320, 240, int64_t{320} * 4, 30);
    fill_rgba_rect(current, RectI{120, 90, 80, 60}, 230);

    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;
    params.ignored_regions.push_back(RectI{80, 60, 160, 120});  // exactly blocks (2..5, 2..5)

    const auto suppressed = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(suppressed.ok());
    EXPECT_EQ(suppressed.value().classification, ChangeClassification::kNone);
    EXPECT_TRUE(suppressed.value().changed_regions.empty());

    fill_rgba_rect(current, RectI{120, 90, 80, 60}, 30);   // restore the background
    fill_rgba_rect(current, RectI{240, 60, 40, 30}, 230);  // outside the ignored region

    const auto detected = detect_change(previous.view, current.view, params);
    ASSERT_TRUE(detected.ok());
    EXPECT_EQ(detected.value().classification, ChangeClassification::kPartial);
    ASSERT_EQ(detected.value().changed_regions.size(), 1U);
    EXPECT_EQ(detected.value().changed_regions[0], (RectI{240, 60, 40, 30}));
}

TEST(DetectChange, RejectsInvalidViewsAndParameters) {
    const GrayImage frame = make_gray(64, 64, 64, 0);
    const ChangeDetectionParams defaults;

    const auto null_previous = detect_change(ImageView{}, frame.view, defaults);
    ASSERT_FALSE(null_previous.ok());
    EXPECT_EQ(null_previous.status().code(), ErrorCode::kInvalidArgument);

    const auto null_current = detect_change(frame.view, ImageView{}, defaults);
    ASSERT_FALSE(null_current.ok());
    EXPECT_EQ(null_current.status().code(), ErrorCode::kInvalidArgument);

    auto params = defaults;
    params.fingerprint_similarity_threshold = -0.01;
    const auto negative_threshold = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(negative_threshold.ok());
    EXPECT_EQ(negative_threshold.status().code(), ErrorCode::kInvalidArgument);

    params.fingerprint_similarity_threshold = 1.01;
    const auto large_threshold = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(large_threshold.ok());

    params = defaults;
    params.fingerprint_similarity_threshold = std::numeric_limits<double>::quiet_NaN();
    const auto nan_threshold = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(nan_threshold.ok());

    params = defaults;
    params.thumbnail_size = 7;
    const auto small_thumbnail = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(small_thumbnail.ok());

    params.thumbnail_size = 257;
    const auto large_thumbnail = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(large_thumbnail.ok());

    params = defaults;
    params.block_size = 0;
    const auto zero_block = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(zero_block.ok());

    params = defaults;
    params.block_size = 100;  // larger than thumbnail_size 64
    const auto huge_block = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(huge_block.ok());

    params = defaults;
    params.thumbnail_size = 256;
    params.block_size = 3;  // 86 blocks per side > 64
    const auto fine_grid = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(fine_grid.ok());

    params = defaults;
    params.block_diff_threshold = -1;
    const auto negative_diff = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(negative_diff.ok());

    params.block_diff_threshold = 256;
    const auto large_diff = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(large_diff.ok());

    params = defaults;
    params.global_area_ratio = 0.0;
    const auto zero_ratio = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(zero_ratio.ok());

    params.global_area_ratio = 1.01;
    const auto large_ratio = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(large_ratio.ok());

    params = defaults;
    params.ignored_regions.push_back(RectI{-1, 0, 8, 8});
    const auto negative_region = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(negative_region.ok());

    params.ignored_regions.clear();
    params.ignored_regions.push_back(RectI{60, 60, 8, 8});  // exceeds the 64x64 frame
    const auto outside_region = detect_change(frame.view, frame.view, params);
    ASSERT_FALSE(outside_region.ok());
}

// --- M2-03: bounded signatures and the signature-based overload -----------------

TEST(ChangeSignature, ViewAndSignaturePathsProduceIdenticalReports) {
    const GrayImage previous = make_gray(320, 240, 320, 40);
    GrayImage current = make_gray(320, 240, 320, 40);
    fill_gray_rect(current, RectI{100, 80, 60, 40}, 200);

    const ChangeDetectionParams defaults;
    const auto via_views = detect_change(previous.view, current.view, defaults);
    ASSERT_TRUE(via_views.ok());

    auto previous_signature = mirador::make_change_signature(previous.view, defaults);
    auto current_signature = mirador::make_change_signature(current.view, defaults);
    ASSERT_TRUE(previous_signature.ok());
    ASSERT_TRUE(current_signature.ok());

    // Stride-independent stability: rebuilding the signature from a padded
    // layout changes neither fingerprint nor thumbnail content.
    const GrayImage padded_previous = make_gray(320, 240, 384, 40);
    auto padded_signature = mirador::make_change_signature(padded_previous.view, defaults);
    ASSERT_TRUE(padded_signature.ok());
    EXPECT_EQ(padded_signature.value().fingerprint, previous_signature.value().fingerprint);

    const auto via_signatures = detect_change(previous_signature.value(), current_signature.value(), defaults);
    ASSERT_TRUE(via_signatures.ok());
    EXPECT_EQ(via_signatures.value().classification, via_views.value().classification);
    EXPECT_EQ(via_signatures.value().reason, via_views.value().reason);
    EXPECT_EQ(via_signatures.value().changed_area_ratio, via_views.value().changed_area_ratio);
    EXPECT_EQ(via_signatures.value().changed_regions, via_views.value().changed_regions);
    EXPECT_EQ(via_signatures.value().frame_similarity, via_views.value().frame_similarity);

    // ROI mapping uses the current signature's stored frame dimensions.
    EXPECT_EQ(current_signature.value().frame_width, 320);
    EXPECT_EQ(current_signature.value().frame_height, 240);
    EXPECT_EQ(current_signature.value().thumbnail.format(), PixelFormat::kGray8);
    EXPECT_EQ(current_signature.value().thumbnail.width(), defaults.thumbnail_size);
}

TEST(ChangeSignature, EarlyExitSkipsNothingButIsReportedIdentically) {
    const GrayImage frame_a = make_gray(200, 200, 200, 10);
    const GrayImage frame_b = make_gray(200, 200, 200, 10);
    const ChangeDetectionParams defaults;

    auto signature_a = mirador::make_change_signature(frame_a.view, defaults);
    auto signature_b = mirador::make_change_signature(frame_b.view, defaults);
    ASSERT_TRUE(signature_a.ok());
    ASSERT_TRUE(signature_b.ok());

    const auto via_signatures = detect_change(signature_a.value(), signature_b.value(), defaults);
    const auto via_views = detect_change(frame_a.view, frame_b.view, defaults);
    ASSERT_TRUE(via_signatures.ok());
    ASSERT_TRUE(via_views.ok());
    EXPECT_EQ(via_signatures.value().classification, ChangeClassification::kNone);
    EXPECT_EQ(via_signatures.value().reason, ChangeReason::kFingerprintEarlyExit);
    EXPECT_EQ(via_signatures.value().frame_similarity, via_views.value().frame_similarity);
    EXPECT_EQ(via_signatures.value().previous_fingerprint, via_views.value().previous_fingerprint);
}

TEST(ChangeSignature, RejectsMismatchedThumbnailsAndBadParameters) {
    const GrayImage small = make_gray(64, 64, 64, 10);
    const GrayImage large = make_gray(320, 240, 320, 10);
    const ChangeDetectionParams defaults;

    auto small_signature = mirador::make_change_signature(small.view, defaults);
    auto large_signature = mirador::make_change_signature(large.view, defaults);
    ASSERT_TRUE(small_signature.ok());
    ASSERT_TRUE(large_signature.ok());

    // Thumbnail agreement comes from the signatures themselves: build one with
    // a different thumbnail_size and compare.
    ChangeDetectionParams smaller = defaults;
    smaller.thumbnail_size = 32;
    auto small_grid_signature = mirador::make_change_signature(small.view, smaller);
    ASSERT_TRUE(small_grid_signature.ok());
    EXPECT_EQ(small_grid_signature.value().thumbnail.width(), 32);
    const auto mismatched = detect_change(small_grid_signature.value(), large_signature.value(), defaults);
    ASSERT_FALSE(mismatched.ok());
    EXPECT_EQ(mismatched.status().code(), ErrorCode::kInvalidArgument);

    ChangeDetectionParams params = defaults;
    params.block_size = defaults.thumbnail_size + 1;  // larger than the stored thumbnail edge
    const auto oversized_block = detect_change(small_signature.value(), large_signature.value(), params);
    ASSERT_FALSE(oversized_block.ok());

    // Ignored regions are checked against the current signature's frame size.
    params = defaults;
    params.ignored_regions.push_back(RectI{280, 200, 40, 40});  // exactly inside 320x240
    const auto inside = detect_change(small_signature.value(), large_signature.value(), params);
    EXPECT_TRUE(inside.ok());
    const auto outside = detect_change(large_signature.value(), small_signature.value(), params);
    ASSERT_FALSE(outside.ok());

    // Invalid view or thumbnail range is rejected at signature build time.
    const ImageView invalid_view;
    const auto invalid = mirador::make_change_signature(invalid_view, defaults);
    ASSERT_FALSE(invalid.ok());

    ChangeDetectionParams bad_thumbnail = defaults;
    bad_thumbnail.thumbnail_size = 4;
    const auto bad_size = mirador::make_change_signature(small.view, bad_thumbnail);
    ASSERT_FALSE(bad_size.ok());
}

}  // namespace
