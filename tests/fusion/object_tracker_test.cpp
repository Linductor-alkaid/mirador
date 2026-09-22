// M7-01 (DEC-019/DEC-020) bounded target pool contract tests for
// ObjectTracker: option validation, adoption validation with failure
// atomicity, documented initialization and byte accounting, explicit eviction
// order (terminated first, then oldest verification, then id), terminate
// lifecycle, reset, deterministic enumeration, and the presented-space
// covering crop of the adoption template (all rotations, odd sizes,
// non-contiguous stride, flush edges). Every error path must leave the pool
// untouched. The M7-02 section below independently verifies the bounded pool
// primitives (record_observation, add_template, add_negative_template,
// advance_layout_generation, observations_in_generation), the per-resource
// eviction counters and the byte_size formula.

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
using mirador::EvidenceGrade;
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

// --- M7-02 bounded pool primitives: independent verification suite -------------------
//
// Written against the `ObjectTracker` header contract: observation stamping
// and clamping, drop-oldest eviction with explicit counters, the pinned
// adoption template, generation grouping, failure atomicity, and the
// documented byte_size formula.

/// Synthetic template whose fingerprint is dimensionally valid for `side`.
/// `fill` seeds every thumbnail byte so pinned-index preservation can be
/// compared byte for byte; `tag` distinguishes entries in eviction checks.
TrackTemplate synthetic_template(int32_t side, uint8_t fill, uint64_t tag, uint64_t frame_sequence = 0) {
    TrackTemplate entry;
    entry.fingerprint.content_hash = tag;
    entry.fingerprint.dhash = tag;
    entry.fingerprint.thumb_width = side;
    entry.fingerprint.thumb_height = side;
    entry.fingerprint.thumbnail_gray.assign(static_cast<size_t>(side) * static_cast<size_t>(side), std::byte{fill});
    entry.frame_sequence = frame_sequence;
    entry.capture_grade = EvidenceGrade::kConfirmed;
    return entry;
}

/// Cumulative eviction tallies of one pool (RULE-06 accounting).
struct EvictionTally {
    uint64_t tracks = 0;
    uint64_t observations = 0;
    uint64_t templates = 0;
    uint64_t negative_templates = 0;
};

EvictionTally tally_of(const ObjectTracker& tracker) {
    return EvictionTally{tracker.evicted_track_count(), tracker.evicted_observation_count(),
                         tracker.evicted_template_count(), tracker.evicted_negative_template_count()};
}

void expect_tally_equal(const EvictionTally& expected, const ObjectTracker& tracker) {
    EXPECT_EQ(tracker.evicted_track_count(), expected.tracks);
    EXPECT_EQ(tracker.evicted_observation_count(), expected.observations);
    EXPECT_EQ(tracker.evicted_template_count(), expected.templates);
    EXPECT_EQ(tracker.evicted_negative_template_count(), expected.negative_templates);
}

/// The documented byte formula recomputed from the observable pool contents:
/// per track, overhead + template and negative-template bytes (overhead +
/// thumbnail each) + observation bytes + semantics label/text lengths.
int64_t observable_pool_bytes(const ObjectTracker& tracker) {
    int64_t bytes = 0;
    for (const uint64_t id : tracker.track_ids()) {
        const TargetTrack* track = tracker.find_track(id);
        if (track == nullptr) {
            ADD_FAILURE() << "track " << id << " vanished between track_ids and find_track";
            continue;
        }
        bytes += ObjectTracker::kTrackOverheadBytes;
        for (const TrackTemplate& entry : track->templates) {
            bytes +=
                ObjectTracker::kTemplateOverheadBytes + static_cast<int64_t>(entry.fingerprint.thumbnail_gray.size());
        }
        for (const TrackTemplate& entry : track->negative_templates) {
            bytes +=
                ObjectTracker::kTemplateOverheadBytes + static_cast<int64_t>(entry.fingerprint.thumbnail_gray.size());
        }
        bytes += static_cast<int64_t>(track->position_history.size()) * ObjectTracker::kObservationOverheadBytes;
        bytes += static_cast<int64_t>(track->semantics.label.size());
        bytes += static_cast<int64_t>(track->semantics.text.size());
    }
    return bytes;
}

/// byte_size must always equal the recomputed formula and stay in budget.
void expect_byte_invariant(const ObjectTracker& tracker) {
    EXPECT_EQ(tracker.byte_size(), observable_pool_bytes(tracker));
    EXPECT_LE(tracker.byte_size(), tracker.options().pool_budget_bytes);
}

/// History ordering by frame sequence, oldest first.
void expect_history_frames(const TargetTrack& track, const std::vector<uint64_t>& frames) {
    ASSERT_EQ(track.position_history.size(), frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(track.position_history[i].frame_sequence, frames[i]) << "history slot " << i;
    }
}

// --- record_observation ---------------------------------------------------------------

