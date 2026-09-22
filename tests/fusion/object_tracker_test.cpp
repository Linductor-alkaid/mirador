// M7-01 (DEC-019/DEC-020) bounded target pool contract tests for
// ObjectTracker: option validation, adoption validation with failure
// atomicity, documented initialization and byte accounting, explicit eviction
// order (terminated first, then oldest verification, then id), terminate
// lifecycle, reset, deterministic enumeration, and the presented-space
// covering crop of the adoption template (all rotations, odd sizes,
// non-contiguous stride, flush edges). Every error path must leave the pool
// untouched.

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
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

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
using mirador::TargetTrack;
using mirador::TrackObservation;
using mirador::TrackState;
using mirador::TrackTemplate;
using mirador::VisualPatchFingerprint;
using mirador::VisualRegion;

constexpr int64_t kGenerousBudget = int64_t{1} << 20;

// --- Fixtures and helpers -----------------------------------------------------

/// Owning RGBA8 pixel buffer with an explicit row stride (padding included).
struct TestImage {
    std::vector<std::byte> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int64_t stride = 0;
};

TestImage make_rgba_image(int32_t width, int32_t height, int64_t stride_padding_bytes = 0) {
    TestImage image;
    image.width = width;
    image.height = height;
    image.stride = static_cast<int64_t>(width) * 4 + stride_padding_bytes;
    // 0xAA poisons the padding; only real pixels are overwritten below.
    image.pixels.assign(static_cast<size_t>(image.stride) * static_cast<size_t>(height), std::byte{0xAA});
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const size_t offset =
                static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x) * 4U;
            const int r = (x * 13 + y * 7) % 256;
            const int g = (x * 5 + y * 29) % 256;
            const int b = (x * 11 + y * 3) % 256;
            image.pixels[offset + 0] = static_cast<std::byte>(r);
            image.pixels[offset + 1] = static_cast<std::byte>(g);
            image.pixels[offset + 2] = static_cast<std::byte>(b);
            image.pixels[offset + 3] = static_cast<std::byte>(255);
        }
    }
    return image;
}

/// Valid non-owning view over the buffer. `width`/`height` are the presented
/// (post-rotation) dimensions; the buffer is indexed in presented coordinates.
ImageView view_of(const TestImage& image, Rotation rotation = Rotation::k0) {
    ImageView view;
    view.data = image.pixels.data();
    view.width = image.width;
    view.height = image.height;
    view.row_stride_bytes = image.stride;
    view.format = PixelFormat::kRgba8;
    view.rotation = rotation;
    return view;
}

VisualRegion make_region(uint64_t stable_id, RectF bounds, float confidence = 0.9F, std::string label = {},
                         std::string text = {}) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.anchor = PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    region.confidence = confidence;
    region.label = std::move(label);
    region.text = std::move(text);
    return region;
}

/// The documented covering ROI of the adoption template: floor on the leading
/// edge, ceil on the trailing edge, clamped into the view.
RectI covering_roi_of(const RectF& bounds, const ImageView& view) {
    const auto x0 = static_cast<int32_t>(std::floor(static_cast<double>(bounds.x)));
    const auto y0 = static_cast<int32_t>(std::floor(static_cast<double>(bounds.y)));
    const auto x1 = static_cast<int32_t>(std::ceil(static_cast<double>(bounds.x + bounds.width)));
    const auto y1 = static_cast<int32_t>(std::ceil(static_cast<double>(bounds.y + bounds.height)));
    const int32_t clamped_x0 = std::max(x0, 0);
    const int32_t clamped_y0 = std::max(y0, 0);
    return RectI{clamped_x0, clamped_y0, std::min(x1, view.width) - clamped_x0, std::min(y1, view.height) - clamped_y0};
}

/// Reference template computed through the public image API on the same view.
mirador::Result<VisualPatchFingerprint> direct_template(const ImageView& view, const RectF& bounds,
                                                        int32_t thumb_side) {
    const auto cropped = mirador::crop(view, covering_roi_of(bounds, view), kGenerousBudget);
    if (!cropped.ok()) {
        return cropped.status();
    }
    return mirador::make_visual_patch_fingerprint(cropped.value().view(), PatchFingerprintParams{thumb_side},
                                                  kGenerousBudget);
}

/// Documented byte accounting of one track (header `byte_size` formula).
int64_t expected_track_bytes(int32_t thumb_side, size_t template_count, size_t history_count, size_t label_bytes,
                             size_t text_bytes) {
    return ObjectTracker::kTrackOverheadBytes +
           static_cast<int64_t>(template_count) *
               (ObjectTracker::kTemplateOverheadBytes + static_cast<int64_t>(thumb_side) * thumb_side) +
           static_cast<int64_t>(history_count) * ObjectTracker::kObservationOverheadBytes +
           static_cast<int64_t>(label_bytes) + static_cast<int64_t>(text_bytes);
}

ObjectTracker make_tracker(const ObjectTrackerOptions& options = {}) {
    auto created = ObjectTracker::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_tracker: " << created.status().message();
        return ObjectTracker{};
    }
    return created.take_value();
}

/// Observable pool state for failure-atomicity comparisons.
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

void expect_track_identity_equal(const TargetTrack& lhs, const TargetTrack& rhs) {
    EXPECT_EQ(lhs.track_id, rhs.track_id);
    EXPECT_EQ(lhs.state, rhs.state);
    EXPECT_EQ(lhs.last_bounds, rhs.last_bounds);
    EXPECT_EQ(lhs.predicted_center, rhs.predicted_center);
    EXPECT_EQ(lhs.layout_generation, rhs.layout_generation);
}

void expect_track_semantics_equal(const TargetTrack& lhs, const TargetTrack& rhs) {
    EXPECT_EQ(lhs.negative_templates.size(), rhs.negative_templates.size());
    EXPECT_EQ(lhs.semantics.label, rhs.semantics.label);
    EXPECT_EQ(lhs.semantics.text, rhs.semantics.text);
    EXPECT_EQ(lhs.confidence, rhs.confidence);
    EXPECT_EQ(lhs.last_verified_sequence, rhs.last_verified_sequence);
    EXPECT_EQ(lhs.terminated_sequence, rhs.terminated_sequence);
}

void expect_observation_equal(const TrackObservation& lhs, const TrackObservation& rhs) {
    EXPECT_EQ(lhs.frame_sequence, rhs.frame_sequence);
    EXPECT_EQ(lhs.bounds, rhs.bounds);
    EXPECT_EQ(lhs.confidence, rhs.confidence);
    EXPECT_EQ(lhs.layout_generation, rhs.layout_generation);
}

