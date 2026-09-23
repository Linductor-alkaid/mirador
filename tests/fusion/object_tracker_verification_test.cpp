// M7-05 neighborhood verifier: independent verification suite for
// `ObjectTracker::verification_roi`, `ObjectTracker::verify_track` and
// `ObjectTracker::record_structure_baseline` (object-tracking design section
// 6.2). Written against the frozen header contracts only:
//   * E1 multi-template NCC with the peak-sidelobe-quality gate (a flat
//     response surface is never trusted at any peak height), integer-shift
//     recovery, the single-offset edge-clamped fallback and the negative-
//     template boundary (impostor veto frozen for M7-06).
//   * E2 closure-structure deviation against the recorded per-track baseline,
//     including the explicit kNotSupplied/kNoBaseline missing-input states and
//     the inclusive tolerance boundary.
//   * Baseline bookkeeping: one overwrite slot per track, +32 bytes accounted,
//     released by terminate/eviction, kBudgetExceeded with the pool untouched.
//   * Acceptance matrix: DOD-03 rotations x odd size x non-contiguous stride,
//     flush edges, format invariance, bitwise determinism (repeat + twin
//     instance), purity of the const decision, the frozen planned-work budget
//     formula with its exact boundary, validation-before-cancellation
//     precedence, and the DOD-04 parameter/template-set invalidation flips.
// Determinism of every scenario below rests on exact integer resampling
// (verified against `resize_area`: 1:1 crops copy, 2x2 constant blocks resample
// to the block value) and on the exact integer RGBA->gray luma
// (77r+150g+29b+128)>>8, which maps (v,v,v,255) to v.
//
// Privacy (RULE-10/DOD-06) is structural here, as in the M7-03 suite: the
// result types (`TrackVerification` and children) carry only ids, enums, ROI
// rectangles and floating-point scores — no template bytes, thumbnails or
// frame content exist in them to leak, the verifier is a pure in-memory
// decision with no logging, filesystem or network surface, and the shared
// privacy suite covers the binary.

#include <mirador/object_tracker.hpp>

#include <mirador/crop.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/patch_fingerprint.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/visual_fingerprint.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

using mirador::AppearanceChannelOutcome;
using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PatchFingerprintParams;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::RectF;
using mirador::RectI;
using mirador::Rotation;
using mirador::StructureChannelOutcome;
using mirador::TargetTrack;
using mirador::TrackState;
using mirador::TrackStructureDescriptors;
using mirador::TrackTemplate;
using mirador::TrackVerification;
using mirador::VisualPatchFingerprint;
using mirador::VisualRegion;

constexpr int64_t kGenerousBudget = int64_t{1} << 20;

// --- fixtures and helpers -----------------------------------------------------

/// Owning single-channel gray8 buffer with an explicit row stride.
struct GrayImage {
    std::vector<std::byte> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int64_t stride = 0;
};

GrayImage make_gray_image(int32_t width, int32_t height, std::byte fill, int64_t stride_padding_bytes = 0) {
    GrayImage image;
    image.width = width;
    image.height = height;
    image.stride = static_cast<int64_t>(width) + stride_padding_bytes;
    image.pixels.assign(static_cast<size_t>(image.stride) * static_cast<size_t>(height), fill);
    return image;
}

void fill_gray_rect(GrayImage& image, const RectI& rect, uint8_t value) {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x)] =
                std::byte{value};
        }
    }
}

void write_gray_block(GrayImage& image, int32_t origin_x, int32_t origin_y, const std::vector<uint8_t>& block) {
    ASSERT_EQ(block.size(), 64U);
    for (int32_t row = 0; row < 8; ++row) {
        for (int32_t col = 0; col < 8; ++col) {
            image.pixels[static_cast<size_t>(origin_y + row) * static_cast<size_t>(image.stride) +
                         static_cast<size_t>(origin_x + col)] =
                std::byte{block[static_cast<size_t>(row) * 8U + static_cast<size_t>(col)]};
        }
    }
}

/// Deterministic pseudo-random 8x8 block (no periodicity, so partial-window
/// sidelobes decorrelate and the PSR margin stays healthy).
std::vector<uint8_t> pseudo_block(uint64_t seed) {
    std::vector<uint8_t> block(64U);
    for (size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<uint8_t>((i * seed + seed * seed) % 256U);
    }
    return block;
}

/// Noise-like per-pixel pattern (uint32 hashing, deterministic, no UB):
/// content decorrelates under integer translation, keeping translation
/// sidelobes at noise level so the peak-sidelobe gate has healthy margins.
uint8_t noise_pixel(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393U + static_cast<uint32_t>(y) * 668265263U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    return static_cast<uint8_t>((h ^ (h >> 16U)) % 256U);
}

ImageView view_of(const GrayImage& image, Rotation rotation = Rotation::k0) {
    ImageView view;
    view.data = image.pixels.data();
    view.width = image.width;
    view.height = image.height;
    view.row_stride_bytes = image.stride;
    view.format = PixelFormat::kGray8;
    view.rotation = rotation;
    return view;
}

/// Owning RGBA8 buffer (format-invariance case): every pixel is the neutral
/// gray (v, v, v, 255), which the exact integer luma maps back to v.
struct RgbaImage {
    std::vector<std::byte> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int64_t stride = 0;
};

RgbaImage rgba_from_gray(const GrayImage& gray) {
    RgbaImage image;
    image.width = gray.width;
    image.height = gray.height;
    image.stride = static_cast<int64_t>(gray.width) * 4;
    image.pixels.assign(static_cast<size_t>(image.stride) * static_cast<size_t>(gray.height), std::byte{0});
    for (int32_t y = 0; y < gray.height; ++y) {
        for (int32_t x = 0; x < gray.width; ++x) {
            const auto value = std::to_integer<uint8_t>(
                gray.pixels[static_cast<size_t>(y) * static_cast<size_t>(gray.stride) + static_cast<size_t>(x)]);
            const size_t offset =
                static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x) * 4U;
            image.pixels[offset + 0] = std::byte{value};
            image.pixels[offset + 1] = std::byte{value};
            image.pixels[offset + 2] = std::byte{value};
            image.pixels[offset + 3] = std::byte{255};
        }
    }
    return image;
}

ImageView view_of(const RgbaImage& image) {
    ImageView view;
    view.data = image.pixels.data();
    view.width = image.width;
    view.height = image.height;
    view.row_stride_bytes = image.stride;
    view.format = PixelFormat::kRgba8;
    view.rotation = Rotation::k0;
    return view;
}

VisualRegion make_region(uint64_t stable_id, RectF bounds, float confidence = 0.9F) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.anchor = PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    region.confidence = confidence;
    return region;
}

ObjectTracker make_tracker(const ObjectTrackerOptions& options = {}) {
    auto created = ObjectTracker::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_tracker: " << created.status().message();
        return ObjectTracker{};
    }
    return created.take_value();
}

/// The documented covering ROI (floor leading edge, ceil trailing edge).
RectI covering_roi_of(const RectF& bounds, const ImageView& view) {
    const auto x0 = static_cast<int32_t>(std::floor(static_cast<double>(bounds.x)));
    const auto y0 = static_cast<int32_t>(std::floor(static_cast<double>(bounds.y)));
    const auto x1 = static_cast<int32_t>(std::ceil(static_cast<double>(bounds.x + bounds.width)));
    const auto y1 = static_cast<int32_t>(std::ceil(static_cast<double>(bounds.y + bounds.height)));
    const int32_t clamped_x0 = std::max(x0, 0);
    const int32_t clamped_y0 = std::max(y0, 0);
    return RectI{clamped_x0, clamped_y0, std::min(x1, view.width) - clamped_x0, std::min(y1, view.height) - clamped_y0};
}

