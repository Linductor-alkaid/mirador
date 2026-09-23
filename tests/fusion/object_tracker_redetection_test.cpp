// M7-08 cascade redetection primitives and identity review: independent
// verification suite for `ObjectTracker::evaluate_redetection_gate`,
// `record_redetection_failure`, `record_redetection_recapture`,
// `record_redetection_association` and the bounded redetection log
// (object-tracking design section 7). Written against the frozen header
// contracts only:
//   * Trigger and throttle primitives (RULE-12): the gate is a pure const
//     classification — a kNone (static) frame NEVER triggers redetection
//     (the design section 8 zero-trigger negative-test anchor), the backoff
//     window holds with an exact remaining-wait readout, live tracks report
//     kInactive explicitly, and no pool state moves on any gate path.
//   * The frozen doubling backoff `min(base * 2^(n-1), max)` counted in the
//     caller's frame sequences (RULE-03, no wall clock), with the exact
//     `next_attempt_sequence` boundary (hold before it, trigger at it).
//   * Budget exhaustion as an explicit, visible kTerminated archive performed
//     by `record_redetection_failure` itself — never a silent pool clear,
//     never a gate verdict — with every error path leaving the pool
//     untouched (RULE-06/RULE-08).
//   * Identity review as pure reuse of the frozen M7-05/M7-06 entries: a real
//     `verify_track` on the kLost track, the confirming
//     `commit_track_evidence` (kLost -> kTracking, the state machine's edge)
//     and then the interruption event; the insufficient branch adopts a new
//     id through the normal fusion path and records the association (DEC-010).
//   * The bounded pool-wide log: oldest-first, overflow drops the oldest with
//     an explicit counter, ids and frame sequences only (RULE-10/DOD-06),
//     cleared by `reset` only, surviving track termination and eviction.
//   * Episode bookkeeping: the slot allocated by the first accounted failure,
//     closed by recapture bookkeeping / termination / eviction / reset, and
//     the stale-key rule that reads a slot of an older episode as the fresh
//     episode it is.
//   * Entry-only cancellation polls converting to kCancelled/kTimeout, twin
//     instance bitwise determinism, and the DOD-03 rotation/odd-size/stride
//     matrix over the one coordinate-consuming reuse (verify_track).
//
// Privacy (RULE-10/DOD-06) is structural here, as in the M7-05/06/07 suites:
// every new result type (decisions, records, events, associations) carries
// ids, sequences and enums only — the suite asserts every field of every
// record exhaustively, and the tracker has no logging, filesystem or network
// surface.

#include <mirador/object_tracker.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::AppearanceChannelOutcome;
using mirador::ChangeClassification;
using mirador::ErrorCode;
using mirador::EvidenceGrade;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::PositionScenario;
using mirador::RectF;
using mirador::RectI;
using mirador::RedetectionAssociation;
using mirador::RedetectionFailureRecord;
using mirador::RedetectionGateDecision;
using mirador::RedetectionGateVerdict;
using mirador::RedetectionRecord;
using mirador::RedetectionRecordKind;
using mirador::Rotation;
using mirador::TargetTrack;
using mirador::TrackInterruptionEvent;
using mirador::TrackPositionEvidence;
using mirador::TrackState;
using mirador::TrackVerification;
using mirador::VisualRegion;

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

/// Noise-like per-pixel pattern (uint32 hashing, deterministic, no UB): the
/// same content generator the M7-05/06/07 suites use, so the real
/// `verify_track` reuse below has healthy peak-sidelobe margins at the
/// frozen default thresholds.
uint8_t noise_pixel(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393U + static_cast<uint32_t>(y) * 668265263U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    return static_cast<uint8_t>((h ^ (h >> 16U)) % 256U);
}

GrayImage noise_image(int32_t width, int32_t height, int64_t stride_padding_bytes = 0) {
    GrayImage image = make_gray_image(width, height, std::byte{0}, stride_padding_bytes);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x)] =
                std::byte{noise_pixel(x, y)};
        }
    }
    return image;
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

VisualRegion make_region(uint64_t stable_id, RectF bounds, float confidence = 0.9F, std::string label = {}) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.anchor = PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    region.confidence = confidence;
    region.label = std::move(label);
    return region;
}

/// Suite defaults: the loss side uses `uncertain_frame_limit = 1` so a single
/// placeholder commit is the deterministic kLost entry (the loss path is the
/// frozen M7-06 state machine's; this suite only drives it).
ObjectTrackerOptions redetection_options() {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 1;
    return options;
}

ObjectTracker make_tracker(const ObjectTrackerOptions& options = redetection_options()) {
    auto created = ObjectTracker::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_tracker: " << created.status().message();
        return ObjectTracker{};
    }
    return created.take_value();
}

void adopt_or_fail(ObjectTracker& tracker, const ImageView& view, uint64_t id, const RectF& bounds,
                   uint64_t frame_sequence = 1, std::string label = {}) {
    const auto adopted = tracker.adopt_track(make_region(id, bounds, 0.9F, std::move(label)), view, frame_sequence);
    ASSERT_TRUE(adopted.ok()) << "adopt " << id << ": " << adopted.status().message();
}

/// Hand-built caller evidence, exactly the M7-06 suite's evidence-trust shape:
/// the commit never re-runs the scan, so channel outcomes are test inputs.
TrackVerification verification_of(uint64_t track_id, TrackState state_echo, AppearanceChannelOutcome appearance) {
    TrackVerification verification;
    verification.track_id = track_id;
    verification.state = state_echo;
    verification.verification_roi = RectI{0, 0, 1, 1};
    verification.appearance.outcome = appearance;
    verification.appearance.peak_ncc = 0.9;
    verification.appearance.peak_sidelobe_ratio = 10.0;
    verification.appearance.best_template_index = 0U;
    verification.appearance.best_offset_dx = 0;
    verification.appearance.best_offset_dy = 0;
    return verification;
}

TrackPositionEvidence position_of(bool inside_gate = true) {
    TrackPositionEvidence position;
    position.scenario = PositionScenario::kStationary;
    position.inside_gate = inside_gate;
    return position;
}