void expect_observations_equal(const std::vector<TrackObservation>& lhs, const std::vector<TrackObservation>& rhs) {
    ASSERT_EQ(lhs.size(), rhs.size());
    for (size_t i = 0; i < lhs.size(); ++i) {
        expect_observation_equal(lhs[i], rhs[i]);
    }
}

void expect_template_entry_equal(const TrackTemplate& lhs, const TrackTemplate& rhs) {
    EXPECT_EQ(lhs.fingerprint, rhs.fingerprint);
    EXPECT_EQ(lhs.frame_sequence, rhs.frame_sequence);
    EXPECT_EQ(lhs.layout_generation, rhs.layout_generation);
    EXPECT_EQ(lhs.capture_grade, rhs.capture_grade);
}

void expect_template_entries_equal(const std::vector<TrackTemplate>& lhs, const std::vector<TrackTemplate>& rhs) {
    ASSERT_EQ(lhs.size(), rhs.size());
    for (size_t i = 0; i < lhs.size(); ++i) {
        expect_template_entry_equal(lhs[i], rhs[i]);
    }
}

void expect_tracks_equal(const TargetTrack& lhs, const TargetTrack& rhs) {
    expect_track_identity_equal(lhs, rhs);
    expect_track_semantics_equal(lhs, rhs);
    expect_observations_equal(lhs.position_history, rhs.position_history);
    expect_template_entries_equal(lhs.templates, rhs.templates);
    expect_template_entries_equal(lhs.negative_templates, rhs.negative_templates);
}

/// Adoption of `id` must succeed and report exactly `expected_evicted`.
void expect_adopt_evicts(ObjectTracker& tracker, const ImageView& view, uint64_t id, uint64_t sequence,
                         const std::vector<uint64_t>& expected_evicted) {
    const auto adopted = tracker.adopt_track(make_region(id, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, sequence);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().evicted_track_ids, expected_evicted);
}

/// The stored adoption template of `id` must equal the fingerprint computed
/// directly through the public crop + patch-fingerprint API on the same view.
void expect_adoption_template_matches_direct(const ObjectTracker& tracker, uint64_t id, const ImageView& view,
                                             const RectF& bounds, int32_t thumb_side) {
    const TargetTrack* track = tracker.find_track(id);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 1U);
    const auto reference = direct_template(view, bounds, thumb_side);
    ASSERT_TRUE(reference.ok()) << reference.status().message();
    EXPECT_EQ(track->templates[0].fingerprint, reference.value())
        << "covering-ROI template mismatch for bounds " << bounds.x << "," << bounds.y;
}

