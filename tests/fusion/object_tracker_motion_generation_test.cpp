// M7-07 global motion compensation and layout-generation integration:
// independent verification suite for `advance_generation_for_classification`,
// `compensate_global_motion` and `sweep_generation_lag` (object-tracking
// design sections 6.3 and 6.4). Written against the frozen header contracts
// only:
//   * The trigger decision: only `ChangeClassification::kGlobal` advances the
//     layout generation (one step per call), kNone/kPartial leave it and every
//     track untouched, an unknown enum value is an explicit kInvalidArgument,
//     and `evaluate_change_gate` stays a pure read (the M7-03 deferral is
//     closed, not moved).
//   * The compensation: a uniform component-wise float translation of every
//     non-terminated track's `last_bounds` with `predicted_center` recomputed
//     as the exact new center (the frozen invariant maintained), terminated
//     archives staying put, history/templates/generations/state untouched, an
//     inclusive confidence gate with an explicit `applied == false` refusal,
//     atomic validation (non-finite/overflow failures leave the pool
//     untouched, plan before mutation across the whole pool), the M7-07
//     entry-poll cancellation order and bitwise determinism.
//   * The generation-lag sweep: the strict `lag > max_generation_lag` boundary,
//     the kUncertain-only exhausted-evidence condition (kTracking keeps its
//     confirmed status, kLost stays sticky, kTerminated archives untouched),
//     ascending reported ids, cancellation conversion, and the kLost ->
//     kTracking edge remaining exclusive to confirming commits.
//   * The pipeline chain: trigger(kGlobal) + `kGenerationSwitch` commits
//     degrade a kTracking track without confirming evidence and recover on
//     confirmation (the M7-06 state machine driven by the M7-07 trigger).
//   * Obligation 5, the scroll round trip: `estimate_global_shift` (M7-04) in
//     the loop — before compensation the stale prior short-circuits a partial
//     change report away from the true position (kReuse) and verification only
//     finds the target at the full scroll offset; after compensation the same
//     report verifies (kVerify), the same-frame check is strong at offset
//     (0, 0) and a `kCompensatedScroll` commit confirms — plus the DOD-03
//     matrix (0/90/180/270 rotation metadata x odd size x non-contiguous
//     stride x flush edges x both shift directions) where the translation is
//     metadata-independent and the round trip holds under every cell.
//
// Privacy (RULE-10/DOD-06) is structural here, as in the M7-03/M7-05/M7-06
// suites: the result types (`GenerationAdvance`, `MotionCompensationResult`,
// the swept-id vector) carry only ids, enums, states and coordinate rectangles
// — no template bytes, thumbnails or frame content exist in them to leak; the
// tracker is an in-memory decision with no logging, filesystem or network
// surface; the shared privacy suite covers the fusion pipeline binary.

#include <mirador/object_tracker.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/shift_estimation.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::AppearanceChannelOutcome;
using mirador::ChangeClassification;
using mirador::ChangeGateDecision;
using mirador::ChangeReport;
using mirador::ErrorCode;
using mirador::EvidenceGrade;
using mirador::ExecutionContext;
using mirador::GenerationAdvance;
using mirador::ImageView;
using mirador::MotionCompensationResult;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::PositionScenario;
using mirador::RectF;
using mirador::RectI;
using mirador::Rotation;
using mirador::ShiftEstimate;
using mirador::TargetTrack;
using mirador::TrackPositionEvidence;
using mirador::TrackSemantics;
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
/// high-frequency texture keeps both the SAD shift surface and the E1 NCC
/// surface peaked (a smoke-traffic concern, not a contract one).
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