/// Drives an adopted track to kLost with one placeholder commit and returns
/// the frame sequence stamped as the kLost entry (the state slot's episode
/// key, the frozen M7-06 loss bookkeeping).
uint64_t lose_track(ObjectTracker& tracker, uint64_t id, const ImageView& view, uint64_t lost_at_sequence) {
    const auto lost =
        tracker.commit_track_evidence(id, verification_of(id, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(), std::nullopt, view, lost_at_sequence);
    EXPECT_TRUE(lost.ok()) << "lose " << id << ": " << lost.status().message();
    EXPECT_EQ(lost.value().state, TrackState::kLost);
    return lost_at_sequence;
}

/// Observable pool state for purity/atomicity comparisons, extended with the
/// M7-08 counters and the redetection log.
struct PoolSnapshot {
    size_t track_count = 0;
    int64_t used_bytes = 0;
    uint64_t evicted_count = 0;
    uint64_t evicted_redetection_records = 0;
    uint32_t layout_generation = 0;
    std::vector<uint64_t> ids;
    std::vector<RedetectionRecord> records;
};

PoolSnapshot snapshot_of(const ObjectTracker& tracker) {
    PoolSnapshot snapshot;
    snapshot.track_count = tracker.track_count();
    snapshot.used_bytes = tracker.byte_size();
    snapshot.evicted_count = tracker.evicted_track_count();
    snapshot.evicted_redetection_records = tracker.evicted_redetection_record_count();
    snapshot.layout_generation = tracker.layout_generation();
    snapshot.ids = tracker.track_ids();
    snapshot.records = tracker.redetection_records();
    return snapshot;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_records_equal(const std::vector<RedetectionRecord>& lhs, const std::vector<RedetectionRecord>& rhs);

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_pool_untouched(const PoolSnapshot& before, const ObjectTracker& tracker) {
    EXPECT_EQ(tracker.track_count(), before.track_count);
    EXPECT_EQ(tracker.byte_size(), before.used_bytes);
    EXPECT_EQ(tracker.evicted_track_count(), before.evicted_count);
    EXPECT_EQ(tracker.evicted_redetection_record_count(), before.evicted_redetection_records);
    EXPECT_EQ(tracker.layout_generation(), before.layout_generation);
    EXPECT_EQ(tracker.track_ids(), before.ids);
    EXPECT_NO_FATAL_FAILURE(expect_records_equal(tracker.redetection_records(), before.records));
}

void expect_gate_equal(const RedetectionGateDecision& lhs, const RedetectionGateDecision& rhs) {
    EXPECT_EQ(lhs.track_id, rhs.track_id);
    EXPECT_EQ(lhs.state, rhs.state);
    EXPECT_EQ(lhs.verdict, rhs.verdict);
    EXPECT_EQ(lhs.attempts, rhs.attempts);
    EXPECT_EQ(lhs.wait_frames, rhs.wait_frames);
}

void expect_failure_equal(const RedetectionFailureRecord& lhs, const RedetectionFailureRecord& rhs) {
    EXPECT_EQ(lhs.track_id, rhs.track_id);
    EXPECT_EQ(lhs.state, rhs.state);
    EXPECT_EQ(lhs.attempts, rhs.attempts);
    EXPECT_EQ(lhs.backoff_frames, rhs.backoff_frames);
    EXPECT_EQ(lhs.next_attempt_sequence, rhs.next_attempt_sequence);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_records_equal(const std::vector<RedetectionRecord>& lhs, const std::vector<RedetectionRecord>& rhs) {
    ASSERT_EQ(lhs.size(), rhs.size());
    for (size_t i = 0; i < lhs.size(); ++i) {
        EXPECT_EQ(lhs[i].kind, rhs[i].kind) << "record " << i;
        EXPECT_EQ(lhs[i].sequence, rhs[i].sequence) << "record " << i;
        EXPECT_EQ(lhs[i].track_id, rhs[i].track_id) << "record " << i;
        EXPECT_EQ(lhs[i].related_track_id, rhs[i].related_track_id) << "record " << i;
        EXPECT_EQ(lhs[i].lost_sequence, rhs[i].lost_sequence) << "record " << i;
        EXPECT_EQ(lhs[i].attempts, rhs[i].attempts) << "record " << i;
    }
}

/// The frozen backoff formula `min(base * 2^(n-1), max)` recomputed from the
/// contract, for cross-checking the implementation.
int32_t reference_backoff(int32_t n, int32_t base, int32_t max_wait) {
    int64_t wait = base;
    for (int32_t i = 1; i < n; ++i) {
        wait = std::min(wait * 2, static_cast<int64_t>(max_wait));
    }
    return static_cast<int32_t>(wait);
}

// --- gate: the static-frame zero-trigger negative anchor (design section 8) ---

/// A kNone classification holds redetection for a kLost track on every path:
/// before any accounted failure, inside a scheduled backoff window and long
/// after it. The gate is a pure const read — the pool (including the log and
/// the eviction counters) does not move.
TEST(ObjectTrackerRedetectionTest, StaticFrameNeverTriggersRedetection) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    // Before any accounted failure.
    const auto before = tracker.evaluate_redetection_gate(7U, ChangeClassification::kNone, 3);
    ASSERT_TRUE(before.ok()) << before.status().message();
    EXPECT_EQ(before.value().verdict, RedetectionGateVerdict::kHoldStaticFrame);
    EXPECT_EQ(before.value().state, TrackState::kLost);
    EXPECT_EQ(before.value().attempts, 0U);
    EXPECT_EQ(before.value().wait_frames, 0U);

    // One accounted failure schedules a backoff window [4, inf); the static
    // frame still holds INSIDE and BEYOND the window — the classification
    // dominates the backoff state.
    const auto failure = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(failure.ok()) << failure.status().message();
    ASSERT_EQ(failure.value().next_attempt_sequence, 4U);
    for (const uint64_t sequence : {3U, 4U, 5U, 1000U}) {
        const auto held = tracker.evaluate_redetection_gate(7U, ChangeClassification::kNone, sequence);
        ASSERT_TRUE(held.ok()) << "sequence " << sequence;
        EXPECT_EQ(held.value().verdict, RedetectionGateVerdict::kHoldStaticFrame) << "sequence " << sequence;
        EXPECT_EQ(held.value().wait_frames, 0U) << "sequence " << sequence;
    }

    // Purity: the const query moved nothing (pool counters, log, records).
    const PoolSnapshot before_snapshot = snapshot_of(tracker);
    const auto again = tracker.evaluate_redetection_gate(7U, ChangeClassification::kNone, 1000);
    ASSERT_TRUE(again.ok());
    EXPECT_NO_FATAL_FAILURE(expect_pool_untouched(before_snapshot, tracker));
    EXPECT_EQ(again.value().verdict, RedetectionGateVerdict::kHoldStaticFrame);
}

// --- gate: the full verdict matrix and its explicitness -------------------------

/// kPartial/kGlobal frames are eligible: kTrigger with no episode, kHoldBackoff
/// with an exact remaining-wait inside the scheduled window, kTrigger again
/// exactly AT the scheduled sequence (the `frame_sequence < next_attempt`
/// boundary). Live tracks report kInactive explicitly with the state echoed —
/// never a silent skip.
TEST(ObjectTrackerRedetectionTest, GateVerdictMatrixIsExplicit) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    // Live tracks: explicit kInactive with the state echoed, for both eligible
    // classifications.
    const auto tracking_partial = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 2);
    ASSERT_TRUE(tracking_partial.ok());
    EXPECT_EQ(tracking_partial.value().verdict, RedetectionGateVerdict::kInactive);
    EXPECT_EQ(tracking_partial.value().state, TrackState::kTracking);
    EXPECT_EQ(tracking_partial.value().attempts, 0U);
    EXPECT_EQ(tracking_partial.value().wait_frames, 0U);

    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    // Eligible classification, fresh episode: kTrigger with zero attempts.
    const auto partial = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    ASSERT_TRUE(partial.ok());
    EXPECT_EQ(partial.value().verdict, RedetectionGateVerdict::kTrigger);
    EXPECT_EQ(partial.value().state, TrackState::kLost);
    EXPECT_EQ(partial.value().attempts, 0U);
    EXPECT_EQ(partial.value().wait_frames, 0U);

    const auto global = tracker.evaluate_redetection_gate(7U, ChangeClassification::kGlobal, 3);
    ASSERT_TRUE(global.ok());
    EXPECT_EQ(global.value().verdict, RedetectionGateVerdict::kTrigger);

    // One failure at sequence 3 with the default base 1 schedules next = 4:
    // hold at 3 with wait 1, trigger exactly at 4.
    const auto failure = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(failure.ok());
    ASSERT_EQ(failure.value().next_attempt_sequence, 4U);

    const auto holding = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    ASSERT_TRUE(holding.ok());
    EXPECT_EQ(holding.value().verdict, RedetectionGateVerdict::kHoldBackoff);
    EXPECT_EQ(holding.value().attempts, 1U);
    EXPECT_EQ(holding.value().wait_frames, 1U);

    const auto boundary = tracker.evaluate_redetection_gate(7U, ChangeClassification::kGlobal, 4);
    ASSERT_TRUE(boundary.ok());
    EXPECT_EQ(boundary.value().verdict, RedetectionGateVerdict::kTrigger);
    EXPECT_EQ(boundary.value().attempts, 1U);
    EXPECT_EQ(boundary.value().wait_frames, 0U);

    // An kUncertain track is also explicitly inactive (degradation is the
    // state machine's business, not the gate's).
    ObjectTrackerOptions limit_two = redetection_options();
    limit_two.uncertain_frame_limit = 2;
    ObjectTracker two = make_tracker(limit_two);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(two, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    const auto first =
        two.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                  position_of(), std::nullopt, view, 2);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().state, TrackState::kUncertain);
    const auto uncertain = two.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    ASSERT_TRUE(uncertain.ok());
    EXPECT_EQ(uncertain.value().verdict, RedetectionGateVerdict::kInactive);
    EXPECT_EQ(uncertain.value().state, TrackState::kUncertain);
}