/// Reference template through the public crop + patch-fingerprint pipeline.
mirador::Result<VisualPatchFingerprint> direct_template(const ImageView& view, const RectI& roi, int32_t thumb_side) {
    auto cropped = mirador::crop(view, roi, kGenerousBudget);
    if (!cropped.ok()) {
        return cropped.status();
    }
    return mirador::make_visual_patch_fingerprint(cropped.value().view(), PatchFingerprintParams{thumb_side},
                                                  kGenerousBudget);
}

/// The documented M3-10 NCC of two same-size packed thumbnails, recomputed
/// from the contract for cross-checking verifier scores.
double reference_ncc(const VisualPatchFingerprint& query, const VisualPatchFingerprint& entry) {
    const size_t count = query.thumbnail_gray.size();
    EXPECT_GT(count, 0U);
    EXPECT_EQ(entry.thumbnail_gray.size(), count);
    if (count == 0 || entry.thumbnail_gray.size() != count) {
        return 0.0;
    }
    double query_mean = 0.0;
    double entry_mean = 0.0;
    for (size_t i = 0; i < count; ++i) {
        query_mean += std::to_integer<uint8_t>(query.thumbnail_gray[i]);
        entry_mean += std::to_integer<uint8_t>(entry.thumbnail_gray[i]);
    }
    query_mean /= static_cast<double>(count);
    entry_mean /= static_cast<double>(count);
    double covariance = 0.0;
    double query_variance = 0.0;
    double entry_variance = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double q = std::to_integer<uint8_t>(query.thumbnail_gray[i]) - query_mean;
        const double e = std::to_integer<uint8_t>(entry.thumbnail_gray[i]) - entry_mean;
        covariance += q * e;
        query_variance += q * q;
        entry_variance += e * e;
    }
    const double denominator = std::sqrt(query_variance * entry_variance);
    if (denominator <= 0.0) {
        return query.thumbnail_gray == entry.thumbnail_gray ? 1.0 : 0.0;
    }
    return covariance / denominator;
}

/// Observable pool state for purity/atomicity comparisons.
struct PoolSnapshot {
    size_t track_count = 0;
    int64_t used_bytes = 0;
    uint64_t evicted_count = 0;
    uint32_t layout_generation = 0;
    std::vector<uint64_t> ids;
};

PoolSnapshot snapshot_of(const ObjectTracker& tracker) {
    PoolSnapshot snapshot;
    snapshot.track_count = tracker.track_count();
    snapshot.used_bytes = tracker.byte_size();
    snapshot.evicted_count = tracker.evicted_track_count();
    snapshot.layout_generation = tracker.layout_generation();
    snapshot.ids = tracker.track_ids();
    return snapshot;
}

void expect_pool_untouched(const PoolSnapshot& before, const ObjectTracker& tracker) {
    EXPECT_EQ(tracker.track_count(), before.track_count);
    EXPECT_EQ(tracker.byte_size(), before.used_bytes);
    EXPECT_EQ(tracker.evicted_track_count(), before.evicted_count);
    EXPECT_EQ(tracker.layout_generation(), before.layout_generation);
    EXPECT_EQ(tracker.track_ids(), before.ids);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_verification_bitwise_equal(const TrackVerification& lhs, const TrackVerification& rhs) {
    EXPECT_EQ(lhs.track_id, rhs.track_id);
    EXPECT_EQ(lhs.state, rhs.state);
    EXPECT_EQ(lhs.verification_roi, rhs.verification_roi);
    EXPECT_EQ(lhs.appearance.outcome, rhs.appearance.outcome);
    EXPECT_EQ(lhs.appearance.peak_ncc, rhs.appearance.peak_ncc);
    EXPECT_EQ(lhs.appearance.peak_sidelobe_ratio, rhs.appearance.peak_sidelobe_ratio);
    EXPECT_EQ(lhs.appearance.best_template_index, rhs.appearance.best_template_index);
    EXPECT_EQ(lhs.appearance.best_offset_dx, rhs.appearance.best_offset_dx);
    EXPECT_EQ(lhs.appearance.best_offset_dy, rhs.appearance.best_offset_dy);
    EXPECT_EQ(lhs.structure.outcome, rhs.structure.outcome);
    EXPECT_EQ(lhs.structure.closure_deviation, rhs.structure.closure_deviation);
    EXPECT_EQ(lhs.structure.rectangularity_deviation, rhs.structure.rectangularity_deviation);
    EXPECT_EQ(lhs.structure.edge_support_deviation, rhs.structure.edge_support_deviation);
    EXPECT_EQ(lhs.structure.max_deviation, rhs.structure.max_deviation);
}

void adopt_or_fail(ObjectTracker& tracker, const ImageView& view, uint64_t id, const RectF& bounds,
                   uint64_t frame_sequence = 1) {
    const auto adopted = tracker.adopt_track(make_region(id, bounds), view, frame_sequence);
    ASSERT_TRUE(adopted.ok()) << "adopt " << id << ": " << adopted.status().message();
}

TrackStructureDescriptors descriptors_of(float closure, float rectangularity, float edge_support) {
    return TrackStructureDescriptors{closure, rectangularity, edge_support};
}

// --- verification_roi: frozen expansion rule --------------------------------------

/// Hand-computed exact ROIs for integer bounds on a 64x64 view (the rule
/// expands around the predicted center: expanded.x = bounds.x - margin).
/// Ratio 1: margin = 8*sqrt(2)/2 ~= 5.657, expanded
/// {10.343, 10.343, 19.314, 19.314} -> covering {10, 10, 20, 20}. Ratio 2:
/// margin ~= 11.314, expanded {4.686, 4.686, 30.627, 30.627} -> covering
/// {4, 4, 32, 32}.
TEST(ObjectTrackerVerificationTest, VerificationRoiMatchesFrozenExpansionRule) {
    ObjectTracker ratio_one = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(ratio_one, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    const auto roi_one = ratio_one.verification_roi(7U, view);
    ASSERT_TRUE(roi_one.ok()) << roi_one.status().message();
    EXPECT_EQ(roi_one.value(), (RectI{10, 10, 20, 20}));

    ObjectTrackerOptions double_ratio_options;
    double_ratio_options.verification_roi_diagonal_ratio = 2.0;
    ObjectTracker ratio_two = make_tracker(double_ratio_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(ratio_two, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const auto roi_two = ratio_two.verification_roi(7U, view);
    ASSERT_TRUE(roi_two.ok()) << roi_two.status().message();
    EXPECT_EQ(roi_two.value(), (RectI{4, 4, 32, 32}));
}

/// A flush corner track with the maximum ratio expands past every edge and
/// must clamp to exactly the presented view.
TEST(ObjectTrackerVerificationTest, VerificationRoiClampsToViewAtFlushEdges) {
    ObjectTrackerOptions options;
    options.verification_roi_diagonal_ratio = 8.0;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = make_gray_image(24, 24, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}));

    const auto roi = tracker.verification_roi(3U, view);
    ASSERT_TRUE(roi.ok()) << roi.status().message();
    EXPECT_EQ(roi.value(), (RectI{0, 0, 24, 24}));
}

TEST(ObjectTrackerVerificationTest, VerificationRoiRejectsUnknownTerminatedAndInvalidView) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(32, 32, std::byte{40});
    const ImageView view = view_of(image);

    const auto unknown = tracker.verification_roi(99U, view);
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 5U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    ASSERT_TRUE(tracker.terminate(5U, 9).ok());
    const auto terminated = tracker.verification_roi(5U, view);
    EXPECT_EQ(terminated.status().code(), ErrorCode::kInvalidArgument);

    ImageView null_data;
    null_data.width = 32;
    null_data.height = 32;
    null_data.row_stride_bytes = 32;
    null_data.format = PixelFormat::kGray8;
    const auto invalid_view = tracker.verification_roi(5U, null_data);
    EXPECT_EQ(invalid_view.status().code(), ErrorCode::kInvalidArgument);

    // The query is a pure read: nothing about the pool moved (the terminated
    // archive keeps its identity record only).
    EXPECT_EQ(tracker.track_count(), 1U);
    EXPECT_EQ(tracker.byte_size(), ObjectTracker::kTrackOverheadBytes);
}

/// Adopted in a 64x64 frame, queried against a 3x3 view: the expanded ROI lies
/// entirely outside the tiny view, which is the documented kInvalidArgument.
TEST(ObjectTrackerVerificationTest, VerificationRoiDoesNotIntersectTinyViewErrors) {
    ObjectTracker tracker = make_tracker();
    const GrayImage frame = make_gray_image(64, 64, std::byte{40});
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame), 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    const GrayImage tiny = make_gray_image(3, 3, std::byte{40});
    const auto roi = tracker.verification_roi(7U, view_of(tiny));
    EXPECT_EQ(roi.status().code(), ErrorCode::kInvalidArgument);
}