TEST(ObjectTrackerTest, RecordObservationAppendsStampedClampedEntriesOldestFirst) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0.9F, "btn", "ok"), view, 0).ok());

    const RectF moved{1.0F, 2.0F, 3.0F, 4.0F};
    ASSERT_TRUE(tracker.record_observation(1U, moved, 1.5F, 1).ok());    // clamps high
    ASSERT_TRUE(tracker.record_observation(1U, moved, -0.25F, 2).ok());  // clamps low
    ASSERT_TRUE(tracker.record_observation(1U, moved, 1.0F, 3).ok());    // exact bounds are kept
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.0F, 4).ok());

    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->position_history.size(), 5U);
    EXPECT_EQ(track->position_history[0].frame_sequence, 0U);  // adoption entry stays first
    EXPECT_EQ(track->position_history[0].bounds, (RectF{0.0F, 0.0F, 8.0F, 8.0F}));
    EXPECT_EQ(track->position_history[0].confidence, 0.9F);
    EXPECT_EQ(track->position_history[1].confidence, 1.0F);
    EXPECT_EQ(track->position_history[2].confidence, 0.0F);
    EXPECT_EQ(track->position_history[3].confidence, 1.0F);
    EXPECT_EQ(track->position_history[4].confidence, 0.0F);
    for (const TrackObservation& entry : track->position_history) {
        EXPECT_EQ(entry.layout_generation, 0U);  // no bump happened yet
        EXPECT_TRUE(entry.bounds == moved || entry.bounds == (RectF{0.0F, 0.0F, 8.0F, 8.0F}));
    }
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 1, 5, 3, 2));
    expect_byte_invariant(tracker);
    expect_tally_equal(EvictionTally{}, tracker);
}

TEST(ObjectTrackerTest, RecordObservationValidationMatrixLeavesPoolByteExact) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0.6F, "btn", "ok"), view, 5).ok());
    const TargetTrack resident = *tracker.find_track(1U);
    const PoolSnapshot before = snapshot_of(tracker);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const RectF valid{1.0F, 1.0F, 2.0F, 2.0F};

    const std::vector<std::pair<uint64_t, RectF>> invalid_cases{
        {999U, valid},                         // unknown track
        {1U, RectF{nan, 1.0F, 2.0F, 2.0F}},    // NaN x
        {1U, RectF{1.0F, nan, 2.0F, 2.0F}},    // NaN y
        {1U, RectF{1.0F, 1.0F, inf, 2.0F}},    // infinite width
        {1U, RectF{1.0F, 1.0F, 2.0F, -inf}},   // infinite height
        {1U, RectF{1.0F, 1.0F, 0.0F, 2.0F}},   // zero width
        {1U, RectF{1.0F, 1.0F, 2.0F, 0.0F}},   // zero height
        {1U, RectF{1.0F, 1.0F, -2.0F, 2.0F}},  // negative width
        {1U, RectF{1.0F, 1.0F, 2.0F, -2.0F}},  // negative height
    };
    for (const auto& [id, bounds] : invalid_cases) {
        const auto rejected = tracker.record_observation(id, bounds, 0.5F, 6);
        EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument)
            << "id " << id << " bounds " << bounds.x << "," << bounds.y << "," << bounds.width << "," << bounds.height;
    }
    expect_pool_untouched(before, tracker);
    expect_byte_invariant(tracker);
    expect_tally_equal(EvictionTally{}, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident);

    // The terminated archive rejects observations as well.
    ASSERT_TRUE(tracker.terminate(1U, 9).ok());
    const PoolSnapshot archived_snapshot = snapshot_of(tracker);
    const TargetTrack archived = *tracker.find_track(1U);
    const auto terminated = tracker.record_observation(1U, valid, 0.5F, 10);
    EXPECT_EQ(terminated.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(archived_snapshot, tracker);
    expect_tracks_equal(*tracker.find_track(1U), archived);
    expect_tally_equal(EvictionTally{}, tracker);
}

TEST(ObjectTrackerTest, RecordObservationOverflowDropsOldestEntryAndCountsEvictions) {
    ObjectTrackerOptions options;
    options.max_position_history = 4;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());

    const RectF moved{2.0F, 2.0F, 3.0F, 3.0F};
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 1).ok());
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 2).ok());
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 3).ok());
    expect_tally_equal(EvictionTally{}, tracker);
    // The 5th entry overflows the capacity of 4: the adoption entry drops.
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 4).ok());
    EXPECT_EQ(tracker.evicted_observation_count(), 1U);
    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    expect_history_frames(*track, {1U, 2U, 3U, 4U});
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 1, 4, 0, 0));  // the swap is byte-neutral

    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 5).ok());
    EXPECT_EQ(tracker.evicted_observation_count(), 2U);
    track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    expect_history_frames(*track, {2U, 3U, 4U, 5U});
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 1, 4, 0, 0));
    expect_byte_invariant(tracker);
}