/// Gate validation errors: unknown track id, terminated track (identity
/// closed — the per-track-entry rule of `verify_track`) and an unknown
/// classification value are explicit kInvalidArgument. After `reset` the old
/// identity is gone (stale-id negative).
TEST(ObjectTrackerRedetectionTest, GateRejectsUnknownTerminatedAndInvalidClassification) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);

    const auto unknown = tracker.evaluate_redetection_gate(99U, ChangeClassification::kPartial, 3);
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): undefined-value contract test
    const auto bad_classification = tracker.evaluate_redetection_gate(7U, static_cast<ChangeClassification>(77), 3);
    EXPECT_EQ(bad_classification.status().code(), ErrorCode::kInvalidArgument);

    ASSERT_TRUE(tracker.terminate(7U, 2).ok());
    const auto terminated = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    EXPECT_EQ(terminated.status().code(), ErrorCode::kInvalidArgument);

    tracker.reset();
    const auto stale = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    EXPECT_EQ(stale.status().code(), ErrorCode::kInvalidArgument);
}

// --- backoff: the frozen doubling sequence, frame-counted --------------------

/// The default configuration (base 1, max 60, 8 attempts) produces the exact
/// doubling waits 1, 2, 4, 8, 16, 32, 60 (capped), and the 8-th consecutive
/// failure is the explicit kTerminated exhaustion — checked in depth by the
/// exhaustion test; here every `next_attempt_sequence` is exactly the recorded
/// sequence plus the wait, recomputed from the frozen formula.
TEST(ObjectTrackerRedetectionTest, BackoffDoublingFollowsFrozenFormula) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    uint64_t sequence = 2;
    for (int32_t attempt = 1; attempt <= 7; ++attempt) {
        const int32_t expected_wait = reference_backoff(attempt, 1, 60);
        const auto failure = tracker.record_redetection_failure(7U, sequence);
        ASSERT_TRUE(failure.ok()) << "attempt " << attempt;
        EXPECT_EQ(failure.value().track_id, 7U);
        EXPECT_EQ(failure.value().state, TrackState::kLost);
        EXPECT_EQ(failure.value().attempts, static_cast<uint32_t>(attempt));
        EXPECT_EQ(failure.value().backoff_frames, expected_wait) << "attempt " << attempt;
        EXPECT_EQ(failure.value().next_attempt_sequence, sequence + static_cast<uint64_t>(expected_wait))
            << "attempt " << attempt;
        sequence = failure.value().next_attempt_sequence;
    }
    // The capped 7-th wait is exactly the ceiling 60.
    EXPECT_EQ(reference_backoff(7, 1, 60), 60);
}

/// Custom base and ceiling: base 3 / max 100 doubles 3, 6, 12, 24, 48, 96,
/// 100, 100...; base == ceiling holds every wait at the ceiling; the waits
/// count the caller's frame sequences — the gate holds until the scheduled
/// sequence and triggers at it regardless of how much "time" passed.
TEST(ObjectTrackerRedetectionTest, BackoffCustomBaseAndCeilingFrameCounted) {
    ObjectTrackerOptions options = redetection_options();
    options.redetect_backoff_base_frames = 3;
    options.redetect_backoff_max_frames = 100;
    options.redetect_max_attempts = 16;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    const std::array<const int32_t, 8> expected_waits{3, 6, 12, 24, 48, 96, 100, 100};
    uint64_t sequence = 2;
    for (int32_t attempt = 1; attempt <= 8; ++attempt) {
        const auto failure = tracker.record_redetection_failure(7U, sequence);
        ASSERT_TRUE(failure.ok()) << "attempt " << attempt;
        EXPECT_EQ(failure.value().backoff_frames, expected_waits[attempt - 1]) << "attempt " << attempt;
        EXPECT_EQ(failure.value().backoff_frames, reference_backoff(attempt, 3, 100));
        sequence = failure.value().next_attempt_sequence;
    }

    // base == ceiling: every wait is the ceiling.
    ObjectTrackerOptions flat = redetection_options();
    flat.redetect_backoff_base_frames = 60;
    flat.redetect_backoff_max_frames = 60;
    flat.redetect_max_attempts = 16;
    ObjectTracker flat_tracker = make_tracker(flat);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(flat_tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(flat_tracker, 7U, view, 2), 2U);
    for (int32_t attempt = 1; attempt <= 3; ++attempt) {
        const auto failure = flat_tracker.record_redetection_failure(7U, 10U * static_cast<uint64_t>(attempt));
        ASSERT_TRUE(failure.ok());
        EXPECT_EQ(failure.value().backoff_frames, 60) << "attempt " << attempt;
    }

    // Frame counting, not wall clock: the last failure (attempt 8, recorded at
    // 291) scheduled next = 391 — the gate holds at 390, triggers at exactly
    // 391 — an equal or larger sequence number is all that matters (no clock
    // exists to consult, RULE-03).
    const auto before_boundary = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 390);
    ASSERT_TRUE(before_boundary.ok());
    EXPECT_EQ(before_boundary.value().verdict, RedetectionGateVerdict::kHoldBackoff);
    EXPECT_EQ(before_boundary.value().wait_frames, 1U);
    const auto at_boundary = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 391);
    ASSERT_TRUE(at_boundary.ok());
    EXPECT_EQ(at_boundary.value().verdict, RedetectionGateVerdict::kTrigger);
}

// --- budget exhaustion: the explicit, visible failure -------------------------

/// Reaching `redetect_max_attempts` consecutive failures makes
/// `record_redetection_failure` itself perform the kLost -> kTerminated
/// transition with terminate's archive semantics: the record reports
/// kTerminated with zeroed backoff fields, the archive keeps the identity
/// record visible (bounds, semantics) while the evidence stores are released,
/// the terminated_sequence is stamped, and every later redetection entry on
/// the closed identity is an explicit kInvalidArgument — the exhausted end
/// state is visible, never a gate verdict and never a silent pool clear.
TEST(ObjectTrackerRedetectionTest, ExhaustionTerminatesExplicitlyAndVisibly) {
    ObjectTrackerOptions options = redetection_options();
    options.redetect_max_attempts = 3;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    const int64_t lost_bytes = tracker.byte_size();

    const auto first = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().state, TrackState::kLost);
    EXPECT_EQ(first.value().attempts, 1U);
    const auto second = tracker.record_redetection_failure(7U, 4);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().state, TrackState::kLost);
    EXPECT_EQ(second.value().attempts, 2U);

    const auto exhausted = tracker.record_redetection_failure(7U, 5);
    ASSERT_TRUE(exhausted.ok()) << exhausted.status().message();
    EXPECT_EQ(exhausted.value().track_id, 7U);
    EXPECT_EQ(exhausted.value().state, TrackState::kTerminated);
    EXPECT_EQ(exhausted.value().attempts, 3U);
    EXPECT_EQ(exhausted.value().backoff_frames, 0) << "a terminated identity schedules nothing";
    EXPECT_EQ(exhausted.value().next_attempt_sequence, 0U);

    // The archive is visible: identity closed, evidence stores released.
    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->state, TrackState::kTerminated);
    EXPECT_EQ(track->terminated_sequence, 5U);
    EXPECT_EQ(track->last_bounds, (RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    EXPECT_EQ(track->semantics.label, "icon");
    EXPECT_TRUE(track->templates.empty());
    EXPECT_TRUE(track->negative_templates.empty());
    EXPECT_TRUE(track->position_history.empty());
    // Byte accounting: the episode slot and the evidence bytes are released.
    EXPECT_LT(tracker.byte_size(), lost_bytes);

    // The closed identity rejects every redetection entry explicitly.
    const auto gated = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 6);
    EXPECT_EQ(gated.status().code(), ErrorCode::kInvalidArgument);
    const auto more = tracker.record_redetection_failure(7U, 6);
    EXPECT_EQ(more.status().code(), ErrorCode::kInvalidArgument);
    const auto recaptured = tracker.record_redetection_recapture(7U, 6, 2);
    EXPECT_EQ(recaptured.status().code(), ErrorCode::kInvalidArgument);
    // And no silent pool clear happened: the archive still holds the id.
    EXPECT_EQ(tracker.track_count(), 1U);
}

/// A budget of one attempt terminates on the FIRST accounted failure — the
/// lower boundary of `redetect_max_attempts` is the same explicit archive.
TEST(ObjectTrackerRedetectionTest, SingleAttemptBudgetTerminatesOnFirstFailure) {
    ObjectTrackerOptions options = redetection_options();
    options.redetect_max_attempts = 1;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    const auto exhausted = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(exhausted.ok());
    EXPECT_EQ(exhausted.value().state, TrackState::kTerminated);
    EXPECT_EQ(exhausted.value().attempts, 1U);
    EXPECT_EQ(exhausted.value().backoff_frames, 0);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTerminated);
}