// --- verify_track E1: template NCC with the peak-sidelobe gate ---------------------

/// Same frame as adoption: the window at offset (0, 0) reproduces the adoption
/// template byte for byte, so the peak is 1.0 at (0, 0), the PSR gate trusts
/// it and the channel grades kStrong. The result echoes id, state and exactly
/// the ROI the pure query reports.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest ASSERT_* macro expansion dominates the metric
TEST(ObjectTrackerVerificationTest, VerifyTrackSameFrameIsStrongAtZeroOffset) {
    ObjectTracker tracker = make_tracker();
    GrayImage image = make_gray_image(64, 64, std::byte{0});
    for (int32_t y = 0; y < 64; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
            image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x)] =
                std::byte{noise_pixel(x, y)};
        }
    }
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    const auto verified = tracker.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    const TrackVerification& result = verified.value();
    EXPECT_EQ(result.track_id, 7U);
    EXPECT_EQ(result.state, TrackState::kTracking);
    const auto expected_roi = tracker.verification_roi(7U, view);
    ASSERT_TRUE(expected_roi.ok()) << expected_roi.status().message();
    EXPECT_EQ(result.verification_roi, expected_roi.value());

    EXPECT_EQ(result.appearance.outcome, AppearanceChannelOutcome::kStrong);
    EXPECT_NEAR(result.appearance.peak_ncc, 1.0, 1e-9);
    EXPECT_GE(result.appearance.peak_sidelobe_ratio, tracker.options().peak_sidelobe_ratio_min);
    EXPECT_EQ(result.appearance.best_template_index, 0U);
    EXPECT_EQ(result.appearance.best_offset_dx, 0);
    EXPECT_EQ(result.appearance.best_offset_dy, 0);

    EXPECT_EQ(result.structure.outcome, StructureChannelOutcome::kNotSupplied);
    EXPECT_EQ(result.structure.max_deviation, 0.0);
}

/// The target moved by the integer translation (8, 4): the E1 scan must find
/// the copied 8x8 block at exactly that offset with a perfect peak.
TEST(ObjectTrackerVerificationTest, VerifyTrackRecoversIntegerShiftOfTarget) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_a, 8, 8, block);
    GrayImage frame_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_b, 16, 12, block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    // Ratio 3 widens the ROI enough that (8, 4) lies inside the offset grid.
    options.verification_roi_diagonal_ratio = 3.0;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));

    const auto verified = tracker.verify_track(7U, view_of(frame_b), std::nullopt);
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    EXPECT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kStrong);
    EXPECT_NEAR(verified.value().appearance.peak_ncc, 1.0, 1e-9);
    EXPECT_EQ(verified.value().appearance.best_template_index, 0U);
    EXPECT_EQ(verified.value().appearance.best_offset_dx, 8);
    EXPECT_EQ(verified.value().appearance.best_offset_dy, 4);
}

/// A completely flat scene: every window matches the flat adoption template
/// perfectly (peak 1.0) but the response surface is flat, so PSR is exactly 0
/// and the gate rejects the peak at any height — kNone.
TEST(ObjectTrackerVerificationTest, VerifyTrackFlatScenePeakRejectedBySidelobeGate) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{128});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    const auto verified = tracker.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    EXPECT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kNone);
    EXPECT_EQ(verified.value().appearance.peak_ncc, 1.0);
    EXPECT_EQ(verified.value().appearance.peak_sidelobe_ratio, 0.0);
    EXPECT_EQ(verified.value().appearance.best_offset_dx, 0);
    EXPECT_EQ(verified.value().appearance.best_offset_dy, 0);
}

/// Raising `peak_sidelobe_ratio_min` above the achieved PSR must reject the
/// same perfect peak: the outcome drops to kNone while the peak evidence
/// itself is still reported unchanged (DOD-04 parameter invalidation).
TEST(ObjectTrackerVerificationTest, VerifyTrackUntrustedPeakYieldsNoneRegardlessOfHeight) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_a, 8, 8, block);
    GrayImage frame_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_b, 16, 12, block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.verification_roi_diagonal_ratio = 3.0;
    options.peak_sidelobe_ratio_min = 1e15;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));

    const auto verified = tracker.verify_track(7U, view_of(frame_b), std::nullopt);
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    EXPECT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kNone);
    EXPECT_NEAR(verified.value().appearance.peak_ncc, 1.0, 1e-9);
    EXPECT_EQ(verified.value().appearance.best_offset_dx, 8);
    EXPECT_EQ(verified.value().appearance.best_offset_dy, 4);
}