TEST(ObjectTrackerTest, RecordObservationByteFullPoolSwapsAtCapacityButRejectsGrowth) {
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    const RectF moved{1.0F, 1.0F, 2.0F, 2.0F};

    // Byte-full pool with the history at capacity: the drop-oldest swap frees
    // exactly the insertion bytes, so it must still succeed (no silent drop).
    ObjectTrackerOptions swap_options;
    swap_options.template_thumb_side = 8;
    swap_options.max_position_history = 2;
    swap_options.pool_budget_bytes = expected_track_bytes(8, 1, 2, 0, 0);
    ObjectTracker swap_tracker = make_tracker(swap_options);
    ASSERT_TRUE(swap_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(swap_tracker.record_observation(1U, moved, 0.5F, 1).ok());
    ASSERT_EQ(swap_tracker.byte_size(), swap_tracker.options().pool_budget_bytes);

    ASSERT_TRUE(swap_tracker.record_observation(1U, moved, 0.5F, 2).ok());
    EXPECT_EQ(swap_tracker.evicted_observation_count(), 1U);
    EXPECT_EQ(swap_tracker.byte_size(), swap_tracker.options().pool_budget_bytes);
    const TargetTrack* track = swap_tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    expect_history_frames(*track, {1U, 2U});
    expect_byte_invariant(swap_tracker);

    // Byte-full pool with the history below capacity: growth must fail
    // explicitly instead of growing or dropping silently.
    ObjectTrackerOptions grow_options;
    grow_options.template_thumb_side = 8;
    grow_options.max_position_history = 2;
    grow_options.pool_budget_bytes = expected_track_bytes(8, 1, 1, 0, 0);
    ObjectTracker grow_tracker = make_tracker(grow_options);
    ASSERT_TRUE(grow_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_EQ(grow_tracker.byte_size(), grow_tracker.options().pool_budget_bytes);

    const PoolSnapshot before = snapshot_of(grow_tracker);
    const TargetTrack resident = *grow_tracker.find_track(1U);
    const auto rejected = grow_tracker.record_observation(1U, moved, 0.5F, 1);
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, grow_tracker);
    expect_tracks_equal(*grow_tracker.find_track(1U), resident);
    expect_tally_equal(EvictionTally{}, grow_tracker);
}

TEST(ObjectTrackerTest, RecordObservationNeverTouchesTrackIdentityOrEvidenceFields) {
    ObjectTrackerOptions options;
    options.max_position_history = 2;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    const RectF adopted_bounds{0.0F, 0.0F, 8.0F, 8.0F};
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, adopted_bounds, 0.6F, "btn", "ok"), view, 5).ok());
    const TargetTrack resident = *tracker.find_track(1U);

    const RectF moved{3.0F, 3.0F, 2.0F, 2.0F};
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.0F, 6).ok());
    ASSERT_TRUE(tracker.record_observation(1U, moved, 1.0F, 7).ok());  // evicts the adoption entry
    ASSERT_TRUE(tracker.advance_layout_generation().ok());
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.3F, 8).ok());  // evicts again

    // Identity, evidence bookkeeping, semantics and templates stay untouched;
    // only the bounded history moved.
    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->track_id, resident.track_id);
    EXPECT_EQ(track->state, resident.state);
    EXPECT_EQ(track->last_bounds, resident.last_bounds);
    EXPECT_EQ(track->predicted_center, resident.predicted_center);
    EXPECT_EQ(track->confidence, resident.confidence);
    EXPECT_EQ(track->last_verified_sequence, resident.last_verified_sequence);
    EXPECT_EQ(track->terminated_sequence, resident.terminated_sequence);
    EXPECT_EQ(track->layout_generation, resident.layout_generation);
    EXPECT_EQ(track->semantics.label, resident.semantics.label);
    EXPECT_EQ(track->semantics.text, resident.semantics.text);
    ASSERT_EQ(track->templates.size(), 1U);
    expect_template_entry_equal(track->templates[0], resident.templates[0]);
    ASSERT_EQ(track->position_history.size(), 2U);
    EXPECT_EQ(track->position_history[1].frame_sequence, 8U);
    EXPECT_EQ(track->position_history[1].layout_generation, 1U);
    expect_byte_invariant(tracker);
}

// --- add_template -----------------------------------------------------------------------

TEST(ObjectTrackerTest, AddTemplateStoresEntriesVerbatimAfterPinnedAdoptionTemplate) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 1U);
    const TrackTemplate adoption = track->templates[0];

    TrackTemplate first = synthetic_template(32, 0x11, 11U, 10);
    first.capture_grade = EvidenceGrade::kTentative;
    first.layout_generation = 3;
    const TrackTemplate second = synthetic_template(32, 0x12, 12U, 11);
    ASSERT_TRUE(tracker.add_template(1U, first).ok());
    ASSERT_TRUE(tracker.add_template(1U, second).ok());

    track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 3U);
    expect_template_entry_equal(track->templates[0], adoption);  // pinned index 0 untouched
    expect_template_entry_equal(track->templates[1], first);     // stored verbatim, no re-stamping
    expect_template_entry_equal(track->templates[2], second);
    EXPECT_EQ(tracker.evicted_template_count(), 0U);
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 3, 1, 0, 0));
    expect_byte_invariant(tracker);
}

TEST(ObjectTrackerTest, AddTemplateFingerprintMatrixRejectsAndKeepsPoolUntouched) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack resident = *tracker.find_track(1U);
    const PoolSnapshot before = snapshot_of(tracker);

    TrackTemplate wrong_width = synthetic_template(32, 0x21, 21U);
    wrong_width.fingerprint.thumb_width = 16;
    TrackTemplate wrong_height = synthetic_template(32, 0x22, 22U);
    wrong_height.fingerprint.thumb_height = 64;
    TrackTemplate short_bytes = synthetic_template(32, 0x23, 23U);
    short_bytes.fingerprint.thumbnail_gray.pop_back();
    TrackTemplate long_bytes = synthetic_template(32, 0x24, 24U);
    long_bytes.fingerprint.thumbnail_gray.push_back(std::byte{0x7F});

    const std::vector<TrackTemplate> invalid_cases{wrong_width, wrong_height, short_bytes, long_bytes};
    for (const TrackTemplate& entry : invalid_cases) {
        const auto rejected = tracker.add_template(1U, entry);
        EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument) << "tag " << entry.fingerprint.content_hash;
    }
    const auto unknown = tracker.add_template(999U, synthetic_template(32, 0x25, 25U));
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    expect_pool_untouched(before, tracker);
    expect_byte_invariant(tracker);
    expect_tally_equal(EvictionTally{}, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident);

    // The terminated archive rejects templates as well.
    ASSERT_TRUE(tracker.terminate(1U, 9).ok());
    const PoolSnapshot archived_snapshot = snapshot_of(tracker);
    const TargetTrack archived = *tracker.find_track(1U);
    const auto terminated = tracker.add_template(1U, synthetic_template(32, 0x26, 26U));
    EXPECT_EQ(terminated.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(archived_snapshot, tracker);
    expect_tracks_equal(*tracker.find_track(1U), archived);
}