// --- error paths leave the pool untouched (RULE-06/RULE-08) -------------------

/// A redetection-episode slot allocation that does not fit the pool budget is
/// an explicit kBudgetExceeded with the pool completely untouched (the error
/// model of RULE-08): no slot, no byte movement, the gate still reports the
/// fresh episode, and a twin tracker with one more byte of budget runs the
/// identical sequence successfully — proving the failure was purely the
/// budget, not the accounting.
TEST(ObjectTrackerRedetectionTest, SlotAllocationBudgetExceededLeavesPoolUntouched) {
    // Probe the deterministic byte footprint of adopt + lose on a generous
    // budget first.
    ObjectTracker probe = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(probe, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(probe, 7U, view, 2), 2U);
    const int64_t lost_bytes = probe.byte_size();

    // One byte short of the slot allocation.
    ObjectTrackerOptions tight = redetection_options();
    tight.pool_budget_bytes = lost_bytes + ObjectTracker::kRedetectSlotOverheadBytes - 1;
    ASSERT_GT(tight.pool_budget_bytes, lost_bytes);
    ObjectTracker tracker = make_tracker(tight);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    ASSERT_EQ(tracker.byte_size(), lost_bytes);

    const PoolSnapshot before = snapshot_of(tracker);
    const auto failure = tracker.record_redetection_failure(7U, 3);
    ASSERT_FALSE(failure.ok());
    EXPECT_EQ(failure.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));

    // The gate still reads the fresh episode (no slot was allocated).
    const auto gate = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    ASSERT_TRUE(gate.ok());
    EXPECT_EQ(gate.value().verdict, RedetectionGateVerdict::kTrigger);
    EXPECT_EQ(gate.value().attempts, 0U);

    // The twin with exactly the slot bytes runs the identical call.
    ObjectTrackerOptions fits = redetection_options();
    fits.pool_budget_bytes = lost_bytes + ObjectTracker::kRedetectSlotOverheadBytes;
    ObjectTracker twin = make_tracker(fits);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(twin, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(twin, 7U, view, 2), 2U);
    const auto twin_failure = twin.record_redetection_failure(7U, 3);
    ASSERT_TRUE(twin_failure.ok()) << twin_failure.status().message();
    EXPECT_EQ(twin_failure.value().attempts, 1U);
    EXPECT_EQ(twin.byte_size(), lost_bytes + ObjectTracker::kRedetectSlotOverheadBytes);
}

/// The recapture bookkeeping is atomic: the planned log append is checked
/// before anything mutates, so a record that does not fit the pool budget
/// fails with kBudgetExceeded while BOTH the log and the episode slot stay
/// exactly as they were (the slot's bytes are still held), per the frozen
/// "on any error neither the log nor the episode slot changes".
TEST(ObjectTrackerRedetectionTest, RecaptureLogAppendBudgetExceededKeepsEpisodeOpen) {
    // Probe adopt + lose + one failure: template capture is off (max_templates
    // == 1) so the confirming commit below stays byte-neutral.
    ObjectTrackerOptions probe_options = redetection_options();
    probe_options.max_templates = 1;
    ObjectTracker probe = make_tracker(probe_options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(probe, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(probe, 7U, view, 2), 2U);
    const int64_t lost_bytes = probe.byte_size();
    const auto probe_failure = probe.record_redetection_failure(7U, 3);
    ASSERT_TRUE(probe_failure.ok());
    const int64_t failed_bytes = probe.byte_size();
    ASSERT_EQ(failed_bytes, lost_bytes + ObjectTracker::kRedetectSlotOverheadBytes);

    // Budget exactly covers the episode slot but leaves no room for one log
    // record.
    ObjectTrackerOptions tight = probe_options;
    tight.pool_budget_bytes = failed_bytes;
    ObjectTracker tracker = make_tracker(tight);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    const auto failure = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(failure.ok()) << failure.status().message();
    ASSERT_TRUE(tracker
                    .commit_track_evidence(7U,
                                           verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                           position_of(), std::nullopt, view, 4)
                    .ok());
    ASSERT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);

    const PoolSnapshot before = snapshot_of(tracker);
    const auto recapture = tracker.record_redetection_recapture(7U, 4, 2);
    ASSERT_FALSE(recapture.ok());
    EXPECT_EQ(recapture.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));
    EXPECT_TRUE(tracker.redetection_records().empty()) << "the log did not move";

    // The episode slot is still open — observable through the accounting: the
    // +`kRedetectSlotOverheadBytes` the failure allocated are still held (a
    // second failure cannot be recorded on the now-live track; the slot's
    // fate after a later re-loss is the stale-key rule's business).
    EXPECT_EQ(tracker.byte_size(), before.used_bytes) << "the slot was not released";
}

// --- recapture: the entry's own validation matrix -----------------------------

/// `record_redetection_recapture` requires the confirming commit to have
/// happened (kTracking IS the recapture proof), a closed identity is rejected,
/// and the caller-supplied lost_sequence evidence must not exceed the
/// recapture sequence — the boundary lost_sequence == frame_sequence is
/// legal, and a first-attempt recapture legitimately records attempts == 0.
TEST(ObjectTrackerRedetectionTest, RecaptureEntryValidationMatrix) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    const auto unknown = tracker.record_redetection_recapture(99U, 2, 1);
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    // A kTracking track passes the state check by design — the kTracking
    // state IS the recapture proof (the evidence-trust boundary: the pool
    // cannot distinguish a recaptured track from a never-lost one). The
    // evidence ORDER it still validates: a loss dated after the recapture is
    // rejected (track 8 below is live/kTracking).
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}, 1));
    const auto reversed = tracker.record_redetection_recapture(8U, 4, 5);
    EXPECT_EQ(reversed.status().code(), ErrorCode::kInvalidArgument);

    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    const auto still_lost = tracker.record_redetection_recapture(7U, 3, 2);
    EXPECT_EQ(still_lost.status().code(), ErrorCode::kInvalidArgument) << "a kLost track has not been recaptured yet";

    // Recapture through the frozen state machine, then the boundary case
    // lost_sequence == recapture sequence, zero accounted attempts.
    ASSERT_TRUE(tracker
                    .commit_track_evidence(7U,
                                           verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                           position_of(), std::nullopt, view, 3)
                    .ok());
    const auto boundary = tracker.record_redetection_recapture(7U, 3, 3);
    ASSERT_TRUE(boundary.ok()) << boundary.status().message();
    EXPECT_EQ(boundary.value().track_id, 7U);
    EXPECT_EQ(boundary.value().lost_sequence, 3U);
    EXPECT_EQ(boundary.value().recapture_sequence, 3U);
    EXPECT_EQ(boundary.value().attempts, 0U) << "zero accounted failures is legitimate";

    // A terminated identity is closed.
    ObjectTracker terminated_tracker = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(terminated_tracker, view, 8U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_TRUE(terminated_tracker.terminate(8U, 2).ok());
    const auto closed = terminated_tracker.record_redetection_recapture(8U, 3, 2);
    EXPECT_EQ(closed.status().code(), ErrorCode::kInvalidArgument);
}

// --- episode lifecycle: closure, fresh episodes, the stale-key rule -----------

/// Recapture bookkeeping closes the episode: after recapture + a re-loss the
/// gate reads a fresh episode (attempts 0), and the next accounted failure
/// restarts the count at 1. The recapture event carries the closed episode's
/// exact attempt count.
TEST(ObjectTrackerRedetectionTest, RecaptureClosesEpisodeAndReLossStartsFresh) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    // Two accounted failures in the first episode.
    const auto first = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().attempts, 1U);
    const auto second = tracker.record_redetection_failure(7U, 4);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().attempts, 2U);

    // Recapture through the frozen state machine, then the bookkeeping.
    ASSERT_TRUE(tracker
                    .commit_track_evidence(7U,
                                           verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                           position_of(), std::nullopt, view, 5)
                    .ok());
    const auto event = tracker.record_redetection_recapture(7U, 5, 2);
    ASSERT_TRUE(event.ok()) << event.status().message();
    EXPECT_EQ(event.value().track_id, 7U);
    EXPECT_EQ(event.value().lost_sequence, 2U);
    EXPECT_EQ(event.value().recapture_sequence, 5U);
    EXPECT_EQ(event.value().attempts, 2U) << "the closed episode's exact count";

    // Re-loss: the gate reads a fresh episode (the slot was released).
    ASSERT_EQ(lose_track(tracker, 7U, view, 8), 8U);
    const auto fresh = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 9);
    ASSERT_TRUE(fresh.ok());
    EXPECT_EQ(fresh.value().verdict, RedetectionGateVerdict::kTrigger);
    EXPECT_EQ(fresh.value().attempts, 0U);
    const auto restart = tracker.record_redetection_failure(7U, 9);
    ASSERT_TRUE(restart.ok());
    EXPECT_EQ(restart.value().attempts, 1U) << "a new episode counts from one";
}