/// Controlled single-candidate search: the whole-view track on a 16x16 frame
/// has exactly the offset (0, 0) in its grid, so the single response is
/// trivially distinctive (PSR = peak / 1e-12). The frame pair is built from
/// 2x2 constant blocks, which `resize_area` resamples exactly, so the NCC of
/// the candidate against the adoption template is recomputable in the test
/// from the documented M3-10 formula (~0.7014 — inside the weak band).
/// Three trackers differing only in the NCC thresholds must grade the SAME
/// response kStrong / kWeak / kNone (DOD-04: parameter change flips the
/// outcome, no silent reuse across parameter differences).
TEST(ObjectTrackerVerificationTest, VerifyTrackWeakBandAndThresholdBoundaryFlips) {
    // frame_a -> thumbnail 0*63 pixels + one 255; frame_b -> two 255 pixels.
    GrayImage frame_a = make_gray_image(16, 16, std::byte{0});
    fill_gray_rect(frame_a, RectI{14, 14, 2, 2}, 255);
    GrayImage frame_b = make_gray_image(16, 16, std::byte{0});
    fill_gray_rect(frame_b, RectI{0, 0, 2, 2}, 255);
    fill_gray_rect(frame_b, RectI{14, 14, 2, 2}, 255);

    const auto template_a = direct_template(view_of(frame_a), RectI{0, 0, 16, 16}, 8);
    const auto template_b = direct_template(view_of(frame_b), RectI{0, 0, 16, 16}, 8);
    ASSERT_TRUE(template_a.ok()) << template_a.status().message();
    ASSERT_TRUE(template_b.ok()) << template_b.status().message();
    const double expected_ncc = reference_ncc(template_b.value(), template_a.value());
    EXPECT_GT(expected_ncc, 0.6);
    EXPECT_LT(expected_ncc, 0.8);

    const RectF whole_view{0.0F, 0.0F, 16.0F, 16.0F};
    ObjectTracker default_tracker = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(default_tracker, view_of(frame_a), 7U, whole_view));
    const auto weak = default_tracker.verify_track(7U, view_of(frame_b), std::nullopt);
    ASSERT_TRUE(weak.ok()) << weak.status().message();
    EXPECT_EQ(weak.value().appearance.outcome, AppearanceChannelOutcome::kWeak);
    EXPECT_NEAR(weak.value().appearance.peak_ncc, expected_ncc, 1e-9);
    EXPECT_GT(weak.value().appearance.peak_sidelobe_ratio, default_tracker.options().peak_sidelobe_ratio_min);
    EXPECT_EQ(weak.value().appearance.best_offset_dx, 0);
    EXPECT_EQ(weak.value().appearance.best_offset_dy, 0);

    ObjectTrackerOptions strong_options;
    strong_options.ncc_strong_threshold = 0.65;
    ObjectTracker strong_tracker = make_tracker(strong_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(strong_tracker, view_of(frame_a), 7U, whole_view));
    const auto strong = strong_tracker.verify_track(7U, view_of(frame_b), std::nullopt);
    ASSERT_TRUE(strong.ok()) << strong.status().message();
    EXPECT_EQ(strong.value().appearance.outcome, AppearanceChannelOutcome::kStrong);
    EXPECT_EQ(strong.value().appearance.peak_ncc, weak.value().appearance.peak_ncc);

    ObjectTrackerOptions none_options;
    none_options.ncc_weak_threshold = 0.85;
    none_options.ncc_strong_threshold = 0.9;
    ObjectTracker none_tracker = make_tracker(none_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(none_tracker, view_of(frame_a), 7U, whole_view));
    const auto none = none_tracker.verify_track(7U, view_of(frame_b), std::nullopt);
    ASSERT_TRUE(none.ok()) << none.status().message();
    EXPECT_EQ(none.value().appearance.outcome, AppearanceChannelOutcome::kNone);
    EXPECT_EQ(none.value().appearance.peak_ncc, weak.value().appearance.peak_ncc);
}

/// Verifying against a view smaller than the track window with a small ROI
/// ratio: the clamped ROI ({43, 43, 3, 3}) is smaller than the 8x8 window in
/// both axes, so the strict offset set is empty and exactly the fallback
/// offset (0, 0) runs on the view-clamped window — visible through
/// best_offset_* == 0 and the reported clamped ROI.
TEST(ObjectTrackerVerificationTest, VerifyTrackFallbackSingleOffsetOnSmallerView) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_a, 44, 44, block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.verification_roi_diagonal_ratio = 0.1;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame_a), 7U, RectF{44.0F, 44.0F, 8.0F, 8.0F}));

    const GrayImage small_view = make_gray_image(46, 46, std::byte{40});
    const auto verified = tracker.verify_track(7U, view_of(small_view), std::nullopt);
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    EXPECT_EQ(verified.value().verification_roi, (RectI{43, 43, 3, 3}));
    const auto query_roi = tracker.verification_roi(7U, view_of(small_view));
    ASSERT_TRUE(query_roi.ok()) << query_roi.status().message();
    EXPECT_EQ(verified.value().verification_roi, query_roi.value());
    EXPECT_EQ(verified.value().appearance.best_offset_dx, 0);
    EXPECT_EQ(verified.value().appearance.best_offset_dy, 0);
    // The fallback window is the flat view corner: no appearance evidence.
    EXPECT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kNone);
    EXPECT_EQ(verified.value().appearance.peak_ncc, 0.0);
}

/// DOD-04 template-set invalidation: on a frame showing a different block,
/// the newly added template becomes the winner and the verification result
/// changes — the stale single-template result is never silently reused.
TEST(ObjectTrackerVerificationTest, VerifyTrackPicksNewlyAddedTemplateBest) {
    const std::vector<uint8_t> block_p = pseudo_block(53);
    const std::vector<uint8_t> block_q = pseudo_block(97);
    GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_a, 8, 8, block_p);
    GrayImage frame_c = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_c, 16, 12, block_q);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.verification_roi_diagonal_ratio = 3.0;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));

    const auto before = tracker.verify_track(7U, view_of(frame_c), std::nullopt);
    ASSERT_TRUE(before.ok()) << before.status().message();
    EXPECT_EQ(before.value().appearance.best_template_index, 0U);
    EXPECT_LT(before.value().appearance.peak_ncc, 1.0);

    // Capture the Q template from frame C through the public pipeline.
    auto template_q = direct_template(view_of(frame_c), RectI{16, 12, 8, 8}, 8);
    ASSERT_TRUE(template_q.ok()) << template_q.status().message();
    TrackTemplate entry;
    entry.fingerprint = template_q.take_value();
    entry.frame_sequence = 2;
    ASSERT_TRUE(tracker.add_template(7U, entry).ok()) << "add_template failed";

    const auto after = tracker.verify_track(7U, view_of(frame_c), std::nullopt);
    ASSERT_TRUE(after.ok()) << after.status().message();
    EXPECT_EQ(after.value().appearance.best_template_index, 1U);
    EXPECT_NEAR(after.value().appearance.peak_ncc, 1.0, 1e-9);
    EXPECT_EQ(after.value().appearance.best_offset_dx, 8);
    EXPECT_EQ(after.value().appearance.best_offset_dy, 4);
    EXPECT_EQ(after.value().appearance.outcome, AppearanceChannelOutcome::kStrong);

    // Identical template added again: equal surfaces, the lower index wins
    // the frozen (peak, PSR, template index) total order.
    TrackTemplate duplicate = entry;
    duplicate.frame_sequence = 3;
    ASSERT_TRUE(tracker.add_template(7U, duplicate).ok());
    const auto tie = tracker.verify_track(7U, view_of(frame_c), std::nullopt);
    ASSERT_TRUE(tie.ok()) << tie.status().message();
    EXPECT_EQ(tie.value().appearance.best_template_index, 1U);
    EXPECT_NEAR(tie.value().appearance.peak_ncc, 1.0, 1e-9);
}

/// Frozen negative-template boundary: the verifier reads positive templates
/// only, so storing the adoption template as an impostor must not change the
/// result bit for bit (the impostor veto is the M7-06 state machine's
/// contract).
TEST(ObjectTrackerVerificationTest, VerifyTrackNeverReadsNegativeTemplates) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_a, 8, 8, block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker plain = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(plain, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));

    ObjectTracker with_negative = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(with_negative, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    const TargetTrack* track = with_negative.find_track(7U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 1U);
    TrackTemplate impostor = track->templates[0];
    impostor.frame_sequence = 9;
    ASSERT_TRUE(with_negative.add_negative_template(7U, impostor).ok());

    const auto plain_result = plain.verify_track(7U, view_of(frame_a), std::nullopt);
    const auto negative_result = with_negative.verify_track(7U, view_of(frame_a), std::nullopt);
    ASSERT_TRUE(plain_result.ok()) << plain_result.status().message();
    ASSERT_TRUE(negative_result.ok()) << negative_result.status().message();
    ASSERT_NO_FATAL_FAILURE(expect_verification_bitwise_equal(plain_result.value(), negative_result.value()));
}

// --- verify_track E2: closure-structure consistency --------------------------------