TEST(ObjectTrackerTest, AddTemplateOverflowEvictsOldestUnpinnedAndKeepsAdoptionTemplateByteWise) {
    ObjectTrackerOptions options;
    options.max_templates = 3;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    const TrackTemplate adoption = track->templates[0];

    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(32, 0x31, 31U, 10)).ok());
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(32, 0x32, 32U, 11)).ok());
    expect_tally_equal(EvictionTally{}, tracker);
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(32, 0x33, 33U, 12)).ok());  // evicts tag 31

    EXPECT_EQ(tracker.evicted_template_count(), 1U);
    track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 3U);
    EXPECT_EQ(track->templates[0].fingerprint, adoption.fingerprint);  // pinned, byte-wise
    EXPECT_EQ(track->templates[1].fingerprint.content_hash, 32U);
    EXPECT_EQ(track->templates[2].fingerprint.content_hash, 33U);

    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(32, 0x34, 34U, 13)).ok());  // evicts tag 32
    EXPECT_EQ(tracker.evicted_template_count(), 2U);
    track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 3U);
    EXPECT_EQ(track->templates[0].fingerprint, adoption.fingerprint);
    EXPECT_EQ(track->templates[1].fingerprint.content_hash, 33U);
    EXPECT_EQ(track->templates[2].fingerprint.content_hash, 34U);
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 3, 1, 0, 0));
    expect_byte_invariant(tracker);
}

TEST(ObjectTrackerTest, AddTemplateSoleCapacityFailsAsBudgetExceeded) {
    ObjectTrackerOptions options;
    options.max_templates = 1;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack resident = *tracker.find_track(1U);
    const PoolSnapshot before = snapshot_of(tracker);

    const auto rejected = tracker.add_template(1U, synthetic_template(32, 0x41, 41U));
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, tracker);
    expect_byte_invariant(tracker);
    expect_tally_equal(EvictionTally{}, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident);
}

TEST(ObjectTrackerTest, AddTemplateByteFullPoolRejectsWithoutEviction) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.pool_budget_bytes = expected_track_bytes(8, 1, 1, 0, 0);
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_EQ(tracker.byte_size(), tracker.options().pool_budget_bytes);
    const TargetTrack resident = *tracker.find_track(1U);
    const PoolSnapshot before = snapshot_of(tracker);

    const auto rejected = tracker.add_template(1U, synthetic_template(8, 0x42, 42U));
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident);
    expect_tally_equal(EvictionTally{}, tracker);
    expect_byte_invariant(tracker);
}

TEST(ObjectTrackerTest, AddTemplateByteFullPoolSwapsAtCapacity) {
    // The budget fits the adoption track plus exactly one more template: the
    // second add only fits because the drop-oldest eviction frees its bytes.
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_templates = 2;
    options.pool_budget_bytes = expected_track_bytes(8, 2, 1, 0, 0);
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    const TrackTemplate adoption = track->templates[0];

    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(8, 0x51, 51U)).ok());  // fills the budget exactly
    ASSERT_EQ(tracker.byte_size(), tracker.options().pool_budget_bytes);
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(8, 0x52, 52U)).ok());  // fits only via eviction

    EXPECT_EQ(tracker.evicted_template_count(), 1U);
    EXPECT_EQ(tracker.byte_size(), tracker.options().pool_budget_bytes);
    track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 2U);
    EXPECT_EQ(track->templates[0].fingerprint, adoption.fingerprint);
    EXPECT_EQ(track->templates[1].fingerprint.content_hash, 52U);  // tag 51 was dropped
    expect_byte_invariant(tracker);
}

TEST(ObjectTrackerTest, AddTemplateInvalidEntryAtCapacityDoesNotEvict) {
    // Validation must run before the eviction logic: an invalid entry against
    // a full, byte-exact pool must fail without dropping the oldest entry.
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_templates = 2;
    options.pool_budget_bytes = expected_track_bytes(8, 2, 1, 0, 0);
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(8, 0x53, 53U)).ok());
    ASSERT_EQ(tracker.byte_size(), tracker.options().pool_budget_bytes);

    const PoolSnapshot before = snapshot_of(tracker);
    const TargetTrack resident = *tracker.find_track(1U);
    TrackTemplate bad = synthetic_template(8, 0x54, 54U);
    bad.fingerprint.thumb_width = 16;
    const auto rejected = tracker.add_template(1U, bad);
    EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(before, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident);  // tag 53 is still resident
    expect_tally_equal(EvictionTally{}, tracker);
    expect_byte_invariant(tracker);
}

// --- add_negative_template ----------------------------------------------------------------

TEST(ObjectTrackerTest, AddNegativeTemplateZeroCapacityFailsAsBudgetExceeded) {
    ObjectTrackerOptions options;
    options.max_negative_templates = 0;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack resident = *tracker.find_track(1U);
    const PoolSnapshot before = snapshot_of(tracker);

    const auto rejected = tracker.add_negative_template(1U, synthetic_template(32, 0x61, 61U));
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident);
    expect_tally_equal(EvictionTally{}, tracker);
    expect_byte_invariant(tracker);

    // The pool stays usable for the other primitives.
    EXPECT_TRUE(tracker.record_observation(1U, RectF{1.0F, 1.0F, 2.0F, 2.0F}, 0.5F, 1).ok());
    EXPECT_TRUE(tracker.add_template(1U, synthetic_template(32, 0x62, 62U)).ok());
}