/// The stale-key rule without bookkeeping: a track recaptured through the
/// state machine but WITHOUT `record_redetection_recapture` keeps its old
/// episode slot; on re-loss the slot's key no longer matches the new kLost
/// entry sequence, so the gate reads the fresh episode (attempts 0) and the
/// next accounted failure restarts the count and re-keys the slot.
TEST(ObjectTrackerRedetectionTest, StaleEpisodeSlotWithoutBookkeepingReadsFresh) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    const auto first = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().attempts, 1U);

    // Recapture WITHOUT bookkeeping: the episode slot survives the commit
    // (the stale-key mechanism, not an entry, must neutralize it). The commit
    // itself captures a positive template, so the slot's survival is measured
    // against a twin driven through the identical commit WITHOUT the failure:
    // the only byte difference between the two is the slot.
    ObjectTracker twin = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(twin, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(twin, 7U, view, 2), 2U);
    ASSERT_TRUE(twin.commit_track_evidence(7U,
                                           verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                           position_of(), std::nullopt, view, 5)
                    .ok());
    ASSERT_TRUE(tracker
                    .commit_track_evidence(7U,
                                           verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                           position_of(), std::nullopt, view, 5)
                    .ok());
    EXPECT_EQ(tracker.byte_size(), twin.byte_size() + ObjectTracker::kRedetectSlotOverheadBytes)
        << "the slot is bookkeeping's to release";

    // Re-loss: the stale slot reads as the fresh episode it is.
    ASSERT_EQ(lose_track(tracker, 7U, view, 8), 8U);
    const auto fresh = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 9);
    ASSERT_TRUE(fresh.ok());
    EXPECT_EQ(fresh.value().verdict, RedetectionGateVerdict::kTrigger);
    EXPECT_EQ(fresh.value().attempts, 0U) << "the stale count must not leak into the new episode";

    const auto restart = tracker.record_redetection_failure(7U, 9);
    ASSERT_TRUE(restart.ok());
    EXPECT_EQ(restart.value().attempts, 1U) << "the count restarts and re-keys the slot";
}

/// The recapture event applies the same stale-reads-fresh rule to its
/// `attempts` count, keyed by the caller's `lost_sequence` evidence (the only
/// episode key available after the confirming commit zeroed the state slot's
/// entry sequence): a slot keyed by an older episode — a loss episode that
/// ended without recapture bookkeeping, then a re-loss — reports 0, never the
/// dead episode's count, while a keyed episode reports its exact count and a
/// mismatched key reads as the fresh episode. The stale slot is still
/// released with the closed episode.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): the three-branch key rule dominates the metric
TEST(ObjectTrackerRedetectionTest, RecaptureAttemptsApplyTheCallerEpisodeKeyRule) {
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);

    // Stale slot: the dead episode's count must not leak into the new
    // episode's event (regression for the key rule on this read).
    {
        ObjectTracker tracker = make_tracker();
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
        ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
        ASSERT_TRUE(tracker.record_redetection_failure(7U, 3).ok());  // episode 1: count 1, key 2
        ASSERT_TRUE(
            tracker
                .commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 5)
                .ok());  // walk-in recapture, no bookkeeping
        ASSERT_EQ(lose_track(tracker, 7U, view, 8), 8U);
        ASSERT_TRUE(
            tracker
                .commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 10)
                .ok());  // episode 2 recaptured
        const auto stale = tracker.record_redetection_recapture(7U, 10U, 8U);
        ASSERT_TRUE(stale.ok());
        EXPECT_EQ(stale.value().attempts, 0U) << "the stale count must not leak into the new episode's event";
        ASSERT_EQ(tracker.redetection_records().size(), 1U);
        EXPECT_EQ(tracker.redetection_records()[0].attempts, 0U) << "the stored record carries the keyed count";
    }

    // Keyed episode: the slot whose key matches the supplied evidence reports
    // its exact count; a mismatched key reads as the fresh episode (0).
    {
        ObjectTracker tracker = make_tracker();
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
        ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
        ASSERT_TRUE(
            tracker
                .commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 5)
                .ok());  // walk-in recapture of episode 1
        ASSERT_EQ(lose_track(tracker, 7U, view, 8), 8U);
        ASSERT_TRUE(tracker.record_redetection_failure(7U, 9).ok());  // episode 2: count 1, re-keyed to 8
        ASSERT_TRUE(
            tracker
                .commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 10)
                .ok());
        const auto keyed = tracker.record_redetection_recapture(7U, 10U, 8U);
        ASSERT_TRUE(keyed.ok());
        EXPECT_EQ(keyed.value().attempts, 1U) << "the keyed episode's exact count";
        // A mismatched key (the caller asserts an episode that holds no slot)
        // reads as the fresh episode it is.
        const auto mismatched = tracker.record_redetection_recapture(7U, 11U, 5U);
        ASSERT_TRUE(mismatched.ok());
        EXPECT_EQ(mismatched.value().attempts, 0U) << "the mismatched key reads as the fresh episode";
    }

    // The stale slot is still released with the closed episode: after the
    // bookkeeping, the byte footprint equals a twin that ran the identical
    // scenario WITHOUT the accounted failure.
    {
        ObjectTracker tracker = make_tracker();
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
        ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
        ASSERT_TRUE(tracker.record_redetection_failure(7U, 3).ok());
        ASSERT_TRUE(
            tracker
                .commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 5)
                .ok());
        ASSERT_EQ(lose_track(tracker, 7U, view, 8), 8U);
        ASSERT_TRUE(
            tracker
                .commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 10)
                .ok());
        ASSERT_TRUE(tracker.record_redetection_recapture(7U, 10U, 8U).ok());

        ObjectTracker twin = make_tracker();
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(twin, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
        ASSERT_EQ(lose_track(twin, 7U, view, 2), 2U);
        ASSERT_TRUE(
            twin.commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 5)
                .ok());
        ASSERT_EQ(lose_track(twin, 7U, view, 8), 8U);
        ASSERT_TRUE(
            twin.commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                       position_of(), std::nullopt, view, 10)
                .ok());
        EXPECT_EQ(tracker.byte_size(), twin.byte_size() + ObjectTracker::kRedetectionRecordOverheadBytes)
            << "exactly one log record remains: the stale slot was released with the closed episode";
    }
}

// --- association: the new-id branch of the ID semantics -----------------------