/// The frame after a global scroll: the content of `previous` moved by
/// (dx, dy) — `p_current = p_previous + (dx, dy)`, the `ShiftEstimate`
/// direction convention — and pixels scrolled in from outside the previous
/// frame take `outside`.
GrayImage scrolled_frame(const GrayImage& previous, int32_t dx, int32_t dy, uint8_t outside) {
    const int64_t padding = previous.stride - previous.width;
    GrayImage image = make_gray_image(previous.width, previous.height, std::byte{outside}, padding);
    for (int32_t y = 0; y < image.height; ++y) {
        for (int32_t x = 0; x < image.width; ++x) {
            const int32_t sx = x - dx;
            const int32_t sy = y - dy;
            if (sx >= 0 && sy >= 0 && sx < previous.width && sy < previous.height) {
                image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x)] =
                    previous.pixels[static_cast<size_t>(sy) * static_cast<size_t>(previous.stride) +
                                    static_cast<size_t>(sx)];
            }
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

/// Hand-built caller evidence (the tracker consumes estimates, it never runs
/// the estimator itself — the evidence-trust boundary under test).
ShiftEstimate shift_of(float dx, float dy, float confidence) {
    ShiftEstimate shift;
    shift.thumbnail_dx = static_cast<int32_t>(dx);
    shift.thumbnail_dy = static_cast<int32_t>(dy);
    shift.dx = dx;
    shift.dy = dy;
    shift.confidence = confidence;
    return shift;
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

ObjectTracker make_tracker(const ObjectTrackerOptions& options = {}) {
    auto created = ObjectTracker::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_tracker: " << created.status().message();
        return ObjectTracker{};
    }
    return created.take_value();
}

void adopt_or_fail(ObjectTracker& tracker, const ImageView& view, uint64_t id, const RectF& bounds,
                   uint64_t frame_sequence = 1) {
    const auto adopted = tracker.adopt_track(make_region(id, bounds), view, frame_sequence);
    ASSERT_TRUE(adopted.ok()) << "adopt " << id << ": " << adopted.status().message();
}

/// Hand-built caller evidence for the state-machine chain tests; the commit is
/// contractually forbidden from re-running the scan, so the channel outcomes
/// are test inputs here.
TrackVerification verification_of(uint64_t track_id, TrackState state_echo, AppearanceChannelOutcome appearance,
                                  int32_t offset_dx = 0, int32_t offset_dy = 0, double peak_ncc = 0.9) {
    TrackVerification verification;
    verification.track_id = track_id;
    verification.state = state_echo;
    verification.verification_roi = RectI{0, 0, 1, 1};
    verification.appearance.outcome = appearance;
    verification.appearance.peak_ncc = peak_ncc;
    verification.appearance.peak_sidelobe_ratio = 10.0;
    verification.appearance.best_template_index = 0U;
    verification.appearance.best_offset_dx = offset_dx;
    verification.appearance.best_offset_dy = offset_dy;
    return verification;
}

TrackPositionEvidence position_of(PositionScenario scenario, bool inside_gate) {
    TrackPositionEvidence position;
    position.scenario = scenario;
    position.inside_gate = inside_gate;
    return position;
}

TrackSemantics semantics_of(std::string label) {
    TrackSemantics semantics;
    semantics.label = std::move(label);
    return semantics;
}

ChangeReport make_gate_report(const ChangeClassification classification, std::vector<RectI> regions) {
    ChangeReport report;
    report.classification = classification;
    report.changed_regions = std::move(regions);
    return report;
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

/// One EXPECT per compared field.
void expect_pool_untouched(const PoolSnapshot& before, const ObjectTracker& tracker) {
    EXPECT_EQ(tracker.track_count(), before.track_count);
    EXPECT_EQ(tracker.byte_size(), before.used_bytes);
    EXPECT_EQ(tracker.evicted_track_count(), before.evicted_count);
    EXPECT_EQ(tracker.layout_generation(), before.layout_generation);
    EXPECT_EQ(tracker.track_ids(), before.ids);
}

/// Deep per-track comparison for error paths (one EXPECT per compared field).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_track_untouched(const TargetTrack& copy, const ObjectTracker& tracker) {
    const TargetTrack* track = tracker.find_track(copy.track_id);
    ASSERT_NE(track, nullptr) << "track " << copy.track_id << " vanished";
    EXPECT_EQ(track->state, copy.state);
    EXPECT_EQ(track->last_bounds, copy.last_bounds);
    EXPECT_EQ(track->predicted_center, copy.predicted_center);
    EXPECT_EQ(track->confidence, copy.confidence);
    EXPECT_EQ(track->last_verified_sequence, copy.last_verified_sequence);
    EXPECT_EQ(track->terminated_sequence, copy.terminated_sequence);
    EXPECT_EQ(track->layout_generation, copy.layout_generation);
    EXPECT_EQ(track->position_history.size(), copy.position_history.size());
    for (size_t i = 0; i < copy.position_history.size(); ++i) {
        EXPECT_EQ(track->position_history[i].frame_sequence, copy.position_history[i].frame_sequence);
        EXPECT_EQ(track->position_history[i].bounds, copy.position_history[i].bounds);
        EXPECT_EQ(track->position_history[i].layout_generation, copy.position_history[i].layout_generation);
    }
    ASSERT_EQ(track->templates.size(), copy.templates.size());
    for (size_t i = 0; i < copy.templates.size(); ++i) {
        EXPECT_EQ(track->templates[i].fingerprint, copy.templates[i].fingerprint);
        EXPECT_EQ(track->templates[i].frame_sequence, copy.templates[i].frame_sequence);
        EXPECT_EQ(track->templates[i].layout_generation, copy.templates[i].layout_generation);
    }
}

void expect_advance_equal(const GenerationAdvance& lhs, const GenerationAdvance& rhs) {
    EXPECT_EQ(lhs.advanced, rhs.advanced);
    EXPECT_EQ(lhs.generation, rhs.generation);
}

/// One EXPECT per compared field of the deterministic compensation trace.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_compensation_equal(const MotionCompensationResult& lhs, const MotionCompensationResult& rhs) {
    EXPECT_EQ(lhs.applied, rhs.applied);
    EXPECT_EQ(lhs.dx, rhs.dx);
    EXPECT_EQ(lhs.dy, rhs.dy);
    ASSERT_EQ(lhs.tracks.size(), rhs.tracks.size());
    for (size_t i = 0; i < lhs.tracks.size(); ++i) {
        EXPECT_EQ(lhs.tracks[i].track_id, rhs.tracks[i].track_id);
        EXPECT_EQ(lhs.tracks[i].state, rhs.tracks[i].state);
        EXPECT_EQ(lhs.tracks[i].previous_bounds, rhs.tracks[i].previous_bounds);
        EXPECT_EQ(lhs.tracks[i].compensated_bounds, rhs.tracks[i].compensated_bounds);
    }
}

/// Exact center of `bounds` (the frozen invariant formula).
PointF center_of(const RectF& bounds) {
    return PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
}

// --- options validation ---------------------------------------------------------------

TEST(ObjectTrackerMotionGenerationTest, CreateRejectsCompensationConfidenceOutsideUnitRange) {
    ObjectTrackerOptions options;
    options.min_compensation_confidence = -0.1;
    EXPECT_EQ(ObjectTracker::create(options).status().code(), ErrorCode::kInvalidArgument);
    options.min_compensation_confidence = 1.1;
    EXPECT_EQ(ObjectTracker::create(options).status().code(), ErrorCode::kInvalidArgument);
    options.min_compensation_confidence = 0.0;
    EXPECT_TRUE(ObjectTracker::create(options).ok());
    options.min_compensation_confidence = 1.0;
    EXPECT_TRUE(ObjectTracker::create(options).ok());
}

// --- obligation 1: the frozen trigger decision ----------------------------------------

/// Only kGlobal advances the generation, exactly one step per call; kNone and
/// kPartial report the unchanged value without touching the pool; and the
/// M7-03 gate stays a pure read — a kGlobal change-gate evaluation never
/// advances anything by itself (the deferral is closed, not moved).
TEST(ObjectTrackerMotionGenerationTest, GenerationTriggerAdvancesOnlyOnGlobalClassification) {
    ObjectTracker tracker = make_tracker();

    const auto none = tracker.advance_generation_for_classification(ChangeClassification::kNone);
    ASSERT_TRUE(none.ok()) << none.status().message();
    EXPECT_FALSE(none.value().advanced);
    EXPECT_EQ(none.value().generation, 0U);
    EXPECT_EQ(tracker.layout_generation(), 0U);

    const auto partial = tracker.advance_generation_for_classification(ChangeClassification::kPartial);
    ASSERT_TRUE(partial.ok()) << partial.status().message();
    EXPECT_FALSE(partial.value().advanced);
    EXPECT_EQ(partial.value().generation, 0U);

    const auto global = tracker.advance_generation_for_classification(ChangeClassification::kGlobal);
    ASSERT_TRUE(global.ok()) << global.status().message();
    EXPECT_TRUE(global.value().advanced);
    EXPECT_EQ(global.value().generation, 1U);
    EXPECT_EQ(tracker.layout_generation(), 1U);

    // One step per call, and the echo of a non-advancing call is the current
    // value.
    const auto second_global = tracker.advance_generation_for_classification(ChangeClassification::kGlobal);
    ASSERT_TRUE(second_global.ok());
    EXPECT_TRUE(second_global.value().advanced);
    EXPECT_EQ(second_global.value().generation, 2U);
    const auto echo_after = tracker.advance_generation_for_classification(ChangeClassification::kNone);
    ASSERT_TRUE(echo_after.ok());
    EXPECT_FALSE(echo_after.value().advanced);
    EXPECT_EQ(echo_after.value().generation, 2U);
    EXPECT_EQ(tracker.layout_generation(), 2U);

    // The change gate remains const-pure on the same classification.
    const PoolSnapshot before = snapshot_of(tracker);
    const auto gate = tracker.evaluate_change_gate(make_gate_report(ChangeClassification::kGlobal, {}));
    ASSERT_TRUE(gate.ok()) << gate.status().message();
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));
    EXPECT_EQ(tracker.layout_generation(), 2U);
}