TEST(ObjectTrackerTest, AddNegativeTemplateRejectsUnknownTerminatedAndBadFingerprints) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack resident = *tracker.find_track(1U);
    const PoolSnapshot before = snapshot_of(tracker);

    TrackTemplate wrong_width = synthetic_template(32, 0x71, 71U);
    wrong_width.fingerprint.thumb_width = 8;
    TrackTemplate wrong_height = synthetic_template(32, 0x72, 72U);
    wrong_height.fingerprint.thumb_height = 8;
    TrackTemplate short_bytes = synthetic_template(32, 0x73, 73U);
    short_bytes.fingerprint.thumbnail_gray.pop_back();

    const std::vector<TrackTemplate> invalid_cases{wrong_width, wrong_height, short_bytes};
    for (const TrackTemplate& entry : invalid_cases) {
        const auto rejected = tracker.add_negative_template(1U, entry);
        EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument) << "tag " << entry.fingerprint.content_hash;
    }
    const auto unknown = tracker.add_negative_template(999U, synthetic_template(32, 0x74, 74U));
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    expect_pool_untouched(before, tracker);
    expect_byte_invariant(tracker);
    expect_tally_equal(EvictionTally{}, tracker);
    expect_tracks_equal(*tracker.find_track(1U), resident);

    ASSERT_TRUE(tracker.terminate(1U, 9).ok());
    const PoolSnapshot archived_snapshot = snapshot_of(tracker);
    const TargetTrack archived = *tracker.find_track(1U);
    const auto terminated = tracker.add_negative_template(1U, synthetic_template(32, 0x75, 75U));
    EXPECT_EQ(terminated.status().code(), ErrorCode::kInvalidArgument);
    expect_pool_untouched(archived_snapshot, tracker);
    expect_tracks_equal(*tracker.find_track(1U), archived);
}

TEST(ObjectTrackerTest, AddNegativeTemplateOverflowEvictsOldestAndCountsIndependently) {
    ObjectTrackerOptions options;
    options.max_negative_templates = 2;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const TargetTrack* track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    const TrackTemplate adoption = track->templates[0];

    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(32, 0x81, 81U, 10)).ok());
    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(32, 0x82, 82U, 11)).ok());
    expect_tally_equal(EvictionTally{}, tracker);
    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(32, 0x83, 83U, 12)).ok());  // evicts tag 81
    EXPECT_EQ(tracker.evicted_negative_template_count(), 1U);
    track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->negative_templates.size(), 2U);
    EXPECT_EQ(track->negative_templates[0].fingerprint.content_hash, 82U);
    EXPECT_EQ(track->negative_templates[1].fingerprint.content_hash, 83U);

    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(32, 0x84, 84U, 13)).ok());  // evicts tag 82
    EXPECT_EQ(tracker.evicted_negative_template_count(), 2U);
    track = tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->negative_templates.size(), 2U);
    EXPECT_EQ(track->negative_templates[0].fingerprint.content_hash, 83U);
    EXPECT_EQ(track->negative_templates[1].fingerprint.content_hash, 84U);

    // The appearance set and its counter stay untouched by negative traffic.
    ASSERT_EQ(track->templates.size(), 1U);
    EXPECT_EQ(track->templates[0].fingerprint, adoption.fingerprint);
    EXPECT_EQ(tracker.evicted_template_count(), 0U);
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(32, 3, 1, 0, 0));
    expect_byte_invariant(tracker);
}