/// The association is a diagnostic identity-handoff note: it validates that
/// the predecessor is the replaced open (kLost) identity and the successor is
/// a live adopted track, appends the bounded record, and changes neither
/// track's state. Every validation error leaves the log unchanged.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): the error matrix dominates the metric
TEST(ObjectTrackerRedetectionTest, AssociationIsDiagnosticAndFullyValidated) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    // The successor comes through the normal fusion path (adopt_track,
    // DEC-010) — a different region recalled elsewhere.
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}, 3, "icon"));

    const auto equal_ids = tracker.record_redetection_association(7U, 7U, 4);
    EXPECT_EQ(equal_ids.status().code(), ErrorCode::kInvalidArgument);
    const auto unknown_predecessor = tracker.record_redetection_association(99U, 8U, 4);
    EXPECT_EQ(unknown_predecessor.status().code(), ErrorCode::kInvalidArgument);
    const auto unknown_successor = tracker.record_redetection_association(7U, 99U, 4);
    EXPECT_EQ(unknown_successor.status().code(), ErrorCode::kInvalidArgument);

    // A live predecessor is not a replaced open identity.
    const auto live_predecessor = tracker.record_redetection_association(8U, 7U, 4);
    EXPECT_EQ(live_predecessor.status().code(), ErrorCode::kInvalidArgument);

    // A terminated predecessor is not one either.
    ObjectTracker terminated = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(terminated, view, 6U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(terminated, view, 9U, RectF{40.0F, 40.0F, 8.0F, 8.0F}, 2));
    ASSERT_TRUE(terminated.terminate(6U, 3).ok());
    const auto terminated_predecessor = terminated.record_redetection_association(6U, 9U, 4);
    EXPECT_EQ(terminated_predecessor.status().code(), ErrorCode::kInvalidArgument);
    // A terminated successor is rejected too.
    ASSERT_EQ(lose_track(terminated, 9U, view, 4), 4U);
    ASSERT_TRUE(terminated.terminate(9U, 5).ok());
    const auto terminated_successor = terminated.record_redetection_association(6U, 9U, 6);
    EXPECT_EQ(terminated_successor.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_TRUE(terminated.redetection_records().empty()) << "errors append nothing";

    // The valid handoff: diagnostic only, neither state moves.
    const PoolSnapshot before = snapshot_of(tracker);
    const auto associated = tracker.record_redetection_association(7U, 8U, 4);
    ASSERT_TRUE(associated.ok()) << associated.status().message();
    EXPECT_EQ(associated.value().predecessor_track_id, 7U);
    EXPECT_EQ(associated.value().successor_track_id, 8U);
    EXPECT_EQ(associated.value().sequence, 4U);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kLost) << "the sticky loss is untouched";
    EXPECT_EQ(tracker.find_track(8U)->state, TrackState::kTracking);
    const PoolSnapshot after = snapshot_of(tracker);
    EXPECT_EQ(after.used_bytes, before.used_bytes + ObjectTracker::kRedetectionRecordOverheadBytes);
    EXPECT_EQ(after.records.size(), before.records.size() + 1U);

    // The stored record maps the association fields exactly.
    const std::vector<RedetectionRecord> records = tracker.redetection_records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].kind, RedetectionRecordKind::kAssociation);
    EXPECT_EQ(records[0].sequence, 4U);
    EXPECT_EQ(records[0].track_id, 7U) << "track_id is the predecessor";
    EXPECT_EQ(records[0].related_track_id, 8U) << "related_track_id is the successor";
    EXPECT_EQ(records[0].lost_sequence, 0U);
    EXPECT_EQ(records[0].attempts, 0U);
}

// --- the bounded log: capacity, eviction counting, lifecycle ------------------

/// The pool-wide log holds at most `max_redetection_records` records: overflow
/// drops the OLDEST record with an explicit counter, the order stays oldest
/// first with both kinds mapped exactly, and only `reset` clears the log and
/// its counter.
TEST(ObjectTrackerRedetectionTest, LogOverflowDropsOldestWithExplicitCount) {
    ObjectTrackerOptions options = redetection_options();
    options.max_redetection_records = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}, 1, "icon"));

    // Record 1: track 7 lost and associated with the recalled 8.
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    ASSERT_TRUE(tracker.record_redetection_association(7U, 8U, 3).ok());

    // Record 2: track 8 lost and recaptured (identity continued).
    ASSERT_EQ(lose_track(tracker, 8U, view, 4), 4U);
    ASSERT_TRUE(tracker
                    .commit_track_evidence(8U,
                                           verification_of(8U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                           position_of(), std::nullopt, view, 5)
                    .ok());
    const auto event = tracker.record_redetection_recapture(8U, 5, 4);
    ASSERT_TRUE(event.ok()) << event.status().message();

    const std::vector<RedetectionRecord> full = tracker.redetection_records();
    ASSERT_EQ(full.size(), 2U);
    EXPECT_EQ(full[0].kind, RedetectionRecordKind::kAssociation);
    EXPECT_EQ(full[0].track_id, 7U);
    EXPECT_EQ(full[1].kind, RedetectionRecordKind::kInterruption);
    EXPECT_EQ(full[1].track_id, 8U);
    EXPECT_EQ(full[1].lost_sequence, 4U);
    EXPECT_EQ(full[1].attempts, 0U);
    EXPECT_EQ(tracker.evicted_redetection_record_count(), 0U);
    const int64_t full_bytes = tracker.byte_size();

    // Record 3: overflow — the oldest (the association) is dropped, counted.
    ASSERT_EQ(lose_track(tracker, 7U, view, 7), 7U);
    ASSERT_TRUE(tracker.record_redetection_association(7U, 8U, 8).ok());
    const std::vector<RedetectionRecord> wrapped = tracker.redetection_records();
    ASSERT_EQ(wrapped.size(), 2U);
    EXPECT_EQ(wrapped[0].kind, RedetectionRecordKind::kInterruption);
    EXPECT_EQ(wrapped[0].track_id, 8U);
    EXPECT_EQ(wrapped[1].kind, RedetectionRecordKind::kAssociation);
    EXPECT_EQ(wrapped[1].track_id, 7U);
    EXPECT_EQ(tracker.evicted_redetection_record_count(), 1U);
    EXPECT_EQ(tracker.byte_size(), full_bytes) << "the log is at capacity, byte-stable";

    // Record 4: another counted drop; termination of recorded tracks leaves
    // the log untouched (only reset clears it).
    ASSERT_TRUE(tracker.terminate(7U, 9).ok());
    ASSERT_EQ(tracker.redetection_records().size(), 2U);
    EXPECT_EQ(tracker.evicted_redetection_record_count(), 1U);

    tracker.reset();
    EXPECT_TRUE(tracker.redetection_records().empty());
    EXPECT_EQ(tracker.evicted_redetection_record_count(), 0U) << "the counter is since creation or reset";
}

/// `max_redetection_records` is validated on [1, 4096] per the frozen option
/// contract; the boundaries are accepted.
TEST(ObjectTrackerRedetectionTest, MaxRedetectionRecordsOptionIsBounded) {
    ObjectTrackerOptions zero = redetection_options();
    zero.max_redetection_records = 0;
    EXPECT_EQ(ObjectTracker::create(zero).status().code(), ErrorCode::kInvalidArgument);

    ObjectTrackerOptions over = redetection_options();
    over.max_redetection_records = 4097;
    EXPECT_EQ(ObjectTracker::create(over).status().code(), ErrorCode::kInvalidArgument);

    ObjectTrackerOptions one = redetection_options();
    one.max_redetection_records = 1;
    EXPECT_TRUE(ObjectTracker::create(one).ok());

    ObjectTrackerOptions cap = redetection_options();
    cap.max_redetection_records = 4096;
    EXPECT_TRUE(ObjectTracker::create(cap).ok());
}

// --- slot lifecycle: byte accounting across terminate, eviction, reset ---------

/// The episode slot is accounted (+`kRedetectSlotOverheadBytes` on the first
/// failure, byte-neutral updates afterwards) and released by terminate, track
/// eviction (no residue on the evicted bytes) and `reset`.
TEST(ObjectTrackerRedetectionTest, EpisodeSlotLifecycleAndByteAccounting) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    const int64_t lost_bytes = tracker.byte_size();

    // First failure allocates exactly one slot.
    const auto first = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(tracker.byte_size(), lost_bytes + ObjectTracker::kRedetectSlotOverheadBytes);

    // The second failure updates the slot in place: byte-neutral.
    const auto second = tracker.record_redetection_failure(7U, 4);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(tracker.byte_size(), lost_bytes + ObjectTracker::kRedetectSlotOverheadBytes);

    // Termination releases the slot with the archive: the terminated archive
    // is byte-identical to a twin terminated without any redetection
    // bookkeeping.
    ASSERT_TRUE(tracker.terminate(7U, 5).ok());
    ObjectTracker twin = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(twin, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(lose_track(twin, 7U, view, 2), 2U);
    ASSERT_TRUE(twin.terminate(7U, 5).ok());
    EXPECT_EQ(tracker.byte_size(), twin.byte_size()) << "the terminated archives are identical: the slot was released";

    // Eviction: a track holding an episode slot is evicted without residue.
    ObjectTrackerOptions single = redetection_options();
    single.max_targets = 1;
    ObjectTracker evictor = make_tracker(single);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(evictor, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    const int64_t single_track_bytes = evictor.byte_size();
    ASSERT_EQ(lose_track(evictor, 7U, view, 2), 2U);              // + one state slot
    ASSERT_TRUE(evictor.record_redetection_failure(7U, 3).ok());  // + one episode slot
    ASSERT_EQ(evictor.byte_size(),
              single_track_bytes + ObjectTracker::kStateSlotOverheadBytes + ObjectTracker::kRedetectSlotOverheadBytes);

    const auto evicted = evictor.adopt_track(make_region(8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}), view, 4);
    ASSERT_TRUE(evicted.ok()) << evicted.status().message();
    EXPECT_EQ(evicted.value().evicted_track_ids, (std::vector<uint64_t>{7U}));
    EXPECT_EQ(evictor.byte_size(), single_track_bytes) << "no slot residue on the evicted track's bytes";
    EXPECT_EQ(evictor.evicted_track_count(), 1U);
}