TEST(ObjectTrackerMotionGenerationTest, GenerationTriggerRejectsUnknownClassification) {
    ObjectTracker tracker = make_tracker();
    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    const PoolSnapshot before = snapshot_of(tracker);

    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): undefined-value contract test
    const auto garbage = tracker.advance_generation_for_classification(static_cast<ChangeClassification>(200));
    EXPECT_EQ(garbage.status().code(), ErrorCode::kInvalidArgument);
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));
    EXPECT_EQ(tracker.layout_generation(), 1U);
}

/// The trigger only moves the deterministic counter: tracks in any state keep
/// every field — including their stamped `layout_generation` — byte_size
/// included (degradation is the M7-06 commit chain's decision, never the
/// counter's).
TEST(ObjectTrackerMotionGenerationTest, GenerationAdvanceMovesOnlyTheCounter) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 4;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 5U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{30.0F, 20.0F, 8.0F, 8.0F}));
    // One track degrades to kUncertain so the counter move touches no state.
    const auto degraded =
        tracker.commit_track_evidence(5U, verification_of(5U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(degraded.ok()) << degraded.status().message();
    ASSERT_EQ(degraded.value().state, TrackState::kUncertain);

    std::vector<TargetTrack> before;
    for (const uint64_t id : tracker.track_ids()) {
        before.push_back(*tracker.find_track(id));
    }
    const PoolSnapshot pool_before = snapshot_of(tracker);

    const auto advanced = tracker.advance_generation_for_classification(ChangeClassification::kGlobal);
    ASSERT_TRUE(advanced.ok()) << advanced.status().message();
    EXPECT_EQ(tracker.layout_generation(), 1U);
    EXPECT_EQ(tracker.byte_size(), pool_before.used_bytes);
    ASSERT_EQ(before.size(), tracker.track_ids().size());
    for (const TargetTrack& copy : before) {
        ASSERT_NO_FATAL_FAILURE(expect_track_untouched(copy, tracker));
    }
    EXPECT_EQ(tracker.find_track(5U)->state, TrackState::kUncertain);
    EXPECT_EQ(tracker.find_track(5U)->layout_generation, 0U);  // only commits stamp
}

// --- obligation 4: compensation --------------------------------------------------------

/// Every non-terminated track translates by (dx, dy) with the center invariant
/// recomputed exactly; terminated archives stay; the per-track echo is exact
/// and ascending; history, templates, generations, evidence fields, states and
/// the byte account are untouched (a coordinate update, not evidence).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): per-field assertions dominate the metric
TEST(ObjectTrackerMotionGenerationTest, CompensationTranslatesLiveTracksAndSkipsTerminated) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF tracking_bounds{16.0F, 16.0F, 8.0F, 8.0F};
    const RectF uncertain_bounds{28.0F, 16.0F, 8.0F, 8.0F};
    const RectF lost_bounds{16.0F, 28.0F, 8.0F, 8.0F};
    const RectF terminated_bounds{40.0F, 40.0F, 4.0F, 4.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 9U, tracking_bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 5U, uncertain_bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 3U, lost_bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 1U, terminated_bounds));

    // 5: one insufficient commit -> kUncertain; 3: two -> kLost; 1: archived.
    const auto first =
        tracker.commit_track_evidence(5U, verification_of(5U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_EQ(first.value().state, TrackState::kUncertain);
    const auto second =
        tracker.commit_track_evidence(3U, verification_of(3U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(second.ok()) << second.status().message();
    const auto third =
        tracker.commit_track_evidence(3U, verification_of(3U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 4);
    ASSERT_TRUE(third.ok()) << third.status().message();
    ASSERT_EQ(third.value().state, TrackState::kLost);
    ASSERT_TRUE(tracker.terminate(1U, 5).ok());

    // Dyadic shifts keep every float addition exact.
    const ShiftEstimate shift = shift_of(3.5F, -2.5F, 1.0F);
    const int64_t bytes_before = tracker.byte_size();
    const auto compensated = tracker.compensate_global_motion(shift);
    ASSERT_TRUE(compensated.ok()) << compensated.status().message();
    EXPECT_TRUE(compensated.value().applied);
    EXPECT_EQ(compensated.value().dx, 3.5F);
    EXPECT_EQ(compensated.value().dy, -2.5F);
    ASSERT_EQ(compensated.value().tracks.size(), 3U);  // the terminated archive is excluded
    const std::vector<uint64_t> entry_ids{compensated.value().tracks[0].track_id,
                                          compensated.value().tracks[1].track_id,
                                          compensated.value().tracks[2].track_id};
    EXPECT_EQ(entry_ids, (std::vector<uint64_t>{3U, 5U, 9U}));  // ascending

    const RectF tracking_after{19.5F, 13.5F, 8.0F, 8.0F};
    const RectF uncertain_after{31.5F, 13.5F, 8.0F, 8.0F};
    const RectF lost_after{19.5F, 25.5F, 8.0F, 8.0F};
    const TargetTrack* tracking = tracker.find_track(9U);
    const TargetTrack* uncertain = tracker.find_track(5U);
    const TargetTrack* lost = tracker.find_track(3U);
    const TargetTrack* terminated = tracker.find_track(1U);
    ASSERT_NE(tracking, nullptr);
    ASSERT_NE(uncertain, nullptr);
    ASSERT_NE(lost, nullptr);
    ASSERT_NE(terminated, nullptr);
    EXPECT_EQ(tracking->last_bounds, tracking_after);
    EXPECT_EQ(tracking->predicted_center, center_of(tracking_after));
    EXPECT_EQ(uncertain->last_bounds, uncertain_after);
    EXPECT_EQ(uncertain->predicted_center, center_of(uncertain_after));
    EXPECT_EQ(lost->last_bounds, lost_after);
    EXPECT_EQ(lost->predicted_center, center_of(lost_after));
    EXPECT_EQ(terminated->last_bounds, terminated_bounds);  // archives stay put
    EXPECT_EQ(terminated->predicted_center, center_of(terminated_bounds));

    // Echo entries carry the state at compensation time and the exact bounds.
    EXPECT_EQ(compensated.value().tracks[0].state, TrackState::kLost);
    EXPECT_EQ(compensated.value().tracks[0].previous_bounds, lost_bounds);
    EXPECT_EQ(compensated.value().tracks[0].compensated_bounds, lost_after);
    EXPECT_EQ(compensated.value().tracks[1].state, TrackState::kUncertain);
    EXPECT_EQ(compensated.value().tracks[1].previous_bounds, uncertain_bounds);
    EXPECT_EQ(compensated.value().tracks[1].compensated_bounds, uncertain_after);
    EXPECT_EQ(compensated.value().tracks[2].state, TrackState::kTracking);
    EXPECT_EQ(compensated.value().tracks[2].previous_bounds, tracking_bounds);
    EXPECT_EQ(compensated.value().tracks[2].compensated_bounds, tracking_after);

    // States, evidence fields, history, templates, generations and bytes: all
    // untouched by the coordinate update.
    EXPECT_EQ(tracking->state, TrackState::kTracking);
    EXPECT_EQ(uncertain->state, TrackState::kUncertain);
    EXPECT_EQ(lost->state, TrackState::kLost);
    for (const uint64_t id : {9U, 5U, 3U}) {
        const TargetTrack* track = tracker.find_track(id);
        ASSERT_NE(track, nullptr);
        ASSERT_EQ(track->position_history.size(), 1U);
        EXPECT_EQ(track->position_history[0].bounds,
                  id == 9U ? tracking_bounds : (id == 5U ? uncertain_bounds : lost_bounds));
        EXPECT_EQ(track->templates.size(), 1U);
        EXPECT_EQ(track->layout_generation, 0U);
    }
    EXPECT_EQ(tracking->last_verified_sequence, 1U);  // adoption sequence kept
    EXPECT_EQ(tracker.byte_size(), bytes_before);
}

/// The confidence gate is inclusive at the threshold and its refusal is an
/// explicit, echoed, pool-untouched outcome (RULE-06: nothing drops silently).
TEST(ObjectTrackerMotionGenerationTest, ConfidenceGateRefusesBelowThresholdAndAppliesAtIt) {
    ObjectTrackerOptions options;
    options.min_compensation_confidence = 0.5;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    const TargetTrack before = *tracker.find_track(7U);
    const PoolSnapshot pool_before = snapshot_of(tracker);

    const auto refused = tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 0.49F));
    ASSERT_TRUE(refused.ok()) << refused.status().message();
    EXPECT_FALSE(refused.value().applied);
    EXPECT_EQ(refused.value().dx, 4.0F);  // the evaluated estimate is echoed
    EXPECT_EQ(refused.value().dy, 2.0F);
    EXPECT_TRUE(refused.value().tracks.empty());
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(pool_before, tracker));
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(before, tracker));

    const auto applied = tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 0.5F));
    ASSERT_TRUE(applied.ok()) << applied.status().message();
    EXPECT_TRUE(applied.value().applied);
    ASSERT_EQ(applied.value().tracks.size(), 1U);
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, (RectF{20.0F, 18.0F, 8.0F, 8.0F}));
}