TEST(ObjectTrackerTest, AddNegativeTemplateByteFullPoolRejectsGrowthAndSwapsAtCapacity) {
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);

    // Byte-full pool below capacity: growth must fail explicitly.
    ObjectTrackerOptions grow_options;
    grow_options.template_thumb_side = 8;
    grow_options.pool_budget_bytes = expected_track_bytes(8, 1, 1, 0, 0);
    ObjectTracker grow_tracker = make_tracker(grow_options);
    ASSERT_TRUE(grow_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    const PoolSnapshot before = snapshot_of(grow_tracker);
    const TargetTrack resident = *grow_tracker.find_track(1U);
    const auto rejected = grow_tracker.add_negative_template(1U, synthetic_template(8, 0x91, 91U));
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    expect_pool_untouched(before, grow_tracker);
    expect_tracks_equal(*grow_tracker.find_track(1U), resident);
    expect_tally_equal(EvictionTally{}, grow_tracker);

    // Byte-full pool at capacity: the drop-oldest swap is byte-neutral.
    ObjectTrackerOptions swap_options;
    swap_options.template_thumb_side = 8;
    swap_options.max_negative_templates = 2;
    swap_options.pool_budget_bytes = expected_track_bytes(8, 3, 1, 0, 0);
    ObjectTracker swap_tracker = make_tracker(swap_options);
    ASSERT_TRUE(swap_tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(swap_tracker.add_negative_template(1U, synthetic_template(8, 0x92, 92U)).ok());
    EXPECT_EQ(swap_tracker.byte_size(), expected_track_bytes(8, 2, 1, 0, 0));
    ASSERT_TRUE(swap_tracker.add_negative_template(1U, synthetic_template(8, 0x93, 93U)).ok());
    ASSERT_EQ(swap_tracker.byte_size(), swap_tracker.options().pool_budget_bytes);
    ASSERT_TRUE(swap_tracker.add_negative_template(1U, synthetic_template(8, 0x94, 94U)).ok());
    EXPECT_EQ(swap_tracker.evicted_negative_template_count(), 1U);
    EXPECT_EQ(swap_tracker.byte_size(), swap_tracker.options().pool_budget_bytes);
    const TargetTrack* track = swap_tracker.find_track(1U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->negative_templates.size(), 2U);
    EXPECT_EQ(track->negative_templates[0].fingerprint.content_hash, 93U);
    EXPECT_EQ(track->negative_templates[1].fingerprint.content_hash, 94U);
    EXPECT_EQ(track->position_history.size(), 1U);
    EXPECT_EQ(track->position_history[0].frame_sequence, 0U);
    expect_byte_invariant(swap_tracker);
}

// --- layout generation and grouping --------------------------------------------------------

TEST(ObjectTrackerTest, AdvanceLayoutGenerationStampsAdoptionAndObservationsAcrossBumps) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_EQ(tracker.layout_generation(), 0U);

    const auto first = tracker.advance_layout_generation();
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_EQ(first.value(), 1U);
    EXPECT_EQ(tracker.layout_generation(), first.value());
    const auto second = tracker.advance_layout_generation();
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value(), 2U);
    EXPECT_EQ(tracker.layout_generation(), second.value());

    // Adoption after the bumps stamps the new track with the current generation.
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{0.0F, 0.0F, 4.0F, 4.0F}), view, 5).ok());
    const TargetTrack* second_track = tracker.find_track(2U);
    ASSERT_NE(second_track, nullptr);
    EXPECT_EQ(second_track->layout_generation, 2U);
    ASSERT_EQ(second_track->position_history.size(), 1U);
    EXPECT_EQ(second_track->position_history[0].layout_generation, 2U);
    ASSERT_EQ(second_track->templates.size(), 1U);
    EXPECT_EQ(second_track->templates[0].layout_generation, 2U);

    // Later observations carry the new stamp; existing history and the bumped
    // track's own generation field stay put.
    const RectF moved{1.0F, 1.0F, 2.0F, 2.0F};
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 6).ok());
    const TargetTrack* first_track = tracker.find_track(1U);
    ASSERT_NE(first_track, nullptr);
    EXPECT_EQ(first_track->layout_generation, 0U);
    ASSERT_EQ(first_track->position_history.size(), 2U);
    EXPECT_EQ(first_track->position_history[0].layout_generation, 0U);
    EXPECT_EQ(first_track->position_history[1].layout_generation, 2U);

    // Grouping attributes each entry to its own generation; the skipped
    // generation 1 owns nothing on either track.
    EXPECT_EQ(tracker.observations_in_generation(1U, 0).size(), 1U);
    EXPECT_TRUE(tracker.observations_in_generation(1U, 1).empty());
    EXPECT_EQ(tracker.observations_in_generation(1U, 2).size(), 1U);
    EXPECT_TRUE(tracker.observations_in_generation(2U, 0).empty());
    EXPECT_TRUE(tracker.observations_in_generation(2U, 1).empty());
    EXPECT_EQ(tracker.observations_in_generation(2U, 2).size(), 1U);
    expect_byte_invariant(tracker);
    expect_tally_equal(EvictionTally{}, tracker);
}

TEST(ObjectTrackerTest, ObservationsInGenerationFiltersOrdersAndHandlesMissing) {
    ObjectTracker tracker = make_tracker();
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{2.0F, 2.0F, 4.0F, 4.0F}), view, 1).ok());
    const RectF moved{1.0F, 1.0F, 2.0F, 2.0F};
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 2).ok());
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 3).ok());
    ASSERT_TRUE(tracker.advance_layout_generation().ok());
    ASSERT_TRUE(tracker.record_observation(2U, moved, 0.5F, 4).ok());

    const std::vector<TrackObservation> first_gen0 = tracker.observations_in_generation(1U, 0);
    ASSERT_EQ(first_gen0.size(), 3U);  // adoption + two records, oldest first
    EXPECT_EQ(first_gen0[0].frame_sequence, 0U);
    EXPECT_EQ(first_gen0[0].bounds, (RectF{0.0F, 0.0F, 8.0F, 8.0F}));
    EXPECT_EQ(first_gen0[1].frame_sequence, 2U);
    EXPECT_EQ(first_gen0[2].frame_sequence, 3U);
    for (const TrackObservation& entry : first_gen0) {
        EXPECT_EQ(entry.layout_generation, 0U);
    }

    // Track 2 owns its adoption (generation 0) and one record (generation 1);
    // the groups never mix tracks.
    EXPECT_EQ(tracker.observations_in_generation(2U, 0).size(), 1U);
    const std::vector<TrackObservation> second_gen1 = tracker.observations_in_generation(2U, 1);
    ASSERT_EQ(second_gen1.size(), 1U);
    EXPECT_EQ(second_gen1[0].frame_sequence, 4U);

    // Unknown tracks and unmatched generations yield empty results, and
    // termination releases the history the query reads.
    EXPECT_TRUE(tracker.observations_in_generation(999U, 0).empty());
    EXPECT_TRUE(tracker.observations_in_generation(1U, 77U).empty());
    ASSERT_TRUE(tracker.terminate(1U, 5).ok());
    EXPECT_TRUE(tracker.observations_in_generation(1U, 0).empty());
    EXPECT_EQ(tracker.observations_in_generation(2U, 0).size(), 1U);
}