/// One full rotation-matrix case: adoption on the rotated view must match the
/// direct computation of the same presented space.
void expect_rotation_adoption_matches_direct(const TestImage& image, Rotation rotation, const RectF& bounds) {
    ObjectTracker tracker = make_tracker();
    const ImageView view = view_of(image, rotation);
    const auto adopted = tracker.adopt_track(make_region(11U, bounds), view, 4);
    ASSERT_TRUE(adopted.ok()) << "rotation " << static_cast<int>(rotation) << ": " << adopted.status().message();
    ASSERT_EQ(tracker.track_count(), 1U);
    expect_adoption_template_matches_direct(tracker, 11U, view, bounds, tracker.options().template_thumb_side);

    const TargetTrack* track = tracker.find_track(11U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->last_bounds, bounds);
    const PointF expected_center{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    EXPECT_EQ(track->predicted_center, expected_center);
}

// --- create: defaults and option validation --------------------------------------

TEST(ObjectTrackerTest, CreateDefaultOptionsProducesEmptyPool) {
    const auto created = ObjectTracker::create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    const ObjectTracker& tracker = created.value();
    EXPECT_EQ(tracker.track_count(), 0U);
    EXPECT_EQ(tracker.byte_size(), 0);
    EXPECT_EQ(tracker.layout_generation(), 0U);
    EXPECT_EQ(tracker.evicted_track_count(), 0U);
    EXPECT_TRUE(tracker.track_ids().empty());
    // Frozen initial defaults from the M7-01 contract freeze.
    EXPECT_EQ(tracker.options().max_targets, 64);
    EXPECT_EQ(tracker.options().template_thumb_side, 32);
    EXPECT_EQ(tracker.options().pool_budget_bytes, int64_t{1} * 1024 * 1024);
    EXPECT_DOUBLE_EQ(tracker.options().ncc_weak_threshold, 0.6);
    EXPECT_DOUBLE_EQ(tracker.options().ncc_strong_threshold, 0.8);
}

TEST(ObjectTrackerTest, CreateRejectsPoolOptionsOutsideInclusiveRange) {
    auto expect_rejected = [](const ObjectTrackerOptions& options) {
        const auto created = ObjectTracker::create(options);
        EXPECT_FALSE(created.ok());
        if (!created.ok()) {
            EXPECT_EQ(created.status().code(), ErrorCode::kInvalidArgument);
        }
    };
    ObjectTrackerOptions options;

    options = {};
    options.max_targets = 0;
    expect_rejected(options);
    options.max_targets = 4097;
    expect_rejected(options);

    options = {};
    options.max_position_history = 0;
    expect_rejected(options);
    options.max_position_history = 1025;
    expect_rejected(options);

    options = {};
    options.max_templates = 0;
    expect_rejected(options);
    options.max_templates = 17;
    expect_rejected(options);

    options = {};
    options.max_negative_templates = -1;
    expect_rejected(options);
    options.max_negative_templates = 17;
    expect_rejected(options);

    options = {};
    options.template_thumb_side = 7;
    expect_rejected(options);
    options.template_thumb_side = 65;
    expect_rejected(options);

    options = {};
    options.pool_budget_bytes = 0;
    expect_rejected(options);
    options.pool_budget_bytes = -1;
    expect_rejected(options);

    options = {};
    options.uncertain_frame_limit = 0;
    expect_rejected(options);
    options.uncertain_frame_limit = 4097;
    expect_rejected(options);

    options = {};
    options.max_generation_lag = 0;
    expect_rejected(options);
    options.max_generation_lag = 1025;
    expect_rejected(options);
}

TEST(ObjectTrackerTest, CreateAcceptsInclusiveRangeBoundaries) {
    ObjectTrackerOptions minimum;
    minimum.max_targets = 1;
    minimum.max_position_history = 1;
    minimum.max_templates = 1;
    minimum.max_negative_templates = 0;
    minimum.template_thumb_side = 8;
    minimum.pool_budget_bytes = 1;
    minimum.uncertain_frame_limit = 1;
    minimum.max_generation_lag = 1;
    minimum.ncc_weak_threshold = 0.0;
    minimum.ncc_strong_threshold = 0.0;
    minimum.peak_sidelobe_ratio_min = 1.0;
    minimum.structure_deviation_tolerance = 0.0;
    minimum.verification_roi_diagonal_ratio = std::numeric_limits<double>::denorm_min();
    minimum.redetect_backoff_base_frames = 1;
    minimum.redetect_backoff_max_frames = 1;  // == base is allowed
    minimum.redetect_max_attempts = 1;
    EXPECT_TRUE(ObjectTracker::create(minimum).ok());

    ObjectTrackerOptions maximum;
    maximum.max_targets = 4096;
    maximum.max_position_history = 1024;
    maximum.max_templates = 16;
    maximum.max_negative_templates = 16;
    maximum.template_thumb_side = 64;
    maximum.pool_budget_bytes = kGenerousBudget;
    maximum.uncertain_frame_limit = 4096;
    maximum.max_generation_lag = 1024;
    maximum.ncc_weak_threshold = 1.0;
    maximum.ncc_strong_threshold = 1.0;  // weak == strong is allowed
    maximum.peak_sidelobe_ratio_min = 1.0;
    maximum.structure_deviation_tolerance = 1.0;
    maximum.verification_roi_diagonal_ratio = 8.0;
    maximum.redetect_backoff_base_frames = 1;
    maximum.redetect_backoff_max_frames = 1;
    maximum.redetect_max_attempts = 1;
    EXPECT_TRUE(ObjectTracker::create(maximum).ok());
}

TEST(ObjectTrackerTest, CreateRejectsInvalidVerificationThresholds) {
    auto expect_rejected = [](const ObjectTrackerOptions& options) {
        const auto created = ObjectTracker::create(options);
        EXPECT_FALSE(created.ok());
        if (!created.ok()) {
            EXPECT_EQ(created.status().code(), ErrorCode::kInvalidArgument);
        }
    };
    ObjectTrackerOptions options;

    options = {};
    options.ncc_weak_threshold = -0.1;
    expect_rejected(options);
    options.ncc_weak_threshold = 1.1;
    expect_rejected(options);
    options.ncc_weak_threshold = 0.9;  // weak above strong (0.8 default)
    expect_rejected(options);
    options.ncc_strong_threshold = 1.1;
    expect_rejected(options);

    options = {};
    options.peak_sidelobe_ratio_min = 0.9;
    expect_rejected(options);

    options = {};
    options.structure_deviation_tolerance = -0.1;
    expect_rejected(options);
    options.structure_deviation_tolerance = 1.1;
    expect_rejected(options);

    options = {};
    options.verification_roi_diagonal_ratio = 0.0;  // (0, 8] excludes zero
    expect_rejected(options);
    options.verification_roi_diagonal_ratio = 8.1;
    expect_rejected(options);
}

TEST(ObjectTrackerTest, CreateRejectsInvalidRedetectParameters) {
    auto expect_rejected = [](const ObjectTrackerOptions& options) {
        const auto created = ObjectTracker::create(options);
        EXPECT_FALSE(created.ok());
        if (!created.ok()) {
            EXPECT_EQ(created.status().code(), ErrorCode::kInvalidArgument);
        }
    };
    ObjectTrackerOptions options;

    options = {};
    options.redetect_backoff_base_frames = 0;
    expect_rejected(options);

    options = {};
    options.redetect_backoff_base_frames = 4;
    options.redetect_backoff_max_frames = 3;  // maximum below the base
    expect_rejected(options);

    options = {};
    options.redetect_max_attempts = 0;
    expect_rejected(options);
}

// --- adopt_track: error paths leave the pool untouched ----------------------------

TEST(ObjectTrackerTest, AdoptCancelledContextTakesPriorityOverValidation) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };

    // Even a region that fails every validation must report cancellation.
    const auto invalid_region = tracker.adopt_track(make_region(0U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), view, 0, cancelled);
    EXPECT_EQ(invalid_region.status().code(), ErrorCode::kCancelled);

    const auto valid_region = tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), view, 0, cancelled);
    EXPECT_EQ(valid_region.status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(tracker.track_count(), 0U);
    EXPECT_EQ(tracker.byte_size(), 0);
}

TEST(ObjectTrackerTest, AdoptExpiredDeadlineReturnsTimeoutAndKeepsPool) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);

    // The deadline wins over the invalid stable_id too (checked second).
    const auto invalid_region = tracker.adopt_track(make_region(0U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), view, 0, expired);
    EXPECT_EQ(invalid_region.status().code(), ErrorCode::kTimeout);

    const PoolSnapshot before = snapshot_of(tracker);
    const auto valid_region = tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), view, 0, expired);
    EXPECT_EQ(valid_region.status().code(), ErrorCode::kTimeout);
    expect_pool_untouched(before, tracker);
}

TEST(ObjectTrackerTest, AdoptRejectsZeroStableId) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    const PoolSnapshot before = snapshot_of(tracker);
    const auto adopted = tracker.adopt_track(make_region(0U, RectF{1.0F, 1.0F, 4.0F, 4.0F}), view, 3);
    EXPECT_EQ(adopted.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(before, tracker);
}

TEST(ObjectTrackerTest, AdoptRejectsNonFiniteOrEmptyBounds) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    const std::vector<RectF> invalid_bounds{
        RectF{nan, 1.0F, 4.0F, 4.0F},    // NaN x
        RectF{1.0F, 1.0F, inf, 4.0F},    // infinite width
        RectF{1.0F, 1.0F, 4.0F, 0.0F},   // empty height
        RectF{1.0F, 1.0F, 0.0F, 4.0F},   // empty width
        RectF{1.0F, 1.0F, -4.0F, 4.0F},  // negative width
    };
    for (const RectF& bounds : invalid_bounds) {
        const PoolSnapshot before = snapshot_of(tracker);
        const auto adopted = tracker.adopt_track(make_region(7U, bounds), view, 0);
        EXPECT_EQ(adopted.status().code(), ErrorCode::kInvalidArgument) << "bounds " << bounds.x << "," << bounds.y;
        expect_pool_untouched(before, tracker);
    }
}