/// The default gate (0.0) applies every well-formed estimate, confidence 0
/// included — the development smoke default with no real-world prior.
TEST(ObjectTrackerMotionGenerationTest, DefaultGateAppliesZeroConfidenceEstimate) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(image), 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const auto applied = tracker.compensate_global_motion(shift_of(1.5F, 0.5F, 0.0F));
    ASSERT_TRUE(applied.ok()) << applied.status().message();
    EXPECT_TRUE(applied.value().applied);
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, (RectF{17.5F, 16.5F, 8.0F, 8.0F}));
}

/// Malformed estimates are explicit kInvalidArgument failures with the pool
/// completely untouched — including the track that WOULD have stayed finite
/// when a sibling overflows (the plan runs before any mutation across the
/// whole pool).
TEST(ObjectTrackerMotionGenerationTest, CompensationRejectsInvalidEstimatesAtomically) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    const TargetTrack before = *tracker.find_track(7U);
    const PoolSnapshot pool_before = snapshot_of(tracker);

    ShiftEstimate nan_dx = shift_of(0.0F, 0.0F, 1.0F);
    nan_dx.dx = std::nanf("");
    EXPECT_EQ(tracker.compensate_global_motion(nan_dx).status().code(), ErrorCode::kInvalidArgument);

    ShiftEstimate nan_conf = shift_of(1.0F, 1.0F, 1.0F);
    nan_conf.confidence = std::nanf("");
    EXPECT_EQ(tracker.compensate_global_motion(nan_conf).status().code(), ErrorCode::kInvalidArgument);

    EXPECT_EQ(tracker.compensate_global_motion(shift_of(1.0F, 0.0F, -0.01F)).status().code(),
              ErrorCode::kInvalidArgument);
    EXPECT_EQ(tracker.compensate_global_motion(shift_of(1.0F, 0.0F, 1.01F)).status().code(),
              ErrorCode::kInvalidArgument);
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(pool_before, tracker));
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(before, tracker));

    // A finite-but-huge translation succeeds once; repeating it overflows the
    // float range and fails explicitly with the pool untouched.
    const auto huge = tracker.compensate_global_motion(shift_of(2.0e38F, 0.0F, 1.0F));
    ASSERT_TRUE(huge.ok()) << huge.status().message();
    ASSERT_EQ(tracker.find_track(7U)->last_bounds.x, 2.0e38F);
    const TargetTrack huge_before = *tracker.find_track(7U);
    const auto overflow = tracker.compensate_global_motion(shift_of(2.0e38F, 0.0F, 1.0F));
    EXPECT_EQ(overflow.status().code(), ErrorCode::kInvalidArgument);
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(huge_before, tracker));
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);

    // Sibling atomicity: with one track already at the float edge, a shift
    // that the small track would survive must not move EITHER track.
    ObjectTrackerOptions twin_options;
    twin_options.max_targets = 2;
    ObjectTracker twin = make_tracker(twin_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(twin, view, 4U, bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(twin, view, 6U, RectF{30.0F, 30.0F, 8.0F, 8.0F}));
    ASSERT_TRUE(twin.compensate_global_motion(shift_of(2.0e38F, 0.0F, 1.0F)).ok());
    const TargetTrack small_before = *twin.find_track(4U);
    const TargetTrack edge_before = *twin.find_track(6U);
    const auto mixed = twin.compensate_global_motion(shift_of(2.0e38F, 0.0F, 1.0F));
    EXPECT_EQ(mixed.status().code(), ErrorCode::kInvalidArgument);
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(small_before, twin));
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(edge_before, twin));
}