TEST(ObjectTrackerTest, ResetRestartsGenerationCountersAndStampingFromZero) {
    ObjectTrackerOptions options;
    options.max_targets = 2;
    options.max_position_history = 2;
    options.max_templates = 2;
    options.max_negative_templates = 2;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 0).ok());
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{2.0F, 2.0F, 4.0F, 4.0F}), view, 1).ok());
    const RectF moved{1.0F, 1.0F, 2.0F, 2.0F};
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 2).ok());
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 3).ok());  // observation eviction
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(8, 0xA1, 161U)).ok());
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(8, 0xA2, 162U)).ok());  // template eviction
    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(8, 0xA3, 163U)).ok());
    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(8, 0xA4, 164U)).ok());
    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(8, 0xA5, 165U)).ok());  // negative eviction
    ASSERT_TRUE(tracker.advance_layout_generation().ok());
    ASSERT_TRUE(tracker.terminate(1U, 4).ok());
    const auto adopted = tracker.adopt_track(make_region(3U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 5);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{1U}));  // terminated archive first
    expect_tally_equal(EvictionTally{1, 1, 1, 1}, tracker);
    expect_byte_invariant(tracker);

    tracker.reset();
    expect_tally_equal(EvictionTally{}, tracker);
    EXPECT_EQ(tracker.byte_size(), 0);
    EXPECT_EQ(tracker.track_count(), 0U);
    EXPECT_EQ(tracker.layout_generation(), 0U);
    EXPECT_TRUE(tracker.track_ids().empty());
    EXPECT_EQ(tracker.find_track(3U), nullptr);

    // Stamping and the generation counter restart from zero.
    ASSERT_TRUE(tracker.adopt_track(make_region(4U, RectF{0.0F, 0.0F, 8.0F, 8.0F}), view, 6).ok());
    const TargetTrack* track = tracker.find_track(4U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->position_history.size(), 1U);
    EXPECT_EQ(track->position_history[0].layout_generation, 0U);
    EXPECT_EQ(tracker.observations_in_generation(4U, 0).size(), 1U);
    const auto bumped = tracker.advance_layout_generation();
    ASSERT_TRUE(bumped.ok()) << bumped.status().message();
    EXPECT_EQ(bumped.value(), 1U);
    ASSERT_TRUE(tracker.record_observation(4U, moved, 0.5F, 7).ok());
    EXPECT_EQ(tracker.observations_in_generation(4U, 1).size(), 1U);
    expect_byte_invariant(tracker);
}

// --- determinism and byte accounting --------------------------------------------------------

/// One scripted primitive sequence, successes and rejected calls in fixed
/// order, on a fresh tracker.
struct PrimitiveRun {
    bool ok = false;
    ObjectTracker tracker;
};

PrimitiveRun run_primitive_sequence(const ObjectTrackerOptions& options, const ImageView& view) {
    PrimitiveRun run;
    run.tracker = make_tracker(options);
    const RectF base{0.0F, 0.0F, 8.0F, 8.0F};
    const RectF moved{1.0F, 1.0F, 3.0F, 3.0F};
    const RectF empty{1.0F, 1.0F, 0.0F, 2.0F};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bool ok = true;

    const auto must_ok = [&ok](const char* what, const auto& result) {
        if (!result.ok()) {
            ADD_FAILURE() << what << ": " << result.status().message();
            ok = false;
        }
    };
    const auto must_fail = [](const char* what, const auto& result) {
        if (result.ok()) {
            ADD_FAILURE() << what << " unexpectedly succeeded";
        }
    };

    must_ok("adopt 1", run.tracker.adopt_track(make_region(1U, base, 0.8F, "a", "b"), view, 0));
    must_ok("record 1a", run.tracker.record_observation(1U, moved, 1.7F, 2));
    must_fail("record 1 empty", run.tracker.record_observation(1U, empty, 0.5F, 3));
    must_fail("record 1 nan", run.tracker.record_observation(1U, RectF{nan, 0.0F, 2.0F, 2.0F}, 0.5F, 3));
    must_fail("record unknown", run.tracker.record_observation(999U, moved, 0.5F, 3));
    must_ok("record 1b", run.tracker.record_observation(1U, moved, 0.2F, 4));  // evicts the adoption entry
    must_ok("template a", run.tracker.add_template(1U, synthetic_template(8, 0xB1, 171U, 4)));
    TrackTemplate bad = synthetic_template(8, 0xB2, 172U);
    bad.fingerprint.thumb_width = 16;
    must_fail("template bad", run.tracker.add_template(1U, bad));
    must_ok("template b", run.tracker.add_template(1U, synthetic_template(8, 0xB3, 173U, 5)));  // evicts a
    must_ok("negative a", run.tracker.add_negative_template(1U, synthetic_template(8, 0xB4, 174U)));
    must_ok("negative b", run.tracker.add_negative_template(1U, synthetic_template(8, 0xB5, 175U)));
    must_ok("negative c", run.tracker.add_negative_template(1U, synthetic_template(8, 0xB6, 176U)));  // evicts a
    must_ok("bump", run.tracker.advance_layout_generation());
    must_ok("adopt 2", run.tracker.adopt_track(make_region(2U, base, 0.8F, "a", "b"), view, 6));
    must_ok("record 2", run.tracker.record_observation(2U, moved, 0.4F, 7));
    must_ok("terminate 2", run.tracker.terminate(2U, 8));
    must_fail("record terminated", run.tracker.record_observation(2U, moved, 0.5F, 9));
    must_fail("template terminated", run.tracker.add_template(2U, synthetic_template(8, 0xB7, 177U)));
    const auto adopted = run.tracker.adopt_track(make_region(3U, base, 0.8F, "a", "b"), view, 10);
    must_ok("adopt 3", adopted);
    if (adopted.ok()) {
        EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{2U}));  // terminated archive first
    }
    run.ok = ok;
    return run;
}