/// kNoBaseline is reported explicitly until `record_structure_baseline`
/// bootstraps the channel; after recording, identical descriptors are
/// kConsistent with zero deviations and a half-step drift on an exact dyadic
/// baseline reports the exact relative deviation 0.5 (kDeviated).
TEST(ObjectTrackerVerificationTest, RecordBaselineBootstrapsStructureChannel) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    const auto no_baseline = tracker.verify_track(7U, view, descriptors_of(0.5F, 0.5F, 0.5F));
    ASSERT_TRUE(no_baseline.ok()) << no_baseline.status().message();
    EXPECT_EQ(no_baseline.value().structure.outcome, StructureChannelOutcome::kNoBaseline);
    EXPECT_EQ(no_baseline.value().structure.max_deviation, 0.0);

    ASSERT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 3).ok());

    const auto consistent = tracker.verify_track(7U, view, descriptors_of(0.5F, 0.5F, 0.5F));
    ASSERT_TRUE(consistent.ok()) << consistent.status().message();
    EXPECT_EQ(consistent.value().structure.outcome, StructureChannelOutcome::kConsistent);
    EXPECT_EQ(consistent.value().structure.closure_deviation, 0.0);
    EXPECT_EQ(consistent.value().structure.rectangularity_deviation, 0.0);
    EXPECT_EQ(consistent.value().structure.edge_support_deviation, 0.0);
    EXPECT_EQ(consistent.value().structure.max_deviation, 0.0);

    const auto deviated = tracker.verify_track(7U, view, descriptors_of(0.75F, 0.5F, 0.5F));
    ASSERT_TRUE(deviated.ok()) << deviated.status().message();
    EXPECT_EQ(deviated.value().structure.outcome, StructureChannelOutcome::kDeviated);
    EXPECT_DOUBLE_EQ(deviated.value().structure.closure_deviation, 0.5);
    EXPECT_DOUBLE_EQ(deviated.value().structure.rectangularity_deviation, 0.0);
    EXPECT_DOUBLE_EQ(deviated.value().structure.edge_support_deviation, 0.0);
    EXPECT_DOUBLE_EQ(deviated.value().structure.max_deviation, 0.5);
}

/// Inclusive tolerance boundary on exact dyadic values: deviation 0.5 grades
/// kConsistent at tolerance 0.5 (<= is the frozen rule) and kDeviated at 0.4
/// (DOD-04: a threshold change flips the outcome on identical inputs).
TEST(ObjectTrackerVerificationTest, RecordBaselineToleranceBoundaryUsesInclusiveComparison) {
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    const TrackStructureDescriptors current = descriptors_of(0.75F, 0.5F, 0.5F);
    const TrackStructureDescriptors baseline = descriptors_of(0.5F, 0.5F, 0.5F);

    ObjectTrackerOptions inclusive_options;
    inclusive_options.structure_deviation_tolerance = 0.5;
    ObjectTracker inclusive = make_tracker(inclusive_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(inclusive, view, 7U, bounds));
    ASSERT_TRUE(inclusive.record_structure_baseline(7U, baseline, 2).ok());
    const auto at_boundary = inclusive.verify_track(7U, view, current);
    ASSERT_TRUE(at_boundary.ok()) << at_boundary.status().message();
    EXPECT_EQ(at_boundary.value().structure.outcome, StructureChannelOutcome::kConsistent);
    EXPECT_DOUBLE_EQ(at_boundary.value().structure.max_deviation, 0.5);

    ObjectTrackerOptions strict_options;
    strict_options.structure_deviation_tolerance = 0.4;
    ObjectTracker strict = make_tracker(strict_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(strict, view, 7U, bounds));
    ASSERT_TRUE(strict.record_structure_baseline(7U, baseline, 2).ok());
    const auto beyond = strict.verify_track(7U, view, current);
    ASSERT_TRUE(beyond.ok()) << beyond.status().message();
    EXPECT_EQ(beyond.value().structure.outcome, StructureChannelOutcome::kDeviated);
    EXPECT_DOUBLE_EQ(beyond.value().structure.max_deviation, 0.5);
}

/// The single baseline slot is overwrite-neutral in bytes and re-binds the
/// comparison: after re-recording, deviations compare against the NEW values.
TEST(ObjectTrackerVerificationTest, RecordBaselineOverwriteIsByteNeutralAndRebinds) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const int64_t before_baselines = tracker.byte_size();

    ASSERT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.25F, 0.5F, 0.75F), 2).ok());
    EXPECT_EQ(tracker.byte_size(), before_baselines + ObjectTracker::kStructureBaselineOverheadBytes);

    ASSERT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.75F), 3).ok());
    EXPECT_EQ(tracker.byte_size(), before_baselines + ObjectTracker::kStructureBaselineOverheadBytes);

    // Deviation 0.5 identifies baseline 0.5 (the overwrite), not 0.25
    // (which would give 2.0).
    const auto verified = tracker.verify_track(7U, view, descriptors_of(0.75F, 0.5F, 0.75F));
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    EXPECT_EQ(verified.value().structure.outcome, StructureChannelOutcome::kDeviated);
    EXPECT_DOUBLE_EQ(verified.value().structure.closure_deviation, 0.5);
    EXPECT_DOUBLE_EQ(verified.value().structure.max_deviation, 0.5);
}

/// Baseline accounting: +32 on the first recording, released by terminate
/// (the archive keeps its identity record only) and freed together with an
/// evicted track.
TEST(ObjectTrackerVerificationTest, RecordBaselineByteAccountingAndRelease) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    const GrayImage image = make_gray_image(16, 16, std::byte{40});
    const ImageView view = view_of(image);

    // Thumb 8, empty semantics, one observation: 128 + (64 + 64) + 32 = 288.
    constexpr int64_t kTrackBytes = ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes + 64 +
                                    ObjectTracker::kObservationOverheadBytes;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{0.0F, 0.0F, 8.0F, 8.0F}));
    EXPECT_EQ(tracker.byte_size(), kTrackBytes);
    ASSERT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 2).ok());
    EXPECT_EQ(tracker.byte_size(), kTrackBytes + ObjectTracker::kStructureBaselineOverheadBytes);

    ASSERT_TRUE(tracker.terminate(7U, 9).ok());
    EXPECT_EQ(tracker.byte_size(), ObjectTracker::kTrackOverheadBytes);

    // Eviction frees the victim's baseline slot with the track.
    ObjectTrackerOptions eviction_options = options;
    eviction_options.max_targets = 2;
    eviction_options.pool_budget_bytes = 700;
    ObjectTracker evicting = make_tracker(eviction_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(evicting, view, 1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(evicting, view, 2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 1));
    ASSERT_TRUE(evicting.record_structure_baseline(1U, descriptors_of(0.5F, 0.5F, 0.5F), 2).ok());
    ASSERT_TRUE(evicting.record_structure_baseline(2U, descriptors_of(0.5F, 0.5F, 0.5F), 2).ok());
    EXPECT_EQ(evicting.byte_size(), 2 * kTrackBytes + 2 * ObjectTracker::kStructureBaselineOverheadBytes);

    const auto adopted = evicting.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 5);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{1U}));
    EXPECT_EQ(evicting.byte_size(), 2 * kTrackBytes + ObjectTracker::kStructureBaselineOverheadBytes);
}