/// Cancellation and deadline: the M7-07 entries poll exactly once at the
/// entry — before validation (the documented entry-poll order of these
/// primitives, in contrast to the validation-first commits) — and convert to
/// explicit statuses with the pool untouched.
TEST(ObjectTrackerMotionGenerationTest, CompensationCancellationAndTimeout) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(image), 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const TargetTrack before = *tracker.find_track(7U);
    const PoolSnapshot pool_before = snapshot_of(tracker);

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);

    const auto cancelled_call = tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 1.0F), cancelled);
    EXPECT_EQ(cancelled_call.status().code(), ErrorCode::kCancelled);
    const auto timed_out = tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 1.0F), expired);
    EXPECT_EQ(timed_out.status().code(), ErrorCode::kTimeout);
    // The entry poll precedes validation on these primitives (frozen M7-07
    // entry order): a cancelled call with a malformed estimate still reports
    // the cancellation.
    ShiftEstimate nan_dx = shift_of(0.0F, 0.0F, 1.0F);
    nan_dx.dx = std::nanf("");
    const auto cancelled_invalid = tracker.compensate_global_motion(nan_dx, cancelled);
    EXPECT_EQ(cancelled_invalid.status().code(), ErrorCode::kCancelled);

    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(pool_before, tracker));
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(before, tracker));
}

/// Empty pools succeed with an empty trace; a zero shift applies as the
/// identity; and the pipeline discipline "one call per measured shift" is the
/// caller's: feeding the same estimate again translates again.
TEST(ObjectTrackerMotionGenerationTest, CompensationEmptyPoolZeroShiftAndRepeatDiscipline) {
    ObjectTracker empty = make_tracker();
    const auto on_empty = empty.compensate_global_motion(shift_of(4.0F, 2.0F, 1.0F));
    ASSERT_TRUE(on_empty.ok()) << on_empty.status().message();
    EXPECT_TRUE(on_empty.value().applied);
    EXPECT_TRUE(on_empty.value().tracks.empty());

    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(image), 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    const auto zero = tracker.compensate_global_motion(shift_of(0.0F, 0.0F, 1.0F));
    ASSERT_TRUE(zero.ok()) << zero.status().message();
    EXPECT_TRUE(zero.value().applied);
    ASSERT_EQ(zero.value().tracks.size(), 1U);
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, (RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    const ShiftEstimate shift = shift_of(2.0F, 1.0F, 1.0F);
    ASSERT_TRUE(tracker.compensate_global_motion(shift).ok());
    ASSERT_TRUE(tracker.compensate_global_motion(shift).ok());
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, (RectF{20.0F, 18.0F, 8.0F, 8.0F}));
}