// --- identity review: both ID-semantic branches over the frozen reuse ---------

/// The review-passed branch, over the REAL frozen entries: verify_track (the
/// M7-05 identity review) on the kLost track grades kStrong against the
/// adoption frame, the confirming commit_track_evidence performs the frozen
/// kLost -> kTracking edge, and record_redetection_recapture appends the
/// interruption event and closes the episode. The track id CONTINUES — this
/// is the identity-review pipeline of design section 7, not a rewritten one.
/// With one accounted failure before the review, the event carries attempts 1
/// and the gate reports kInactive afterwards.
TEST(ObjectTrackerRedetectionTest, IdentityReviewPassedBranchContinuesIdAndRecordsInterruption) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);

    // The gate triggers, the upper layer spends one attempt, it fails.
    const auto gate = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    ASSERT_TRUE(gate.ok());
    ASSERT_EQ(gate.value().verdict, RedetectionGateVerdict::kTrigger);
    const auto failure = tracker.record_redetection_failure(7U, 3);
    ASSERT_TRUE(failure.ok());
    ASSERT_EQ(failure.value().attempts, 1U);

    // Identity review: the REAL M7-05 verifier on the kLost candidate.
    const auto review = tracker.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(review.ok()) << review.status().message();
    ASSERT_EQ(review.value().appearance.outcome, AppearanceChannelOutcome::kStrong);
    ASSERT_EQ(review.value().state, TrackState::kLost);

    // The confirming commit: the frozen M7-06 kLost -> kTracking edge.
    const auto commit = tracker.commit_track_evidence(7U, review.value(), position_of(), std::nullopt, view, 4);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(commit.value().previous_state, TrackState::kLost);
    EXPECT_EQ(commit.value().state, TrackState::kTracking);

    // The interruption event closes the episode; the id continued.
    const auto event = tracker.record_redetection_recapture(7U, 4, 2);
    ASSERT_TRUE(event.ok()) << event.status().message();
    EXPECT_EQ(event.value().track_id, 7U) << "the identity continued";
    EXPECT_EQ(event.value().lost_sequence, 2U);
    EXPECT_EQ(event.value().recapture_sequence, 4U);
    EXPECT_EQ(event.value().attempts, 1U);

    const std::vector<RedetectionRecord> records = tracker.redetection_records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].kind, RedetectionRecordKind::kInterruption);
    EXPECT_EQ(records[0].sequence, 4U);
    EXPECT_EQ(records[0].track_id, 7U);
    EXPECT_EQ(records[0].related_track_id, 0U) << "no related id for an interruption";
    EXPECT_EQ(records[0].lost_sequence, 2U);
    EXPECT_EQ(records[0].attempts, 1U);

    // The recaptured track is live again: the gate reports kInactive.
    const auto inactive = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 5);
    ASSERT_TRUE(inactive.ok());
    EXPECT_EQ(inactive.value().verdict, RedetectionGateVerdict::kInactive);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);
}

/// The evidence-insufficient branch: the review does not confirm, the caller
/// assigns a new id through the normal fusion path (`adopt_track`, DEC-010 —
/// the new-id branch never bypasses the static fusion semantics) and records
/// the association. The predecessor keeps its sticky loss; its later
/// disposition (explicit terminate) stays with the frozen paths.
TEST(ObjectTrackerRedetectionTest, IdentityReviewInsufficientBranchAdoptsNewIdAndRecordsAssociation) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    ASSERT_TRUE(tracker.record_redetection_failure(7U, 3).ok());

    // The gate triggers, the attempt failed, the upper layer gives up on the
    // old identity and adopts the recalled region as a new track.
    const auto successor = tracker.adopt_track(make_region(8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}, 0.9F, "icon"), view, 5);
    ASSERT_TRUE(successor.ok()) << successor.status().message();
    ASSERT_EQ(successor.value().track_id, 8U);

    const auto associated = tracker.record_redetection_association(7U, 8U, 5);
    ASSERT_TRUE(associated.ok()) << associated.status().message();
    EXPECT_EQ(associated.value().predecessor_track_id, 7U);
    EXPECT_EQ(associated.value().successor_track_id, 8U);
    EXPECT_EQ(associated.value().sequence, 5U);

    // The predecessor's disposition is unchanged: sticky loss, explicit
    // caller terminate still available.
    ASSERT_EQ(tracker.find_track(7U)->state, TrackState::kLost);
    ASSERT_TRUE(tracker.terminate(7U, 6).ok());
    ASSERT_EQ(tracker.find_track(7U)->state, TrackState::kTerminated);
    // The association record survives the predecessor's termination.
    ASSERT_EQ(tracker.redetection_records().size(), 1U);
    EXPECT_EQ(tracker.redetection_records()[0].kind, RedetectionRecordKind::kAssociation);
}

// --- cancellation and deadlines ------------------------------------------------

/// All four entries poll cancellation exactly once at the entry and convert
/// the two context signals explicitly: a cancelled context reports kCancelled
/// (cancel-first, the `compensate_global_motion` entry order — even for an
/// invalid input), an expired deadline reports kTimeout, and the valid call
/// afterwards still succeeds.
TEST(ObjectTrackerRedetectionTest, CancelAndDeadlineConvertExplicitlyOnAllEntries) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}, 1));

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);

    // Cancel-first: even an unknown track id reports the cancellation.
    const auto gate_cancelled = tracker.evaluate_redetection_gate(99U, ChangeClassification::kPartial, 2, cancelled);
    EXPECT_EQ(gate_cancelled.status().code(), ErrorCode::kCancelled);
    const auto gate_timeout = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 2, expired);
    EXPECT_EQ(gate_timeout.status().code(), ErrorCode::kTimeout);

    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    const PoolSnapshot before = snapshot_of(tracker);

    const auto failure_cancelled = tracker.record_redetection_failure(99U, 3, cancelled);
    EXPECT_EQ(failure_cancelled.status().code(), ErrorCode::kCancelled);
    const auto failure_timeout = tracker.record_redetection_failure(7U, 3, expired);
    EXPECT_EQ(failure_timeout.status().code(), ErrorCode::kTimeout);

    const auto recapture_cancelled = tracker.record_redetection_recapture(99U, 3, 2, cancelled);
    EXPECT_EQ(recapture_cancelled.status().code(), ErrorCode::kCancelled);
    const auto recapture_timeout = tracker.record_redetection_recapture(7U, 3, 2, expired);
    EXPECT_EQ(recapture_timeout.status().code(), ErrorCode::kTimeout);

    const auto association_cancelled = tracker.record_redetection_association(99U, 8U, 3, cancelled);
    EXPECT_EQ(association_cancelled.status().code(), ErrorCode::kCancelled);
    const auto association_timeout = tracker.record_redetection_association(7U, 8U, 3, expired);
    EXPECT_EQ(association_timeout.status().code(), ErrorCode::kTimeout);

    // The state-changing entries moved nothing on their cancelled paths.
    EXPECT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kLost);

    // Valid contexts: the calls run normally afterwards.
    const auto gate = tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3);
    ASSERT_TRUE(gate.ok());
    EXPECT_EQ(gate.value().verdict, RedetectionGateVerdict::kTrigger);
}

// --- determinism ---------------------------------------------------------------