/// A full pool cannot hold one more baseline slot: kBudgetExceeded with the
/// pool untouched and the channel still reporting kNoBaseline, while a pool
/// with exact room records and stays byte-constant across overwrites.
TEST(ObjectTrackerVerificationTest, RecordBaselineBudgetExceededKeepsPoolAndChannelUntouched) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    const GrayImage image = make_gray_image(16, 16, std::byte{40});
    const ImageView view = view_of(image);
    constexpr int64_t kTrackBytes = ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes + 64 +
                                    ObjectTracker::kObservationOverheadBytes;

    ObjectTrackerOptions exact_options = options;
    exact_options.pool_budget_bytes = kTrackBytes;  // zero headroom
    ObjectTracker full = make_tracker(exact_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(full, view, 7U, RectF{0.0F, 0.0F, 8.0F, 8.0F}));
    EXPECT_EQ(full.byte_size(), kTrackBytes);

    const PoolSnapshot before = snapshot_of(full);
    const auto rejected = full.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 2);
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, full);

    const auto still_missing = full.verify_track(7U, view, descriptors_of(0.5F, 0.5F, 0.5F));
    ASSERT_TRUE(still_missing.ok()) << still_missing.status().message();
    EXPECT_EQ(still_missing.value().structure.outcome, StructureChannelOutcome::kNoBaseline);

    // Exact room: recording succeeds and re-recording stays byte-neutral.
    ObjectTrackerOptions roomy_options = options;
    roomy_options.pool_budget_bytes = kTrackBytes + ObjectTracker::kStructureBaselineOverheadBytes;
    ObjectTracker exact = make_tracker(roomy_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(exact, view, 7U, RectF{0.0F, 0.0F, 8.0F, 8.0F}));
    ASSERT_TRUE(exact.record_structure_baseline(7U, descriptors_of(0.25F, 0.5F, 0.5F), 2).ok());
    EXPECT_EQ(exact.byte_size(), roomy_options.pool_budget_bytes);
    ASSERT_TRUE(exact.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 3).ok());
    EXPECT_EQ(exact.byte_size(), roomy_options.pool_budget_bytes);
}

TEST(ObjectTrackerVerificationTest, RecordBaselineRejectsInvalidInput) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(32, 32, std::byte{40});
    const ImageView view = view_of(image);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    const auto unknown = tracker.record_structure_baseline(99U, descriptors_of(0.5F, 0.5F, 0.5F), 1);
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    ASSERT_TRUE(tracker.terminate(7U, 9).ok());
    const PoolSnapshot archived = snapshot_of(tracker);
    const auto terminated = tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 10);
    EXPECT_EQ(terminated.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(archived, tracker);

    ObjectTracker live = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(live, view, 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    const int64_t bytes = live.byte_size();
    const std::vector<TrackStructureDescriptors> invalid{
        descriptors_of(nan, 0.5F, 0.5F),
        descriptors_of(0.5F, inf, 0.5F),
        descriptors_of(0.5F, 0.5F, -0.25F),
        descriptors_of(1.5F, 0.5F, 0.5F),
    };
    for (const TrackStructureDescriptors& descriptors : invalid) {
        const auto rejected = live.record_structure_baseline(7U, descriptors, 2);
        EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument);
        EXPECT_EQ(live.byte_size(), bytes);
    }
    // The pool still accepts a valid recording afterwards.
    ASSERT_TRUE(live.record_structure_baseline(7U, descriptors_of(0.0F, 0.0F, 0.0F), 3).ok());
    EXPECT_EQ(live.byte_size(), bytes + ObjectTracker::kStructureBaselineOverheadBytes);
}

/// The channels are independent: with the target gone (flat frame, zero NCC
/// everywhere, untrusted flat surface) the E1 channel is kNone while the E2
/// channel still grades the supplied descriptors against the baseline.
TEST(ObjectTrackerVerificationTest, VerifyTrackChannelsAreIndependent) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_a, 8, 8, block);
    const GrayImage frame_gone = make_gray_image(64, 64, std::byte{40});

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    ASSERT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 2).ok());

    const auto verified = tracker.verify_track(7U, view_of(frame_gone), descriptors_of(0.5F, 0.5F, 0.5F));
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    EXPECT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kNone);
    EXPECT_EQ(verified.value().appearance.peak_ncc, 0.0);
    EXPECT_EQ(verified.value().structure.outcome, StructureChannelOutcome::kConsistent);
}

// --- error paths, budget, cancellation ---------------------------------------------

TEST(ObjectTrackerVerificationTest, VerifyTrackRejectsInvalidInputLeavingPoolUntouched) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const PoolSnapshot before = snapshot_of(tracker);

    auto expect_rejected = [&](const ObjectTracker& pool, const mirador::Result<TrackVerification>& result,
                               const PoolSnapshot& reference, const char* label) {
        EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument) << label;
        ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(reference, pool));
    };

    expect_rejected(tracker, tracker.verify_track(99U, view, std::nullopt), before, "unknown track");

    ASSERT_TRUE(tracker.terminate(7U, 9).ok());
    const PoolSnapshot archived = snapshot_of(tracker);
    expect_rejected(tracker, tracker.verify_track(7U, view, std::nullopt), archived, "terminated track");
    ObjectTracker live = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(live, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const PoolSnapshot live_before = snapshot_of(live);

    ImageView null_data;
    null_data.width = 64;
    null_data.height = 64;
    null_data.row_stride_bytes = 64;
    null_data.format = PixelFormat::kGray8;
    expect_rejected(live, live.verify_track(7U, null_data, std::nullopt), live_before, "null view data");

    ImageView zero_sized;
    zero_sized.format = PixelFormat::kGray8;
    expect_rejected(live, live.verify_track(7U, zero_sized, std::nullopt), live_before, "zero-sized view");

    expect_rejected(live, live.verify_track(7U, view, descriptors_of(nan, 0.5F, 0.5F)), live_before, "NaN descriptor");
    expect_rejected(live, live.verify_track(7U, view, descriptors_of(0.5F, inf, 0.5F)), live_before,
                    "infinite descriptor");
    expect_rejected(live, live.verify_track(7U, view, descriptors_of(-0.25F, 0.5F, 0.5F)), live_before,
                    "descriptor below range");
    expect_rejected(live, live.verify_track(7U, view, descriptors_of(0.5F, 0.5F, 1.5F)), live_before,
                    "descriptor above range");

    // A 3x3 view cannot contain the expanded ROI at all.
    const GrayImage tiny = make_gray_image(3, 3, std::byte{40});
    expect_rejected(live, live.verify_track(7U, view_of(tiny), std::nullopt), live_before, "ROI misses the view");

    // ROI intersecting but the fallback window entirely outside the view.
    ObjectTrackerOptions far_options;
    far_options.template_thumb_side = 8;
    ObjectTracker far_tracker = make_tracker(far_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(far_tracker, view, 7U, RectF{50.0F, 50.0F, 8.0F, 8.0F}));
    const PoolSnapshot far_before = snapshot_of(far_tracker);
    const GrayImage medium = make_gray_image(46, 46, std::byte{40});
    expect_rejected(far_tracker, far_tracker.verify_track(7U, view_of(medium), std::nullopt), far_before,
                    "fallback window misses the view");

    // A valid call still succeeds afterwards.
    const auto ok = live.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(ok.ok()) << ok.status().message();
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(live_before, live));
}

TEST(ObjectTrackerVerificationTest, CreateRejectsNonPositiveWorkBudget) {
    ObjectTrackerOptions options;
    options.verification_work_budget_bytes = 0;
    const auto zero = ObjectTracker::create(options);
    EXPECT_EQ(zero.status().code(), ErrorCode::kInvalidArgument);

    options.verification_work_budget_bytes = -5;
    const auto negative = ObjectTracker::create(options);
    EXPECT_EQ(negative.status().code(), ErrorCode::kInvalidArgument);

    options.verification_work_budget_bytes = 1;
    EXPECT_TRUE(ObjectTracker::create(options).ok());
}