/// Identical pools fed identical pipelines (trigger, compensation, sweep)
/// produce bitwise-identical results, traces and pool records — and the
/// per-call traces are equal field by field.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest macro expansion dominates the metric
TEST(ObjectTrackerMotionGenerationTest, PipelineIsBitwiseDeterministicAcrossInstances) {
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);

    const auto build = [&]() {
        ObjectTrackerOptions options;
        options.uncertain_frame_limit = 4;
        ObjectTracker tracker = make_tracker(options);
        EXPECT_TRUE(tracker.adopt_track(make_region(4U, RectF{16.0F, 16.0F, 8.0F, 8.0F}), view, 1).ok());
        EXPECT_TRUE(tracker.adopt_track(make_region(2U, RectF{30.0F, 20.0F, 8.0F, 8.0F}), view, 1).ok());
        const auto degraded = tracker.commit_track_evidence(
            4U, verification_of(4U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
            position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
        EXPECT_TRUE(degraded.ok());
        return tracker;
    };
    const auto run = [&](ObjectTracker& tracker) {
        const auto trigger_one = tracker.advance_generation_for_classification(ChangeClassification::kGlobal);
        EXPECT_TRUE(trigger_one.ok());
        const auto trigger_two = tracker.advance_generation_for_classification(ChangeClassification::kGlobal);
        EXPECT_TRUE(trigger_two.ok());
        const auto compensated = tracker.compensate_global_motion(shift_of(2.5F, 1.5F, 0.8F));
        EXPECT_TRUE(compensated.ok());
        const auto swept = tracker.sweep_generation_lag(9);
        EXPECT_TRUE(swept.ok());
        return std::pair<std::pair<GenerationAdvance, MotionCompensationResult>, std::vector<uint64_t>>{
            std::pair<GenerationAdvance, MotionCompensationResult>{trigger_two.value(), compensated.value()},
            swept.value()};
    };

    ObjectTracker first = build();
    ObjectTracker second = build();
    const auto [first_results, first_swept] = run(first);
    const auto [second_results, second_swept] = run(second);
    ASSERT_NO_FATAL_FAILURE(expect_advance_equal(first_results.first, second_results.first));
    ASSERT_NO_FATAL_FAILURE(expect_compensation_equal(first_results.second, second_results.second));
    EXPECT_EQ(first_swept, second_swept);
    EXPECT_EQ(first.track_ids(), second.track_ids());
    for (const uint64_t id : first.track_ids()) {
        ASSERT_NO_FATAL_FAILURE(expect_track_untouched(*first.find_track(id), second));
    }
}

// --- obligation 3: the generation-lag sweep -------------------------------------------

/// The exhaustion boundary is strict: lag == max_generation_lag keeps the
/// track (kUncertain), lag == max_generation_lag + 1 sweeps it to kLost with
/// the id reported — at the default limit and at a custom limit.
TEST(ObjectTrackerMotionGenerationTest, SweepBoundaryIsStrictlyGreaterThanMaxGenerationLag) {
    // Default max_generation_lag = 1.
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const auto degraded =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(degraded.ok()) << degraded.status().message();
    ASSERT_EQ(degraded.value().state, TrackState::kUncertain);

    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    const auto at_limit = tracker.sweep_generation_lag(10);  // lag 1: not exhausted
    ASSERT_TRUE(at_limit.ok()) << at_limit.status().message();
    EXPECT_TRUE(at_limit.value().empty());
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kUncertain);

    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    const auto exhausted = tracker.sweep_generation_lag(11);  // lag 2 > 1: the transition
    ASSERT_TRUE(exhausted.ok()) << exhausted.status().message();
    ASSERT_EQ(exhausted.value().size(), 1U);
    EXPECT_EQ(exhausted.value()[0], 7U);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kLost);

    // Custom max_generation_lag = 3: lag 3 survives, lag 4 sweeps.
    ObjectTrackerOptions options;
    options.max_generation_lag = 3;
    options.uncertain_frame_limit = 8;
    ObjectTracker wide = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(wide, view, 4U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const auto wide_degraded =
        wide.commit_track_evidence(4U, verification_of(4U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                   position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(wide_degraded.ok());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(wide.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    }
    const auto wide_at_limit = wide.sweep_generation_lag(20);
    ASSERT_TRUE(wide_at_limit.ok());
    EXPECT_TRUE(wide_at_limit.value().empty());
    EXPECT_EQ(wide.find_track(4U)->state, TrackState::kUncertain);
    ASSERT_TRUE(wide.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    const auto wide_exhausted = wide.sweep_generation_lag(21);
    ASSERT_TRUE(wide_exhausted.ok());
    EXPECT_EQ(wide_exhausted.value(), (std::vector<uint64_t>{4U}));
    EXPECT_EQ(wide.find_track(4U)->state, TrackState::kLost);
}

/// Both exhaustion conditions must hold: only kUncertain tracks with lag
/// sweep. A lagging kTracking track keeps its confirmed status (its
/// degradation is the M7-06 commit chain), a lagging kLost track stays kLost
/// (sticky), terminated archives are untouched, and the report is ascending.
TEST(ObjectTrackerMotionGenerationTest, SweepTakesOnlyUncertainTracksWithLag) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 8U, RectF{40.0F, 40.0F, 8.0F, 8.0F}));  // -> kUncertain
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 6U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));  // stays kTracking
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 4U, RectF{28.0F, 16.0F, 8.0F, 8.0F}));  // -> kLost
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 2U, RectF{16.0F, 28.0F, 8.0F, 8.0F}));  // -> kTerminated
    const auto uncertain =
        tracker.commit_track_evidence(8U, verification_of(8U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(uncertain.ok());
    ASSERT_EQ(uncertain.value().state, TrackState::kUncertain);
    const auto lost_first =
        tracker.commit_track_evidence(4U, verification_of(4U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(lost_first.ok());
    const auto lost_second =
        tracker.commit_track_evidence(4U, verification_of(4U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 4);
    ASSERT_TRUE(lost_second.ok());
    ASSERT_EQ(lost_second.value().state, TrackState::kLost);
    const auto uncertain_then_terminated =
        tracker.commit_track_evidence(2U, verification_of(2U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 5);
    ASSERT_TRUE(uncertain_then_terminated.ok());
    ASSERT_EQ(uncertain_then_terminated.value().state, TrackState::kUncertain);
    ASSERT_TRUE(tracker.terminate(2U, 6).ok());

    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());  // lag 2 > 1

    const RectF terminated_bounds{16.0F, 28.0F, 8.0F, 8.0F};
    const int64_t bytes_before = tracker.byte_size();
    const auto swept = tracker.sweep_generation_lag(30);
    ASSERT_TRUE(swept.ok()) << swept.status().message();
    EXPECT_EQ(swept.value(), (std::vector<uint64_t>{8U}));  // ascending, single entry
    EXPECT_EQ(tracker.find_track(8U)->state, TrackState::kLost);
    EXPECT_EQ(tracker.find_track(6U)->state, TrackState::kTracking);  // confirmed status kept
    EXPECT_EQ(tracker.find_track(6U)->last_bounds, (RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    EXPECT_EQ(tracker.find_track(4U)->state, TrackState::kLost);  // sticky, not re-reported
    EXPECT_EQ(tracker.find_track(2U)->state, TrackState::kTerminated);
    EXPECT_EQ(tracker.find_track(2U)->last_bounds, terminated_bounds);
    EXPECT_EQ(tracker.byte_size(), bytes_before);  // the state slot is reused, no growth
}

/// The sweep never fabricates evidence and never unlocks recapture: a swept
/// kLost track stays kLost under placeholder commits, and only a confirming
/// grade recaptures it (the kLost -> kTracking edge stays M7-06's).
TEST(ObjectTrackerMotionGenerationTest, SweptLostStaysStickyUntilAConfirmingCommit) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const auto degraded =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(degraded.ok());
    ASSERT_EQ(degraded.value().state, TrackState::kUncertain);
    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    const auto swept = tracker.sweep_generation_lag(10);
    ASSERT_TRUE(swept.ok());
    ASSERT_EQ(swept.value(), (std::vector<uint64_t>{7U}));

    const auto placeholder =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 11);
    ASSERT_TRUE(placeholder.ok()) << placeholder.status().message();
    EXPECT_EQ(placeholder.value().previous_state, TrackState::kLost);
    EXPECT_EQ(placeholder.value().state, TrackState::kLost);  // sticky

    const auto recaptured = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong, 0, 0, 0.95),
        position_of(PositionScenario::kStationary, false), std::nullopt, view, 12);  // stale prior must not gate
    ASSERT_TRUE(recaptured.ok()) << recaptured.status().message();
    EXPECT_EQ(recaptured.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(recaptured.value().previous_state, TrackState::kLost);
    EXPECT_EQ(recaptured.value().state, TrackState::kTracking);
    EXPECT_EQ(tracker.find_track(7U)->layout_generation, 2U);  // stamped to the pool's current
}

TEST(ObjectTrackerMotionGenerationTest, SweepCancellationAndTimeout) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 4;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const auto degraded =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(degraded.ok());
    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());
    ASSERT_TRUE(tracker.advance_generation_for_classification(ChangeClassification::kGlobal).ok());

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);

    const auto cancelled_call = tracker.sweep_generation_lag(10, cancelled);
    EXPECT_EQ(cancelled_call.status().code(), ErrorCode::kCancelled);
    const auto timed_out = tracker.sweep_generation_lag(10, expired);
    EXPECT_EQ(timed_out.status().code(), ErrorCode::kTimeout);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kUncertain);  // nothing swept

    const auto clean = tracker.sweep_generation_lag(10);
    ASSERT_TRUE(clean.ok()) << clean.status().message();
    EXPECT_EQ(clean.value(), (std::vector<uint64_t>{7U}));
}