TEST(ObjectTrackerTest, AdoptRejectsInvalidView) {
    ObjectTracker tracker = make_tracker();
    const PoolSnapshot before = snapshot_of(tracker);

    ImageView null_data;
    null_data.width = 16;
    null_data.height = 12;
    null_data.row_stride_bytes = 64;
    null_data.format = PixelFormat::kRgba8;
    const auto null_adopted = tracker.adopt_track(make_region(7U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), null_data, 0);
    EXPECT_EQ(null_adopted.status().code(), ErrorCode::kInvalidArgument);

    ImageView zero_sized;
    zero_sized.format = PixelFormat::kRgba8;
    const auto empty_adopted = tracker.adopt_track(make_region(7U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), zero_sized, 0);
    EXPECT_EQ(empty_adopted.status().code(), ErrorCode::kInvalidArgument);

    expect_pool_untouched(before, tracker);
}

TEST(ObjectTrackerTest, AdoptRejectsBoundsOutsidePresentedView) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    const std::vector<RectF> outside_bounds{
        RectF{-0.5F, 0.0F, 4.0F, 4.0F},       // left edge outside
        RectF{0.0F, -0.25F, 4.0F, 4.0F},      // top edge outside
        RectF{12.0F, 0.0F, 4.5F, 4.0F},       // right edge beyond width 16
        RectF{0.0F, 8.0F, 4.0F, 4.5F},        // bottom edge beyond height 12
        RectF{1.0e30F, 0.0F, 1.0e30F, 4.0F},  // far outside
    };
    for (const RectF& bounds : outside_bounds) {
        const PoolSnapshot before = snapshot_of(tracker);
        const auto adopted = tracker.adopt_track(make_region(7U, bounds), view, 0);
        EXPECT_EQ(adopted.status().code(), ErrorCode::kInvalidArgument) << "bounds " << bounds.x << "," << bounds.y;
        expect_pool_untouched(before, tracker);
    }

    // Exactly touching every edge is inside, not outside.
    const auto flush = tracker.adopt_track(make_region(7U, RectF{0.0F, 0.0F, 16.0F, 12.0F}), view, 0);
    ASSERT_TRUE(flush.ok()) << flush.status().message();
}

TEST(ObjectTrackerTest, AdoptRejectsDuplicateIdIncludingTerminatedArchive) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(7U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), view, 0).ok());
    const TargetTrack* resident = tracker.find_track(7U);
    ASSERT_NE(resident, nullptr);
    const TargetTrack resident_copy = *resident;

    // Live duplicate.
    const PoolSnapshot before_live = snapshot_of(tracker);
    const auto live_duplicate = tracker.adopt_track(make_region(7U, RectF{8.0F, 4.0F, 4.0F, 4.0F}), view, 1);
    EXPECT_EQ(live_duplicate.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(before_live, tracker);
    resident = tracker.find_track(7U);
    ASSERT_NE(resident, nullptr);
    expect_tracks_equal(*resident, resident_copy);

    // Terminated ids stay reserved: the archive must not be silently reused.
    ASSERT_TRUE(tracker.terminate(7U, 9).ok());
    const PoolSnapshot before_terminated = snapshot_of(tracker);
    const TargetTrack terminated_copy = *tracker.find_track(7U);
    const auto terminated_duplicate = tracker.adopt_track(make_region(7U, RectF{8.0F, 4.0F, 4.0F, 4.0F}), view, 10);
    EXPECT_EQ(terminated_duplicate.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(before_terminated, tracker);
    expect_tracks_equal(*tracker.find_track(7U), terminated_copy);
}

// --- adopt_track: success path ----------------------------------------------------

TEST(ObjectTrackerTest, AdoptInitializesTrackStateAndObservation) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(24, 16);
    const ImageView view = view_of(image);
    const RectF bounds{2.0F, 3.0F, 8.0F, 6.0F};

    const auto adopted = tracker.adopt_track(make_region(42U, bounds, 0.7F, "button", "OK"), view, 5);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().track_id, 42U);
    EXPECT_TRUE(adopted.value().evicted_track_ids.empty());
    EXPECT_EQ(tracker.track_count(), 1U);
    EXPECT_EQ(tracker.evicted_track_count(), 0U);

    const TargetTrack* track = tracker.find_track(42U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->track_id, 42U);
    EXPECT_EQ(track->state, TrackState::kTracking);
    EXPECT_EQ(track->last_bounds, bounds);
    EXPECT_EQ(track->predicted_center, (PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F}));
    EXPECT_EQ(track->layout_generation, 0U);

    ASSERT_EQ(track->position_history.size(), 1U);
    EXPECT_EQ(track->position_history[0].frame_sequence, 5U);
    EXPECT_EQ(track->position_history[0].bounds, bounds);
    EXPECT_EQ(track->position_history[0].confidence, 0.7F);
    EXPECT_EQ(track->position_history[0].layout_generation, 0U);

    ASSERT_EQ(track->templates.size(), 1U);
    EXPECT_EQ(track->templates[0].frame_sequence, 5U);
    EXPECT_EQ(track->templates[0].layout_generation, 0U);
    EXPECT_EQ(track->templates[0].capture_grade, mirador::EvidenceGrade::kConfirmed);
    EXPECT_EQ(track->templates[0].fingerprint.thumb_width, 8);
    EXPECT_EQ(track->templates[0].fingerprint.thumb_height, 8);
    EXPECT_EQ(track->templates[0].fingerprint.thumbnail_gray.size(), 64U);

    EXPECT_TRUE(track->negative_templates.empty());
    EXPECT_EQ(track->semantics.label, "button");
    EXPECT_EQ(track->semantics.text, "OK");
    EXPECT_EQ(track->confidence, 0.7F);
    EXPECT_EQ(track->last_verified_sequence, 5U);
    EXPECT_EQ(track->terminated_sequence, 0U);
}

TEST(ObjectTrackerTest, AdoptCapturesTemplateMatchingDirectPublicComputation) {
    // Fractional bounds exercise the documented covering rule (floor leading
    // edge, ceil trailing edge, clamp into the view).
    const std::vector<RectF> cases{
        RectF{1.4F, 2.6F, 7.2F, 5.8F},    // -> ROI {1, 2, 8, 7}
        RectF{2.5F, 3.5F, 6.4F, 7.1F},    // -> ROI {2, 3, 7, 8}
        RectF{0.0F, 0.0F, 16.0F, 12.0F},  // whole view
    };
    int32_t next_id = 1;
    for (const RectF& bounds : cases) {
        const auto id = static_cast<uint64_t>(next_id);
        ObjectTracker tracker = make_tracker();
        const TestImage image = make_rgba_image(16, 12);
        const ImageView view = view_of(image);

        const auto adopted = tracker.adopt_track(make_region(id, bounds), view, 2);
        ASSERT_TRUE(adopted.ok()) << adopted.status().message();
        expect_adoption_template_matches_direct(tracker, id, view, bounds, tracker.options().template_thumb_side);
        ++next_id;
    }
}