/// The whole cascade is deterministic: two identically configured trackers
/// driven through the identical scenario produce bitwise-identical gate
/// decisions, failure records, the interruption event, the association, log
/// contents, byte accounting and final states.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): the twin scenario's assert density dominates the metric
TEST(ObjectTrackerRedetectionTest, TwinInstancesAreBitwiseDeterministic) {
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one scenario step per assert
    auto run_scenario = [&]() {
        ObjectTracker tracker = make_tracker();
        EXPECT_TRUE(tracker.adopt_track(make_region(7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 0.9F, "icon"), view, 1).ok());
        std::vector<RedetectionGateDecision> gates;
        gates.push_back(tracker.evaluate_redetection_gate(7U, ChangeClassification::kNone, 2).value());
        gates.push_back(tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 2).value());
        EXPECT_EQ(lose_track(tracker, 7U, view, 2), 2U);
        gates.push_back(tracker.evaluate_redetection_gate(7U, ChangeClassification::kGlobal, 3).value());
        std::vector<RedetectionFailureRecord> failures;
        failures.push_back(tracker.record_redetection_failure(7U, 3).value());
        gates.push_back(tracker.evaluate_redetection_gate(7U, ChangeClassification::kPartial, 3).value());
        failures.push_back(tracker.record_redetection_failure(7U, 5).value());
        auto review = tracker.verify_track(7U, view, std::nullopt);
        EXPECT_TRUE(review.ok());
        auto commit = tracker.commit_track_evidence(7U, review.value(), position_of(), std::nullopt, view, 6);
        EXPECT_TRUE(commit.ok());
        auto event = tracker.record_redetection_recapture(7U, 6, 2);
        EXPECT_TRUE(event.ok());
        EXPECT_TRUE(tracker.adopt_track(make_region(8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}), view, 7).ok());
        EXPECT_EQ(lose_track(tracker, 8U, view, 8), 8U);
        auto association = tracker.record_redetection_association(8U, 7U, 9);
        EXPECT_TRUE(association.ok());
        struct Outcome {
            std::vector<RedetectionGateDecision> gates;
            std::vector<RedetectionFailureRecord> failures;
            TrackInterruptionEvent event;
            RedetectionAssociation association;
            std::vector<RedetectionRecord> records;
            int64_t used_bytes = 0;
            std::vector<TrackState> states;
        } outcome;
        outcome.gates = std::move(gates);
        outcome.failures = std::move(failures);
        outcome.event = event.value();
        outcome.association = association.value();
        outcome.records = tracker.redetection_records();
        outcome.used_bytes = tracker.byte_size();
        for (const uint64_t id : tracker.track_ids()) {
            outcome.states.push_back(tracker.find_track(id)->state);
        }
        return outcome;
    };

    const auto first = run_scenario();
    const auto second = run_scenario();

    ASSERT_EQ(first.gates.size(), second.gates.size());
    for (size_t i = 0; i < first.gates.size(); ++i) {
        EXPECT_NO_FATAL_FAILURE(expect_gate_equal(first.gates[i], second.gates[i])) << "gate " << i;
    }
    ASSERT_EQ(first.failures.size(), second.failures.size());
    for (size_t i = 0; i < first.failures.size(); ++i) {
        EXPECT_NO_FATAL_FAILURE(expect_failure_equal(first.failures[i], second.failures[i])) << "failure " << i;
    }
    EXPECT_EQ(first.event.track_id, second.event.track_id);
    EXPECT_EQ(first.event.lost_sequence, second.event.lost_sequence);
    EXPECT_EQ(first.event.recapture_sequence, second.event.recapture_sequence);
    EXPECT_EQ(first.event.attempts, second.event.attempts);
    EXPECT_EQ(first.association.predecessor_track_id, second.association.predecessor_track_id);
    EXPECT_EQ(first.association.successor_track_id, second.association.successor_track_id);
    EXPECT_EQ(first.association.sequence, second.association.sequence);
    EXPECT_NO_FATAL_FAILURE(expect_records_equal(first.records, second.records));
    EXPECT_EQ(first.used_bytes, second.used_bytes);
    EXPECT_EQ(first.states, second.states);
}

// --- privacy (RULE-10/DOD-06): records carry ids and sequences only ------------

/// The trace representation is the privacy guarantee: with rich semantics on
/// every track, every appended record — exhaustively field-checked — carries
/// only the kind, one frame sequence, and track ids with the episode numbers.
/// No coordinates, no template or image content, no semantics text exists in
/// the record types to leak.
TEST(ObjectTrackerRedetectionTest, RecordsCarryOnlyIdsAndSequences) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const std::string private_label = "private-document-title-with-contents";
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, private_label));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}, 1, private_label));

    ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U);
    ASSERT_TRUE(tracker.record_redetection_failure(7U, 3).ok());
    ASSERT_TRUE(tracker
                    .commit_track_evidence(7U,
                                           verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
                                           position_of(), std::nullopt, view, 4)
                    .ok());
    ASSERT_TRUE(tracker.record_redetection_recapture(7U, 4, 2).ok());
    ASSERT_EQ(lose_track(tracker, 7U, view, 5), 5U);
    ASSERT_TRUE(tracker.record_redetection_association(7U, 8U, 6).ok());

    const std::vector<RedetectionRecord> records = tracker.redetection_records();
    ASSERT_EQ(records.size(), 2U);
    // Exhaustive field checks: these five fields ARE the whole representation.
    EXPECT_EQ(records[0].kind, RedetectionRecordKind::kInterruption);
    EXPECT_EQ(records[0].sequence, 4U);
    EXPECT_EQ(records[0].track_id, 7U);
    EXPECT_EQ(records[0].related_track_id, 0U);
    EXPECT_EQ(records[0].lost_sequence, 2U);
    EXPECT_EQ(records[0].attempts, 1U);
    EXPECT_EQ(records[1].kind, RedetectionRecordKind::kAssociation);
    EXPECT_EQ(records[1].sequence, 6U);
    EXPECT_EQ(records[1].track_id, 7U);
    EXPECT_EQ(records[1].related_track_id, 8U);
    EXPECT_EQ(records[1].lost_sequence, 0U);
    EXPECT_EQ(records[1].attempts, 0U);
}

// --- DOD-03: the reused coordinate surface under the rotation matrix ----------

/// The four entries consume no coordinates themselves; the one
/// coordinate-consuming reuse is the M7-05 identity review. Across 0/90/180/
/// 270 rotation metadata x an odd-sized frame x non-contiguous stride, the
/// review grades kStrong on the adoption frame, the confirming commit
/// recaptures and the interruption event is recorded — the DOD-03 matrix
/// holds over the composed pipeline.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): matrix cells dominate the metric
TEST(ObjectTrackerRedetectionTest, IdentityReviewReuseUnderRotationOddSizeStrideMatrix) {
    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        ObjectTracker tracker = make_tracker();
        const GrayImage frame = noise_image(21, 15, 7);  // odd x odd, +7 stride padding
        const ImageView view = view_of(frame, rotation);
        const RectF bounds{6.0F, 4.0F, 8.0F, 6.0F};
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1))
            << "rotation " << static_cast<int>(rotation);
        ASSERT_EQ(lose_track(tracker, 7U, view, 2), 2U) << "rotation " << static_cast<int>(rotation);

        const auto review = tracker.verify_track(7U, view, std::nullopt);
        ASSERT_TRUE(review.ok()) << "rotation " << static_cast<int>(rotation);
        ASSERT_EQ(review.value().appearance.outcome, AppearanceChannelOutcome::kStrong)
            << "rotation " << static_cast<int>(rotation);
        const auto commit = tracker.commit_track_evidence(7U, review.value(), position_of(), std::nullopt, view, 3);
        ASSERT_TRUE(commit.ok()) << "rotation " << static_cast<int>(rotation);
        ASSERT_EQ(commit.value().state, TrackState::kTracking) << "rotation " << static_cast<int>(rotation);
        const auto event = tracker.record_redetection_recapture(7U, 3, 2);
        ASSERT_TRUE(event.ok()) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(event.value().track_id, 7U) << "rotation " << static_cast<int>(rotation);
        ASSERT_EQ(tracker.redetection_records().size(), 1U);
        EXPECT_EQ(tracker.redetection_records()[0].kind, RedetectionRecordKind::kInterruption)
            << "rotation " << static_cast<int>(rotation);
    }
}

}  // namespace