/// The frozen planned-work formula, evaluated independently from the header
/// contract for integer geometry: ROI {10, 10, 20, 20} over bounds {16, 16, 8, 8}
/// gives 13x13 offsets (dx and dy in [-6, 6]); with thumb side 8, one template
/// and gray8 (bytes-per-pixel 1) the planned work is
/// 169 * (2*8*8*1 + 64 + 2*1*64) + 169*1*8 = 55432 bytes. One byte below the
/// call fails with kBudgetExceeded, the exact value passes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest ASSERT_* macro expansion dominates the metric
TEST(ObjectTrackerVerificationTest, VerifyTrackWorkBudgetFrozenFormulaBoundary) {
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    const auto planned = [&]() {
        ObjectTrackerOptions probe_options;
        probe_options.template_thumb_side = 8;
        ObjectTracker probe = make_tracker(probe_options);
        EXPECT_TRUE(probe.adopt_track(make_region(7U, bounds), view, 1).ok());
        const auto roi = probe.verification_roi(7U, view);
        EXPECT_TRUE(roi.ok());
        EXPECT_EQ(roi.value(), (RectI{10, 10, 20, 20}));
        const int64_t dx_min = static_cast<int64_t>(roi.value().x) - static_cast<int64_t>(bounds.x);
        const int64_t dx_max =
            static_cast<int64_t>(roi.value().x + roi.value().width) - static_cast<int64_t>(bounds.x + bounds.width);
        const int64_t dy_min = static_cast<int64_t>(roi.value().y) - static_cast<int64_t>(bounds.y);
        const int64_t dy_max =
            static_cast<int64_t>(roi.value().y + roi.value().height) - static_cast<int64_t>(bounds.y + bounds.height);
        EXPECT_GE(dx_max, dx_min);
        const int64_t offsets = (dx_max - dx_min + 1) * (dy_max - dy_min + 1);
        const int64_t per_offset = 2 * 8 * 8 * 1 + 8 * 8 + 2 * 1 * 8 * 8;
        return offsets * per_offset + offsets * 1 * 8;
    }();
    EXPECT_EQ(planned, 55432);

    ObjectTrackerOptions tight_options;
    tight_options.template_thumb_side = 8;
    tight_options.verification_work_budget_bytes = planned - 1;
    ObjectTracker tight = make_tracker(tight_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tight, view, 7U, bounds));
    const auto rejected = tight.verify_track(7U, view, std::nullopt);
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(tight.byte_size(), ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes + 64 +
                                     ObjectTracker::kObservationOverheadBytes);

    ObjectTrackerOptions exact_options = tight_options;
    exact_options.verification_work_budget_bytes = planned;
    ObjectTracker exact = make_tracker(exact_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(exact, view, 7U, bounds));
    const auto accepted = exact.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(accepted.ok()) << accepted.status().message();
}

/// Documented precedence: validation and budget errors beat cancellation —
/// only a fully valid call reports the cancellation itself, and an expired
/// deadline reports kTimeout.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest ASSERT_* macro expansion dominates the metric
TEST(ObjectTrackerVerificationTest, VerifyTrackBudgetAndValidationPrecedeCancellation) {
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);

    ObjectTracker tracker = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    const auto unknown_cancelled = tracker.verify_track(99U, view, std::nullopt, cancelled);
    EXPECT_EQ(unknown_cancelled.status().code(), ErrorCode::kInvalidArgument);
    const auto cancelled_call = tracker.verify_track(7U, view, std::nullopt, cancelled);
    EXPECT_EQ(cancelled_call.status().code(), ErrorCode::kCancelled);
    const auto timed_out = tracker.verify_track(7U, view, std::nullopt, expired);
    EXPECT_EQ(timed_out.status().code(), ErrorCode::kTimeout);
    const auto still_valid = tracker.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(still_valid.ok()) << still_valid.status().message();

    ObjectTrackerOptions tight_options;
    tight_options.template_thumb_side = 8;
    tight_options.verification_work_budget_bytes = 1;
    ObjectTracker tight = make_tracker(tight_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tight, view, 7U, bounds));
    const auto budget_cancelled = tight.verify_track(7U, view, std::nullopt, cancelled);
    EXPECT_EQ(budget_cancelled.status().code(), ErrorCode::kBudgetExceeded);
}

/// The scan polls the context once per offset row: a cancellation that flips
/// after the entry check still aborts the 13x13 grid after the first row with
/// kCancelled and no partial verification.
TEST(ObjectTrackerVerificationTest, VerifyTrackMidScanCancellationAbortsRowScan) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    int polls = 0;
    ExecutionContext flip_after_entry;
    flip_after_entry.is_cancelled = [&polls] { return ++polls > 1; };

    const PoolSnapshot before = snapshot_of(tracker);
    const auto verified = tracker.verify_track(7U, view, std::nullopt, flip_after_entry);
    ASSERT_FALSE(verified.ok());
    EXPECT_EQ(verified.status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(polls, 2) << "expected exactly the entry poll and the first row poll";
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));
}

// --- determinism, coordinate matrix, purity ----------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest ASSERT_* macro expansion dominates the metric
TEST(ObjectTrackerVerificationTest, VerifyTrackBitwiseDeterministicAcrossInstancesAndRepeats) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_a, 8, 8, block);
    GrayImage frame_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_b, 16, 12, block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.verification_roi_diagonal_ratio = 3.0;

    const auto build = [&]() {
        ObjectTracker tracker = make_tracker(options);
        EXPECT_TRUE(tracker.adopt_track(make_region(7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}), view_of(frame_a), 1).ok());
        EXPECT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 2).ok());
        return tracker;
    };

    const ObjectTracker first = build();
    const ObjectTracker second = build();

    const auto one = first.verify_track(7U, view_of(frame_b), descriptors_of(0.5F, 0.5F, 0.5F));
    const auto repeat = first.verify_track(7U, view_of(frame_b), descriptors_of(0.5F, 0.5F, 0.5F));
    const auto other = second.verify_track(7U, view_of(frame_b), descriptors_of(0.5F, 0.5F, 0.5F));
    ASSERT_TRUE(one.ok()) << one.status().message();
    ASSERT_TRUE(repeat.ok()) << repeat.status().message();
    ASSERT_TRUE(other.ok()) << other.status().message();
    ASSERT_NO_FATAL_FAILURE(expect_verification_bitwise_equal(one.value(), repeat.value()));
    ASSERT_NO_FATAL_FAILURE(expect_verification_bitwise_equal(one.value(), other.value()));
    EXPECT_EQ(one.value().appearance.outcome, AppearanceChannelOutcome::kStrong);
    EXPECT_EQ(one.value().structure.outcome, StructureChannelOutcome::kConsistent);
}

/// DOD-03 coordinate matrix: adoption + baseline + verification across all
/// four rotation metadata values on an odd-sized view with non-contiguous
/// stride. The single presented coordinate space keeps the verdicts identical.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest ASSERT_* macro expansion dominates the metric
TEST(ObjectTrackerVerificationTest, VerifyTrackRotationMatrixOddSizeNonContiguousStride) {
    const RectF bounds{5.0F, 4.0F, 8.0F, 6.0F};
    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        ObjectTracker tracker = make_tracker();
        GrayImage image = make_gray_image(21, 15, std::byte{0}, 7);
        for (int32_t y = 0; y < 15; ++y) {
            for (int32_t x = 0; x < 21; ++x) {
                image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x)] =
                    std::byte{noise_pixel(x, y)};
            }
        }
        const ImageView view = view_of(image, rotation);
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 9U, bounds));
        ASSERT_TRUE(tracker.record_structure_baseline(9U, descriptors_of(0.4F, 0.6F, 0.2F), 3).ok())
            << "rotation " << static_cast<int>(rotation);

        const auto verified = tracker.verify_track(9U, view, descriptors_of(0.4F, 0.6F, 0.2F));
        ASSERT_TRUE(verified.ok()) << "rotation " << static_cast<int>(rotation) << ": " << verified.status().message();
        EXPECT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kStrong)
            << "rotation " << static_cast<int>(rotation);
        EXPECT_NEAR(verified.value().appearance.peak_ncc, 1.0, 1e-9) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(verified.value().appearance.best_offset_dx, 0) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(verified.value().appearance.best_offset_dy, 0) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(verified.value().structure.outcome, StructureChannelOutcome::kConsistent)
            << "rotation " << static_cast<int>(rotation);
        const auto query = tracker.verification_roi(9U, view);
        ASSERT_TRUE(query.ok()) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(verified.value().verification_roi, query.value());
    }
}