TEST(ObjectTrackerTest, AdoptByteAccountingFollowsDocumentedFormula) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 16;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(32, 24);
    const ImageView view = view_of(image);

    const auto adopted =
        tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 16.0F, 16.0F}, 0.9F, "btn", "hello"), view, 0);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();

    const int64_t expected = expected_track_bytes(16, 1, 1, std::string("btn").size(), std::string("hello").size());
    EXPECT_EQ(tracker.byte_size(), expected);
    EXPECT_LE(tracker.byte_size(), tracker.options().pool_budget_bytes);
}

TEST(ObjectTrackerTest, AdoptClampsConfidenceIntoUnitRange) {
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    ObjectTracker high_tracker = make_tracker();
    ASSERT_TRUE(high_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 1.5F), view, 0).ok());
    const TargetTrack* high = high_tracker.find_track(1U);
    ASSERT_NE(high, nullptr);
    EXPECT_EQ(high->confidence, 1.0F);
    ASSERT_EQ(high->position_history.size(), 1U);
    EXPECT_EQ(high->position_history[0].confidence, 1.0F);

    ObjectTracker low_tracker = make_tracker();
    ASSERT_TRUE(low_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, -0.5F), view, 0).ok());
    const TargetTrack* low = low_tracker.find_track(1U);
    ASSERT_NE(low, nullptr);
    EXPECT_EQ(low->confidence, 0.0F);
    ASSERT_EQ(low->position_history.size(), 1U);
    EXPECT_EQ(low->position_history[0].confidence, 0.0F);
}

TEST(ObjectTrackerTest, AdoptTruncatesSemanticsTextAt1024Bytes) {
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    const std::string exact(ObjectTracker::kMaxSemanticsTextBytes, 'a');
    ObjectTracker exact_tracker = make_tracker();
    ASSERT_TRUE(
        exact_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0.9F, "", exact), view, 0).ok());
    const TargetTrack* exact_track = exact_tracker.find_track(1U);
    ASSERT_NE(exact_track, nullptr);
    EXPECT_EQ(exact_track->semantics.text.size(), ObjectTracker::kMaxSemanticsTextBytes);
    EXPECT_EQ(exact_track->semantics.text, exact);

    const std::string over(ObjectTracker::kMaxSemanticsTextBytes + 1, 'x');
    ObjectTracker over_tracker = make_tracker();
    ASSERT_TRUE(over_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0.9F, "", over), view, 0).ok());
    const TargetTrack* over_track = over_tracker.find_track(1U);
    ASSERT_NE(over_track, nullptr);
    ASSERT_EQ(over_track->semantics.text.size(), ObjectTracker::kMaxSemanticsTextBytes);
    EXPECT_EQ(over_track->semantics.text, over.substr(0, ObjectTracker::kMaxSemanticsTextBytes));
    // Truncated text: accounted bytes use the stored length (default thumb 32).
    EXPECT_EQ(over_tracker.byte_size(), expected_track_bytes(32, 1, 1, 0, ObjectTracker::kMaxSemanticsTextBytes));
}

// --- explicit eviction under pressure ---------------------------------------------

TEST(ObjectTrackerTest, AdoptCountPressureEvictsOldestLiveFirstAndReportsIds) {
    ObjectTrackerOptions options;
    options.max_targets = 2;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 1).ok());

    const auto adopted = tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 2);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().track_id, 3U);
    EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{1U}));
    EXPECT_EQ(tracker.track_ids(), (std::vector<uint64_t>{2U, 3U}));
    EXPECT_EQ(tracker.evicted_track_count(), 1U);

    // max_targets == 1: every new adoption replaces the single resident.
    ObjectTrackerOptions single;
    single.max_targets = 1;
    ObjectTracker solo = make_tracker(single);
    ASSERT_TRUE(solo.adopt_track(make_region(9U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const auto replaced = solo.adopt_track(make_region(10U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 1);
    ASSERT_TRUE(replaced.ok()) << replaced.status().message();
    EXPECT_EQ(replaced.value().evicted_track_ids, (std::vector<uint64_t>{9U}));
    EXPECT_EQ(solo.track_ids(), (std::vector<uint64_t>{10U}));
}

TEST(ObjectTrackerTest, AdoptEvictsTerminatedTrackBeforeOlderLiveTracks) {
    ObjectTrackerOptions options;
    options.max_targets = 3;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 1).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 2).ok());
    // Terminate the NEWEST track: eviction must still prefer it over the
    // older live tracks.
    ASSERT_TRUE(tracker.terminate(3U, 9).ok());

    const auto adopted = tracker.adopt_track(make_region(4U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 3);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{3U}));
    EXPECT_EQ(tracker.track_ids(), (std::vector<uint64_t>{1U, 2U, 4U}));
    EXPECT_EQ(tracker.evicted_track_count(), 1U);
}