TEST(ObjectTrackerMotionGenerationTest, SweepWithNothingToSweepReturnsEmpty) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(image), 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const auto empty_pool = tracker.sweep_generation_lag(5);
    ASSERT_TRUE(empty_pool.ok()) << empty_pool.status().message();
    EXPECT_TRUE(empty_pool.value().empty());

    // A confirmed track lagging below the strict boundary is not swept either.
    const auto lagging = tracker.sweep_generation_lag(6);
    ASSERT_TRUE(lagging.ok());
    EXPECT_TRUE(lagging.value().empty());
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);
}

// --- obligation 2: the trigger -> generation-switch state chain ------------------------

/// The pipeline chain frozen across M7-07/M7-06: the trigger advances the
/// counter only; the caller's `kGenerationSwitch` commits drive the per-track
/// degradation (kTracking without confirming evidence -> kUncertain, position
/// prior zeroed) and the recovery on confirming appearance evidence.
TEST(ObjectTrackerMotionGenerationTest, GenerationSwitchChainDegradesAndRecovers) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));

    const auto triggered = tracker.advance_generation_for_classification(ChangeClassification::kGlobal);
    ASSERT_TRUE(triggered.ok()) << triggered.status().message();
    EXPECT_EQ(tracker.layout_generation(), 1U);

    // No confirming evidence under the switch scenario: the kTracking track
    // degrades even with the gate refused (the prior is zeroed).
    const auto degraded =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kGenerationSwitch, false), std::nullopt, view, 2);
    ASSERT_TRUE(degraded.ok()) << degraded.status().message();
    EXPECT_EQ(degraded.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(degraded.value().scenario, PositionScenario::kGenerationSwitch);
    EXPECT_EQ(degraded.value().state, TrackState::kUncertain);
    EXPECT_EQ(tracker.find_track(7U)->layout_generation, 1U);

    // Confirming appearance recovers through the same switch scenario.
    const auto confirmed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kStrong, 0, 0, 0.95),
        position_of(PositionScenario::kGenerationSwitch, false), semantics_of("icon"), view, 3);
    ASSERT_TRUE(confirmed.ok()) << confirmed.status().message();
    EXPECT_EQ(confirmed.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(confirmed.value().state, TrackState::kTracking);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);
}

// --- obligation 5: the scroll round trip -----------------------------------------------

/// The full compensation round trip with the M7-04 estimator in the loop on a
/// scrolled frame pair: BEFORE compensation the stale prior misses the target
/// (a partial report over the true position short-circuits to kReuse, and the
/// verifier only finds the target at the full scroll offset); AFTER
/// compensation the same report enters verification (kVerify), the same-frame
/// check is strong at offset (0, 0), and a `kCompensatedScroll` commit
/// confirms — the position prior regained its static-period validity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): round-trip phases dominate the metric
TEST(ObjectTrackerMotionGenerationTest, ScrollRoundTripRestoresThePositionPrior) {
    const GrayImage frame_a = noise_image(64, 64, 3);
    const GrayImage frame_b = scrolled_frame(frame_a, 8, 4, 200);
    const ImageView view_a = view_of(frame_a);
    const ImageView view_b = view_of(frame_b);

    const auto estimated = mirador::estimate_global_shift(view_a, view_b, mirador::ShiftEstimationParams{});
    ASSERT_TRUE(estimated.ok()) << estimated.status().message();
    EXPECT_EQ(estimated.value().dx, 8.0F);  // 64x64 frames: the mapping is exact
    EXPECT_EQ(estimated.value().dy, 4.0F);
    ASSERT_GT(estimated.value().confidence, 0.5F);

    ObjectTracker tracker = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_a, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}));
    const RectF true_position{24.0F, 20.0F, 8.0F, 8.0F};

    // Pre-compensation: the stale prior short-circuits the partial report,
    // and the (8, 4) scroll outruns the frozen verification ROI expansion
    // (half a bounds diagonal ~ 5.7 px per side), so verification cannot reach
    // the target at all inside the gated neighborhood — the designed
    // uncompensated-scroll structural failure the compensation must undo.
    const ChangeReport report = make_gate_report(ChangeClassification::kPartial, {RectI{24, 20, 8, 8}});
    const auto stale_gate = tracker.evaluate_change_gate(report);
    ASSERT_TRUE(stale_gate.ok()) << stale_gate.status().message();
    ASSERT_EQ(stale_gate.value().tracks.size(), 1U);
    EXPECT_EQ(stale_gate.value().tracks[0].decision, ChangeGateDecision::kReuse);
    EXPECT_FALSE(stale_gate.value().tracks[0].change_region_index.has_value());
    const auto stale_verification = tracker.verify_track(7U, view_b, std::nullopt);
    ASSERT_TRUE(stale_verification.ok()) << stale_verification.status().message();
    EXPECT_EQ(stale_verification.value().appearance.outcome, AppearanceChannelOutcome::kNone);

    // Compensate: every live position estimate moves with the frame.
    const auto compensated = tracker.compensate_global_motion(estimated.value());
    ASSERT_TRUE(compensated.ok()) << compensated.status().message();
    EXPECT_TRUE(compensated.value().applied);
    ASSERT_EQ(compensated.value().tracks.size(), 1U);
    EXPECT_EQ(compensated.value().tracks[0].track_id, 7U);
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, true_position);
    EXPECT_EQ(tracker.find_track(7U)->predicted_center, center_of(true_position));

    // Post-compensation: the same report verifies, the same-frame check peaks
    // at offset (0, 0), and the compensated-scroll commit confirms.
    const auto restored_gate = tracker.evaluate_change_gate(report);
    ASSERT_TRUE(restored_gate.ok()) << restored_gate.status().message();
    ASSERT_EQ(restored_gate.value().tracks.size(), 1U);
    const auto& gate_entry = restored_gate.value().tracks[0];
    EXPECT_EQ(gate_entry.decision, ChangeGateDecision::kVerify);
    ASSERT_TRUE(gate_entry.change_region_index.has_value());
    if (gate_entry.change_region_index.has_value()) {
        EXPECT_EQ(*gate_entry.change_region_index, 0U);
    }
    const auto restored_verification = tracker.verify_track(7U, view_b, std::nullopt);
    ASSERT_TRUE(restored_verification.ok()) << restored_verification.status().message();
    ASSERT_EQ(restored_verification.value().appearance.outcome, AppearanceChannelOutcome::kStrong);
    EXPECT_EQ(restored_verification.value().appearance.best_offset_dx, 0);
    EXPECT_EQ(restored_verification.value().appearance.best_offset_dy, 0);
    EXPECT_GT(restored_verification.value().appearance.peak_ncc, 0.999);
    const auto committed =
        tracker.commit_track_evidence(7U, restored_verification.value(),
                                      position_of(PositionScenario::kCompensatedScroll, true), std::nullopt, view_b, 2);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(committed.value().scenario, PositionScenario::kCompensatedScroll);
    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->state, TrackState::kTracking);
    EXPECT_EQ(track->last_bounds, true_position);  // offset (0, 0): the window stays
    EXPECT_EQ(track->last_verified_sequence, 2U);
}