void expect_primitive_runs_identical(const PrimitiveRun& first, const PrimitiveRun& second) {
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(first.tracker.track_ids(), second.tracker.track_ids());
    EXPECT_EQ(first.tracker.byte_size(), second.tracker.byte_size());
    EXPECT_EQ(first.tracker.layout_generation(), second.tracker.layout_generation());
    expect_tally_equal(tally_of(first.tracker), second.tracker);
    expect_track_maps_identical(first.tracker, second.tracker);  // includes exact fingerprint bytes
    for (const uint64_t id : first.tracker.track_ids()) {
        for (const uint32_t generation : {0U, 1U}) {
            expect_observations_equal(first.tracker.observations_in_generation(id, generation),
                                      second.tracker.observations_in_generation(id, generation));
        }
    }
}

TEST(ObjectTrackerTest, IdenticalPrimitiveSequencesWithFailuresProduceIdenticalPools) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_targets = 2;
    options.max_position_history = 2;
    options.max_templates = 2;
    options.max_negative_templates = 2;

    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);

    const PrimitiveRun first = run_primitive_sequence(options, view);
    const PrimitiveRun second = run_primitive_sequence(options, view);
    expect_primitive_runs_identical(first, second);

    // Every eviction counter must have fired for the comparison to be
    // meaningful: track 2 evicted, two history swaps, one template and one
    // negative-template drop.
    expect_tally_equal(EvictionTally{1, 1, 1, 1}, first.tracker);
    EXPECT_EQ(first.tracker.track_ids(), (std::vector<uint64_t>{1U, 3U}));
    // Track 1 keeps 2 appearance + 2 negative templates and 2 observations;
    // track 3 is a fresh adoption.
    EXPECT_EQ(first.tracker.byte_size(), expected_track_bytes(8, 4, 2, 1, 1) + expected_track_bytes(8, 1, 1, 1, 1));
    expect_byte_invariant(first.tracker);
    expect_byte_invariant(second.tracker);
}

TEST(ObjectTrackerTest, ByteSizeMatchesFormulaAcrossTemplatesNegativesAndGenerations) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 16;
    options.max_position_history = 6;
    ObjectTracker tracker = make_tracker(options);
    const TestImage image = make_rgba_image(8, 8);
    const ImageView view = view_of(image);
    ASSERT_TRUE(tracker.adopt_track(make_region(1U, RectF{0.0F, 0.0F, 8.0F, 8.0F}, 0.5F, "ab", "cd"), view, 0).ok());
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(16, 1, 1, 2, 2));
    expect_byte_invariant(tracker);

    const RectF moved{1.0F, 1.0F, 2.0F, 2.0F};
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(16, 0xC1, 181U)).ok());
    expect_byte_invariant(tracker);
    ASSERT_TRUE(tracker.add_template(1U, synthetic_template(16, 0xC2, 182U)).ok());
    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(16, 0xC3, 183U)).ok());
    ASSERT_TRUE(tracker.add_negative_template(1U, synthetic_template(16, 0xC4, 184U)).ok());
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(16, 5, 1, 2, 2));  // 5 templates incl. adoption + negatives

    for (const uint64_t frame : {1U, 2U, 3U, 4U, 5U}) {
        ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, frame).ok());
        expect_byte_invariant(tracker);
    }
    ASSERT_TRUE(tracker.advance_layout_generation().ok());
    // The 7th entry overflows: one generation-0 entry drops, bytes stay put.
    ASSERT_TRUE(tracker.record_observation(1U, moved, 0.5F, 6).ok());
    EXPECT_EQ(tracker.evicted_observation_count(), 1U);
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(16, 5, 6, 2, 2));  // the swap is byte-neutral
    EXPECT_EQ(tracker.observations_in_generation(1U, 0).size(), 5U);
    EXPECT_EQ(tracker.observations_in_generation(1U, 1).size(), 1U);
    expect_byte_invariant(tracker);

    // Rejected calls interleaved between successes must not perturb bytes.
    TrackTemplate bad_bytes = synthetic_template(16, 0xC5, 185U);
    bad_bytes.fingerprint.thumbnail_gray.push_back(std::byte{0x01});
    EXPECT_EQ(tracker.add_template(1U, bad_bytes).status().code(), ErrorCode::kInvalidArgument);
    const RectF empty{1.0F, 1.0F, 0.0F, 2.0F};
    EXPECT_EQ(tracker.record_observation(1U, empty, 0.5F, 7).status().code(), ErrorCode::kInvalidArgument);
    expect_byte_invariant(tracker);

    // A second track with different accounting; the pool total sums both.
    ASSERT_TRUE(tracker.adopt_track(make_region(2U, RectF{2.0F, 2.0F, 4.0F, 4.0F}, 0.4F, "z"), view, 8).ok());
    EXPECT_EQ(tracker.byte_size(), expected_track_bytes(16, 5, 6, 2, 2) + expected_track_bytes(16, 1, 1, 1, 0));
    expect_byte_invariant(tracker);
}

}  // namespace