TEST(ObjectTrackerTest, AdoptTerminatedEvictionOrdersByOldestVerification) {
    ObjectTrackerOptions options;
    options.max_targets = 4;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    for (const uint64_t id : {1U, 2U, 3U, 4U}) {
        ASSERT_TRUE(tracker.adopt_track(make_region(id, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, id - 1).ok());
    }
    // Terminate ids 2 (lvs 1) and 4 (lvs 3): among terminated tracks the
    // oldest last_verified_sequence evicts first.
    ASSERT_TRUE(tracker.terminate(2U, 20).ok());
    ASSERT_TRUE(tracker.terminate(4U, 21).ok());

    expect_adopt_evicts(tracker, view, 5U, 10, {2U});
    expect_adopt_evicts(tracker, view, 6U, 11, {4U});
    // All terminated archives are gone; the next victim is the oldest live
    // track by last_verified_sequence (id 1, lvs 0, vs id 3, lvs 2).
    expect_adopt_evicts(tracker, view, 7U, 12, {1U});

    EXPECT_EQ(tracker.evicted_track_count(), 3U);
    EXPECT_EQ(tracker.track_ids(), (std::vector<uint64_t>{3U, 5U, 6U, 7U}));
}

TEST(ObjectTrackerTest, AdoptLiveEvictionTieBreaksByVerificationThenId) {
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);

    // Older last_verified_sequence wins even with the higher track id.
    ObjectTrackerOptions options;
    options.max_targets = 2;
    ObjectTracker sequence_tracker = make_tracker(options);
    ASSERT_TRUE(sequence_tracker.adopt_track(make_region(200U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 5).ok());
    ASSERT_TRUE(sequence_tracker.adopt_track(make_region(100U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 7).ok());
    const auto by_sequence = sequence_tracker.adopt_track(make_region(300U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 8);
    ASSERT_TRUE(by_sequence.ok()) << by_sequence.status().message();
    EXPECT_EQ(by_sequence.value().evicted_track_ids, (std::vector<uint64_t>{200U}));

    // Equal last_verified_sequence: the lower track id evicts first.
    ObjectTracker tie_tracker = make_tracker(options);
    ASSERT_TRUE(tie_tracker.adopt_track(make_region(200U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 5).ok());
    ASSERT_TRUE(tie_tracker.adopt_track(make_region(100U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 5).ok());
    const auto by_id = tie_tracker.adopt_track(make_region(300U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 6);
    ASSERT_TRUE(by_id.ok()) << by_id.status().message();
    EXPECT_EQ(by_id.value().evicted_track_ids, (std::vector<uint64_t>{100U}));
}

TEST(ObjectTrackerTest, AdoptByteBudgetPressureEvictsUntilInsertionFits) {
    // Thumb 8, no semantics: one track accounts exactly
    // 128 + (64 + 64) + 32 = 288 bytes. A capture headroom that fits the
    // 8x8 ROI crop (256 bytes) but not the insertion (288 bytes) must make
    // byte-pressure eviction run until the insertion fits.
    constexpr int64_t kTrackBytes = ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes + 64 +
                                    ObjectTracker::kObservationOverheadBytes;
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);

    ObjectTrackerOptions window;
    window.template_thumb_side = 8;
    window.pool_budget_bytes = 2 * kTrackBytes + 264;
    ObjectTracker window_tracker = make_tracker(window);
    ASSERT_TRUE(window_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(window_tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 1).ok());
    const auto window_adopted = window_tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 2);
    ASSERT_TRUE(window_adopted.ok()) << window_adopted.status().message();
    EXPECT_EQ(window_adopted.value().evicted_track_ids, (std::vector<uint64_t>{1U}));
    EXPECT_EQ(window_tracker.byte_size(), 2 * kTrackBytes);
    EXPECT_EQ(window_tracker.evicted_track_count(), 1U);
    EXPECT_EQ(window_tracker.track_ids(), (std::vector<uint64_t>{2U, 3U}));
    EXPECT_LE(window_tracker.byte_size(), window_tracker.options().pool_budget_bytes);
}

TEST(ObjectTrackerTest, AdoptByteExactFitPoolEvictsToAdmitTrack) {
    // Contract (task brief): with pool_budget_bytes exactly fitting two
    // tracks, the third adoption evicts until the insertion fits and
    // succeeds against the byte-full pool (headroom zero).
    constexpr int64_t kTrackBytes = ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes + 64 +
                                    ObjectTracker::kObservationOverheadBytes;
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.pool_budget_bytes = 2 * kTrackBytes;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 1).ok());
    ASSERT_EQ(tracker.byte_size(), 2 * kTrackBytes);

    const auto adopted = tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 2);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{1U}));
    EXPECT_EQ(tracker.byte_size(), 2 * kTrackBytes);
    EXPECT_LE(tracker.byte_size(), tracker.options().pool_budget_bytes);
    EXPECT_EQ(tracker.track_ids(), (std::vector<uint64_t>{2U, 3U}));
    EXPECT_EQ(tracker.evicted_track_count(), 1U);
}

TEST(ObjectTrackerTest, AdoptTrackLargerThanWholeBudgetFailsWithoutEviction) {
    // The worst-case insertion account uses the UNTRUNCATED text: 288 + 1040
    // bytes vs a 1320-byte budget (which would still fit the truncated 1024).
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.pool_budget_bytes = 1320;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    const std::string huge_text(1040, 'x');

    const PoolSnapshot before = snapshot_of(tracker);
    const auto adopted =
        tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0.9F, "", huge_text), view, 0);
    EXPECT_EQ(adopted.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, tracker);
    EXPECT_EQ(tracker.evicted_track_count(), 0U);  // no eviction ran for a doomed insert
}

TEST(ObjectTrackerTest, AdoptTemplateCaptureBudgetFailureLeavesPoolUntouched) {
    // The template capture budget is the pool headroom: an ROI crop larger
    // than the remaining budget must fail the adoption without touching the
    // pool, while a fitting ROI on the same tracker still succeeds.
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.pool_budget_bytes = 700;  // 288-byte track + headroom that skips the 20x20 RGBA crop (1600)
    ObjectTracker tracker = make_tracker(options);

    const TestImage large = make_rgba_image(100, 100);
    const ImageView large_view = view_of(large);
    const TestImage small = make_rgba_image(8, 8);
    const ImageView small_view = view_of(small);

    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), small_view, 0).ok());
    const PoolSnapshot before = snapshot_of(tracker);
    const TargetTrack resident_copy = *tracker.find_track(1U);

    const auto oversized = tracker.adopt_track(make_region(2U, RectF{10.0F, 10.0F, 20.0F, 20.0F}), large_view, 1);
    EXPECT_EQ(oversized.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident_copy);
    EXPECT_EQ(tracker.evicted_track_count(), 0U);

    // The pool is still usable afterwards.
    const auto retried = tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), small_view, 2);
    ASSERT_TRUE(retried.ok()) << retried.status().message();
}

// --- terminate ---------------------------------------------------------------------

TEST(ObjectTrackerTest, TerminateRejectsUnknownAndRepeatedIds) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());

    const PoolSnapshot before = snapshot_of(tracker);
    EXPECT_EQ(tracker.terminate(999U, 1).status().code(), ErrorCode::kInvalidArgument);  // unknown id
    ASSERT_TRUE(tracker.terminate(1U, 1).ok());
    const PoolSnapshot archived = snapshot_of(tracker);
    EXPECT_EQ(tracker.terminate(1U, 2).status().code(), ErrorCode::kInvalidArgument);  // already terminated
    expect_pool_untouched(archived, tracker);                                          // failed calls change nothing
}