/// DOD-03 for the compensation path: across 0/90/180/270 rotation metadata x
/// odd-sized frames x non-contiguous stride x flush edges x both shift
/// directions, the translation is metadata-identical, the compensated
/// position verifies strong at offset (0, 0) on the scrolled frame and the
/// `kCompensatedScroll` commit confirms (round trip). Each scenario runs its
/// own pool: the uniform compensation is whole-pool, so track and scrolled
/// frame must agree on one shift.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): matrix cells dominate the metric
TEST(ObjectTrackerMotionGenerationTest, CompensationRotationMatrixOddSizeStrideFlushEdges) {
    const RectF flush_top_left{0.0F, 0.0F, 8.0F, 6.0F};
    const RectF flush_bottom_right{13.0F, 9.0F, 8.0F, 6.0F};
    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        const GrayImage frame_a = noise_image(21, 15, 7);  // odd x odd, +7 stride padding
        const GrayImage frame_down_right = scrolled_frame(frame_a, 3, 2, 200);
        const GrayImage frame_up_left = scrolled_frame(frame_a, -3, -2, 200);
        const ImageView view_a = view_of(frame_a, rotation);
        const ImageView view_b = view_of(frame_down_right, rotation);
        const ImageView view_c = view_of(frame_up_left, rotation);

        // Flush top-left corner, content scrolled down-right by (3, 2).
        ObjectTracker tracker = make_tracker();
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_a, 11U, flush_top_left));
        const auto compensated = tracker.compensate_global_motion(shift_of(3.0F, 2.0F, 1.0F));
        ASSERT_TRUE(compensated.ok()) << "rotation " << static_cast<int>(rotation) << ": "
                                      << compensated.status().message();
        EXPECT_TRUE(compensated.value().applied);
        ASSERT_EQ(compensated.value().tracks.size(), 1U);
        EXPECT_EQ(compensated.value().tracks[0].track_id, 11U);
        EXPECT_EQ(compensated.value().tracks[0].compensated_bounds, (RectF{3.0F, 2.0F, 8.0F, 6.0F}))
            << "rotation " << static_cast<int>(rotation);
        const auto verified = tracker.verify_track(11U, view_b, std::nullopt);
        ASSERT_TRUE(verified.ok()) << "rotation " << static_cast<int>(rotation) << ": " << verified.status().message();
        ASSERT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kStrong)
            << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(verified.value().appearance.best_offset_dx, 0) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(verified.value().appearance.best_offset_dy, 0) << "rotation " << static_cast<int>(rotation);
        const auto committed = tracker.commit_track_evidence(
            11U, verified.value(), position_of(PositionScenario::kCompensatedScroll, true), std::nullopt, view_b, 2);
        ASSERT_TRUE(committed.ok()) << "rotation " << static_cast<int>(rotation) << ": "
                                    << committed.status().message();
        EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(tracker.find_track(11U)->last_bounds, (RectF{3.0F, 2.0F, 8.0F, 6.0F}));

        // Flush bottom-right corner, content scrolled up-left by (-3, -2).
        ObjectTracker reverse_tracker = make_tracker();
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(reverse_tracker, view_a, 13U, flush_bottom_right));
        const auto reverse = reverse_tracker.compensate_global_motion(shift_of(-3.0F, -2.0F, 1.0F));
        ASSERT_TRUE(reverse.ok()) << "rotation " << static_cast<int>(rotation) << ": " << reverse.status().message();
        ASSERT_EQ(reverse_tracker.find_track(13U)->last_bounds, (RectF{10.0F, 7.0F, 8.0F, 6.0F}));
        const auto reverse_verified = reverse_tracker.verify_track(13U, view_c, std::nullopt);
        ASSERT_TRUE(reverse_verified.ok())
            << "rotation " << static_cast<int>(rotation) << ": " << reverse_verified.status().message();
        ASSERT_EQ(reverse_verified.value().appearance.outcome, AppearanceChannelOutcome::kStrong)
            << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(reverse_verified.value().appearance.best_offset_dx, 0) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(reverse_verified.value().appearance.best_offset_dy, 0) << "rotation " << static_cast<int>(rotation);
        const auto reverse_committed = reverse_tracker.commit_track_evidence(
            13U, reverse_verified.value(), position_of(PositionScenario::kCompensatedScroll, true), std::nullopt,
            view_c, 3);
        ASSERT_TRUE(reverse_committed.ok())
            << "rotation " << static_cast<int>(rotation) << ": " << reverse_committed.status().message();
        EXPECT_EQ(reverse_committed.value().grade, EvidenceGrade::kConfirmed)
            << "rotation " << static_cast<int>(rotation);
    }
}

}  // namespace