/// Stride must not leak into the evidence: identical pixels over a tight and
/// a padded buffer produce bit-identical verification results.
TEST(ObjectTrackerVerificationTest, VerifyTrackStrideInvariance) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage tight_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(tight_a, 8, 8, block);
    GrayImage padded_a = make_gray_image(64, 64, std::byte{40}, 16);
    write_gray_block(padded_a, 8, 8, block);
    GrayImage tight_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(tight_b, 16, 12, block);
    GrayImage padded_b = make_gray_image(64, 64, std::byte{40}, 16);
    write_gray_block(padded_b, 16, 12, block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.verification_roi_diagonal_ratio = 3.0;
    ObjectTracker tight_tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tight_tracker, view_of(tight_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    ObjectTracker padded_tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(padded_tracker, view_of(padded_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));

    const auto tight_result = tight_tracker.verify_track(7U, view_of(tight_b), std::nullopt);
    const auto padded_result = padded_tracker.verify_track(7U, view_of(padded_b), std::nullopt);
    ASSERT_TRUE(tight_result.ok()) << tight_result.status().message();
    ASSERT_TRUE(padded_result.ok()) << padded_result.status().message();
    EXPECT_EQ(padded_result.value().verification_roi, tight_result.value().verification_roi);
    EXPECT_EQ(padded_result.value().appearance.peak_ncc, tight_result.value().appearance.peak_ncc);
    EXPECT_EQ(padded_result.value().appearance.peak_sidelobe_ratio,
              tight_result.value().appearance.peak_sidelobe_ratio);
    EXPECT_EQ(padded_result.value().appearance.outcome, tight_result.value().appearance.outcome);
    EXPECT_EQ(padded_result.value().appearance.best_offset_dx, tight_result.value().appearance.best_offset_dx);
    EXPECT_EQ(padded_result.value().appearance.best_offset_dy, tight_result.value().appearance.best_offset_dy);
}

/// The M3-09 format invariance: neutral-gray RGBA frames and their gray8
/// sources produce identical evidence (the exact integer luma maps
/// (v, v, v, 255) back to v).
TEST(ObjectTrackerVerificationTest, VerifyTrackFormatInvarianceGrayVsRgba) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage gray_a = make_gray_image(64, 64, std::byte{40});
    write_gray_block(gray_a, 8, 8, block);
    GrayImage gray_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(gray_b, 16, 12, block);
    const RgbaImage rgba_a = rgba_from_gray(gray_a);
    const RgbaImage rgba_b = rgba_from_gray(gray_b);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.verification_roi_diagonal_ratio = 3.0;
    ObjectTracker gray_tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(gray_tracker, view_of(gray_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    ObjectTracker rgba_tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(rgba_tracker, view_of(rgba_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));

    const auto gray_result = gray_tracker.verify_track(7U, view_of(gray_b), std::nullopt);
    const auto rgba_result = rgba_tracker.verify_track(7U, view_of(rgba_b), std::nullopt);
    ASSERT_TRUE(gray_result.ok()) << gray_result.status().message();
    ASSERT_TRUE(rgba_result.ok()) << rgba_result.status().message();
    EXPECT_EQ(rgba_result.value().verification_roi, gray_result.value().verification_roi);
    EXPECT_EQ(rgba_result.value().appearance.peak_ncc, gray_result.value().appearance.peak_ncc);
    EXPECT_EQ(rgba_result.value().appearance.peak_sidelobe_ratio, gray_result.value().appearance.peak_sidelobe_ratio);
    EXPECT_EQ(rgba_result.value().appearance.outcome, gray_result.value().appearance.outcome);
    EXPECT_EQ(rgba_result.value().appearance.best_offset_dx, gray_result.value().appearance.best_offset_dx);
    EXPECT_EQ(rgba_result.value().appearance.best_offset_dy, gray_result.value().appearance.best_offset_dy);
}

/// The verifier is a pure per-track decision: no pool counter, no evidence
/// field, no state and no layout generation moves — including around baseline
/// recording and failing calls.
TEST(ObjectTrackerVerificationTest, VerifyTrackIsPureConstDecision) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 8U, RectF{40.0F, 8.0F, 8.0F, 8.0F}));
    ASSERT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 2).ok());

    const PoolSnapshot before = snapshot_of(tracker);
    std::vector<TargetTrack> before_tracks;
    for (const uint64_t id : tracker.track_ids()) {
        before_tracks.push_back(*tracker.find_track(id));
    }

    const auto verified = tracker.verify_track(7U, view, descriptors_of(0.5F, 0.5F, 0.5F));
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    const auto rejected = tracker.verify_track(99U, view, std::nullopt);
    EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument);
    const auto refused = tracker.record_structure_baseline(7U, descriptors_of(2.0F, 0.5F, 0.5F), 9);
    EXPECT_EQ(refused.status().code(), ErrorCode::kInvalidArgument);
    const auto queried = tracker.verification_roi(8U, view);
    ASSERT_TRUE(queried.ok()) << queried.status().message();

    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));
    ASSERT_EQ(tracker.track_count(), before_tracks.size());
    for (const TargetTrack& copy : before_tracks) {
        const TargetTrack* track = tracker.find_track(copy.track_id);
        ASSERT_NE(track, nullptr) << "track " << copy.track_id << " vanished";
        EXPECT_EQ(track->state, copy.state);
        EXPECT_EQ(track->last_bounds, copy.last_bounds);
        EXPECT_EQ(track->predicted_center, copy.predicted_center);
        EXPECT_EQ(track->confidence, copy.confidence);
        EXPECT_EQ(track->last_verified_sequence, copy.last_verified_sequence);
        EXPECT_EQ(track->terminated_sequence, copy.terminated_sequence);
        ASSERT_EQ(track->templates.size(), copy.templates.size());
        for (size_t i = 0; i < copy.templates.size(); ++i) {
            EXPECT_EQ(track->templates[i].fingerprint, copy.templates[i].fingerprint);
            EXPECT_EQ(track->templates[i].frame_sequence, copy.templates[i].frame_sequence);
        }
        EXPECT_EQ(track->position_history.size(), copy.position_history.size());
        EXPECT_EQ(track->layout_generation, copy.layout_generation);
    }
    EXPECT_EQ(tracker.layout_generation(), 0U);
}

/// Reset drops the baseline slots with the rest of the pool: the byte counter
/// returns to exactly zero and a fresh adoption starts without a baseline.
TEST(ObjectTrackerVerificationTest, ResetDropsBaselineSlots) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(32, 32, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}));
    ASSERT_TRUE(tracker.record_structure_baseline(7U, descriptors_of(0.5F, 0.5F, 0.5F), 2).ok());
    EXPECT_EQ(tracker.byte_size(), ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes +
                                       static_cast<int64_t>(32 * 32) + ObjectTracker::kObservationOverheadBytes +
                                       ObjectTracker::kStructureBaselineOverheadBytes);

    tracker.reset();
    EXPECT_EQ(tracker.byte_size(), 0);
    EXPECT_EQ(tracker.track_count(), 0U);
    const auto no_baseline = tracker.verify_track(7U, view, descriptors_of(0.5F, 0.5F, 0.5F));
    EXPECT_EQ(no_baseline.status().code(), ErrorCode::kInvalidArgument);  // unknown after reset
}

}  // namespace