TEST(ObjectTrackerTest, TerminateClosesIdentityAndReleasesTemplates) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(16, 12);
    const ImageView view = view_of(image);
    const RectF bounds{0.0F, 0.0F, 8.0F, 8.0F};
    ASSERT_TRUE(tracker.adopt_track(make_region(5U, bounds, 0.6F, "btn", "ok"), view, 0).ok());
    const int64_t bytes_before = tracker.byte_size();

    ASSERT_TRUE(tracker.terminate(5U, 77).ok());
    const TargetTrack* track = tracker.find_track(5U);
    ASSERT_NE(track, nullptr);  // failure stays visible; no silent pool clearing
    EXPECT_EQ(track->state, TrackState::kTerminated);
    EXPECT_EQ(track->terminated_sequence, 77U);
    EXPECT_TRUE(track->templates.empty());
    EXPECT_TRUE(track->negative_templates.empty());
    EXPECT_TRUE(track->position_history.empty());  // history is released with the templates
    // The identity record stays visible.
    EXPECT_EQ(track->track_id, 5U);
    EXPECT_EQ(track->last_bounds, bounds);
    EXPECT_EQ(track->confidence, 0.6F);
    EXPECT_EQ(track->semantics.label, "btn");
    EXPECT_EQ(track->semantics.text, "ok");
    EXPECT_LT(tracker.byte_size(), bytes_before);
    EXPECT_EQ(tracker.track_ids(), (std::vector<uint64_t>{5U}));
}

TEST(ObjectTrackerTest, TerminateByteAccountingKeepsIdentityRecordOnly) {
    // Ruling: termination releases templates, negative templates AND position
    // history; the archived track keeps only its identity record (128 + kept
    // semantics bytes). Thumb 8, label "btn" (3), text "hi" (2): 293 -> 133.
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0.9F, "btn", "hi"), view, 0).ok());
    ASSERT_EQ(tracker.byte_size(), expected_track_bytes(8, 1, 1, 3, 2));

    ASSERT_TRUE(tracker.terminate(1U, 9).ok());
    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    EXPECT_TRUE(track->templates.empty());
    EXPECT_TRUE(track->negative_templates.empty());
    EXPECT_TRUE(track->position_history.empty());
    EXPECT_EQ(tracker.byte_size(), ObjectTracker::kTrackOverheadBytes + 3 + 2);
    EXPECT_LE(tracker.byte_size(), tracker.options().pool_budget_bytes);
}

TEST(ObjectTrackerTest, TerminateFreesBudgetBytesForNewAdoption) {
    // With correct accounting the terminated track releases its template and
    // history bytes, but a third 288-byte track still needs one eviction (the
    // terminated archive, which also evicts first).
    constexpr int64_t kTrackBytes = ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes + 64 +
                                    ObjectTracker::kObservationOverheadBytes;
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.pool_budget_bytes = 2 * kTrackBytes;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 1).ok());
    ASSERT_TRUE(tracker.terminate(1U, 10).ok());

    const auto adopted = tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 2);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{1U}));
    EXPECT_EQ(tracker.byte_size(), 2 * kTrackBytes);
    EXPECT_LE(tracker.byte_size(), tracker.options().pool_budget_bytes);
    EXPECT_EQ(tracker.track_ids(), (std::vector<uint64_t>{2U, 3U}));
}

// --- reset and enumeration ---------------------------------------------------------

TEST(ObjectTrackerTest, ResetClearsPoolAndAllowsReAdoption) {
    ObjectTrackerOptions options;
    options.max_targets = 2;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 1).ok());
    ASSERT_TRUE(tracker.terminate(1U, 5).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 2).ok());  // evicts 1
    ASSERT_GT(tracker.byte_size(), 0);
    ASSERT_EQ(tracker.evicted_track_count(), 1U);

    tracker.reset();
    EXPECT_EQ(tracker.track_count(), 0U);
    EXPECT_EQ(tracker.byte_size(), 0);
    EXPECT_EQ(tracker.evicted_track_count(), 0U);
    EXPECT_EQ(tracker.layout_generation(), 0U);
    EXPECT_TRUE(tracker.track_ids().empty());
    EXPECT_EQ(tracker.find_track(2U), nullptr);

    // The id space is freed: the same id adopts again from scratch.
    const auto readopted = tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 3);
    ASSERT_TRUE(readopted.ok()) << readopted.status().message();
    EXPECT_EQ(readopted.value().track_id, 2U);
    EXPECT_TRUE(readopted.value().evicted_track_ids.empty());
}

TEST(ObjectTrackerTest, TrackIdsEnumerateAscendingAndFindTrackResolves) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(24, 24);
    const ImageView view = view_of(image);

    ASSERT_TRUE(tracker.adopt_track(make_region(30U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(10U, RectF{8.0F, 0.0F, 8.0F, 8.0F}), view, 1).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(20U, RectF{0.0F, 8.0F, 8.0F, 8.0F}), view, 2).ok());

    EXPECT_EQ(tracker.track_ids(), (std::vector<uint64_t>{10U, 20U, 30U}));

    const TargetTrack* track = tracker.find_track(20U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->track_id, 20U);
    EXPECT_EQ(track->last_bounds, (RectF{0.0F, 8.0F, 8.0F, 8.0F}));
    EXPECT_EQ(tracker.find_track(99U), nullptr);

    // The pointer stays valid across const calls on the tracker.
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 1, 1, 0, 0) * 3);
    EXPECT_EQ(track->track_id, 20U);
}

// --- coordinate matrix (DOD-03) and determinism ------------------------------------

TEST(ObjectTrackerTest, AdoptCoversAllRotationsOnOddSizedView) {
    const TestImage image = make_rgba_image(17, 11);  // odd presented size
    const RectF bounds{1.5F, 2.5F, 8.2F, 6.4F};       // fractional edges

    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        expect_rotation_adoption_matches_direct(image, rotation, bounds);
    }
}

TEST(ObjectTrackerTest, AdoptMatchesAcrossNonContiguousStride) {
    const int32_t width = 12;
    const int32_t height = 9;
    const TestImage tight = make_rgba_image(width, height);
    const TestImage padded = make_rgba_image(width, height, 16);  // 16 padding bytes per row

    const RectF bounds{0.5F, 1.5F, 6.2F, 4.8F};
    ObjectTracker tight_tracker = make_tracker();
    ASSERT_TRUE(tight_tracker.adopt_track(make_region(1U, bounds), view_of(tight), 0).ok());
    ObjectTracker padded_tracker = make_tracker();
    ASSERT_TRUE(padded_tracker.adopt_track(make_region(1U, bounds), view_of(padded), 0).ok());

    const TargetTrack* tight_track = tight_tracker.find_track(1U);
    const TargetTrack* padded_track = padded_tracker.find_track(1U);
    ASSERT_NE(tight_track, nullptr);
    ASSERT_NE(padded_track, nullptr);
    ASSERT_EQ(tight_track->templates.size(), 1U);
    ASSERT_EQ(padded_track->templates.size(), 1U);

    // Each template equals the direct computation on its own view...
    const auto tight_reference = direct_template(view_of(tight), bounds, 32);
    const auto padded_reference = direct_template(view_of(padded), bounds, 32);
    ASSERT_TRUE(tight_reference.ok()) << tight_reference.status().message();
    ASSERT_TRUE(padded_reference.ok()) << padded_reference.status().message();
    EXPECT_EQ(tight_track->templates[0].fingerprint, tight_reference.value());
    EXPECT_EQ(padded_track->templates[0].fingerprint, padded_reference.value());
    // ...and the stride does not change the fingerprint bytes.
    EXPECT_EQ(padded_track->templates[0].fingerprint, tight_track->templates[0].fingerprint);
}

TEST(ObjectTrackerTest, AdoptRegionFlushWithViewEdges) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(12, 9);
    const ImageView view = view_of(image);

    const RectF top_left{0.0F, 0.0F, 12.0F, 9.0F};     // flush at x=0, y=0 and full view
    const RectF bottom_right{4.0F, 3.0F, 8.0F, 6.0F};  // flush at right/bottom edges
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, top_left), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, bottom_right), view, 1).ok());

    for (const auto& [id, bounds] :
         std::array<std::pair<uint64_t, RectF>, 2>{std::pair{1U, top_left}, std::pair{2U, bottom_right}}) {
        const TargetTrack* track = tracker.find_track(id);
        ASSERT_NE(track, nullptr);
        ASSERT_EQ(track->templates.size(), 1U);
        const auto reference = direct_template(view, bounds, tracker.options().template_thumb_side);
        ASSERT_TRUE(reference.ok()) << reference.status().message();
        EXPECT_EQ(track->templates[0].fingerprint, reference.value());
    }
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 1, 1, 0, 0) * 2);
}

/// One deterministic operation sequence: four adoptions (the fourth forces a
/// count eviction), one termination, one more adoption (evicts the terminated
/// archive). `ok` is false when any step fails; failures are reported inside.
struct SequenceRun {
    bool ok = false;
    ObjectTracker tracker;
    std::vector<std::vector<uint64_t>> evictions;
    std::vector<uint64_t> adopted_ids;
};

SequenceRun run_object_tracker_sequence(const ObjectTrackerOptions& options, const ImageView& view,
                                        const RectF& bounds) {
    SequenceRun run;
    run.tracker = make_tracker(options);
    for (const uint64_t id : {10U, 20U, 30U, 40U}) {
        const auto adopted = run.tracker.adopt_track(make_region(id, bounds, 0.8F, "a", "b"), view, id - 10U);
        if (!adopted.ok()) {
            ADD_FAILURE() << "adopt " << id << ": " << adopted.status().message();
            return run;
        }
        run.adopted_ids.push_back(adopted.value().track_id);
        run.evictions.push_back(adopted.value().evicted_track_ids);
    }
    if (!run.tracker.terminate(30U, 50).ok()) {
        ADD_FAILURE() << "terminate 30 failed";
        return run;
    }
    const auto last = run.tracker.adopt_track(make_region(60U, bounds, 0.8F, "a", "b"), view, 5);
    if (!last.ok()) {
        ADD_FAILURE() << "adopt 60: " << last.status().message();
        return run;
    }
    run.adopted_ids.push_back(last.value().track_id);
    run.evictions.push_back(last.value().evicted_track_ids);
    run.ok = true;
    return run;
}

void expect_pool_runs_identical(const SequenceRun& first, const SequenceRun& second) {
    // At least one adoption must have evicted something, so the determinism
    // comparison actually covers eviction bookkeeping.
    EXPECT_TRUE(std::any_of(first.evictions.begin(), first.evictions.end(),
                            [](const std::vector<uint64_t>& evicted) { return !evicted.empty(); }));
    EXPECT_EQ(first.adopted_ids, second.adopted_ids);
    EXPECT_EQ(first.evictions, second.evictions);
    EXPECT_EQ(first.tracker.track_ids(), second.tracker.track_ids());
    EXPECT_EQ(first.tracker.byte_size(), second.tracker.byte_size());
    EXPECT_EQ(first.tracker.evicted_track_count(), second.tracker.evicted_track_count());
}

void expect_track_maps_identical(const ObjectTracker& first, const ObjectTracker& second) {
    const std::vector<uint64_t> ids = first.track_ids();
    ASSERT_EQ(ids.size(), second.track_ids().size());
    for (const uint64_t id : ids) {
        const TargetTrack* lhs = first.find_track(id);
        const TargetTrack* rhs = second.find_track(id);
        ASSERT_NE(lhs, nullptr);
        ASSERT_NE(rhs, nullptr);
        expect_tracks_equal(*lhs, *rhs);  // includes exact fingerprint bytes
    }
}

TEST(ObjectTrackerTest, IdenticalSequencesYieldBitwiseIdenticalPools) {
    // Exact 3-track byte accounting plus headroom for the next template
    // capture, so the sequence forces count evictions (capture runs before
    // eviction and draws on the current headroom).
    constexpr int64_t kTrackBytes = ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes +
                                    static_cast<int64_t>(16) * 16 + ObjectTracker::kObservationOverheadBytes + 2;
    ObjectTrackerOptions options;
    options.template_thumb_side = 16;
    options.max_targets = 3;
    options.pool_budget_bytes = 3 * kTrackBytes + 1024;

    const TestImage image = make_rgba_image(24, 16);
    const RectF bounds{1.0F, 2.0F, 16.0F, 12.0F};
    const ImageView view = view_of(image);

    const SequenceRun first = run_object_tracker_sequence(options, view, bounds);
    const SequenceRun second = run_object_tracker_sequence(options, view, bounds);
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    expect_pool_runs_identical(first, second);
    expect_track_maps_identical(first.tracker, second.tracker);
    EXPECT_LE(first.tracker.byte_size(), first.tracker.options().pool_budget_bytes);
    EXPECT_LE(second.tracker.byte_size(), first.tracker.options().pool_budget_bytes);
}

}  // namespace
