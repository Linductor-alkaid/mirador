// M7-06 evidence fusion and the tracking state machine: independent
// verification suite for `ObjectTracker::commit_track_evidence`, the
// `PositionScenario`/`TrackPositionEvidence`/`TrackEvidenceCommit` contract
// face, the state-slot bookkeeping, and the `StableIdTracker::advance`
// `confirmed_associations` DEC-010 gate passthrough (object-tracking design
// sections 3, 4, 6.4, 6.5). Written against the frozen header contracts only:
//   * The four-level grade table (impostor veto > semantic veto > strong
//     appearance with the gate > weak appearance with the gate > placeholder),
//     with the position gate as a necessary condition under DEC-019 section 2
//     and the three-scenario conditioning (stationary / compensated scroll
//     full weight, generation switch zeroed prior).
//   * The four-state machine: kConfirmed/kTentative recapture (kLost ->
//     kTracking as a state-machine rule only), placeholder/vetoed degradation
//     with the commit-counted `uncertain_frame_limit` boundary observable on
//     both sides, kLost stickiness, kTerminated rejection and the caller-driven
//     terminate edge.
//   * The frozen template-collection policies (kConfirmed positive capture,
//     semantic-conflict-only negative capture, impostor collected exactly once)
//     and the bounded stores with explicit eviction counters.
//   * Atomic commits: budget-checked stores, error paths leave the pool
//     completely untouched, validation precedes cancellation (the frozen
//     contrast to adopt_track's cancel-first entry), kCancelled/kTimeout
//     conversion.
//   * The M7-02/M7-03 deferred paths, now constructible: record_observation,
//     evaluate_change_gate and verify_track on kUncertain/kLost tracks.
//   * DOD-03 coordinate matrix (rotations x odd size x non-contiguous stride x
//     flush edge + re-verification round trip), DOD-04 invalidation flips,
//     bitwise determinism, RULE-06 byte accounting of `kStateSlotOverheadBytes`.
//
// The commit trusts the `TrackVerification` it is handed (the documented
// evidence-trust boundary), so the decision-table matrix drives it with
// hand-built channel outcomes; end-to-end tests run the real `verify_track`
// where the E1 pipeline matters (same-frame strong evidence, rotation matrix,
// impostor patch identity).
//
// Privacy (RULE-10/DOD-06) is structural here, as in the M7-03/M7-05 suites:
// `TrackEvidenceCommit` carries only ids, enums and booleans — no template
// bytes, thumbnails or frame content exist in it to leak; the tracker is an
// in-memory decision with no logging, filesystem or network surface; negative
// templates live only in the pool's memory. The shared privacy suite covers
// the fusion pipeline binary.

#include <mirador/object_tracker.hpp>
#include <mirador/stable_id_tracker.hpp>

#include <mirador/change_detection.hpp>
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

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::AppearanceChannelOutcome;
using mirador::ChangeClassification;
using mirador::ChangeGateDecision;
using mirador::ChangeReport;
using mirador::ConfirmedAssociation;
using mirador::ErrorCode;
using mirador::EvidenceGrade;
using mirador::ExecutionContext;
using mirador::IdEvent;
using mirador::ImageView;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PatchFingerprintParams;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::PositionScenario;
using mirador::RectF;
using mirador::RectI;
using mirador::Rotation;
using mirador::StableIdOptions;
using mirador::StableIdReport;
using mirador::StableIdTracker;
using mirador::StructureChannelOutcome;
using mirador::TargetTrack;
using mirador::TrackEvidenceCommit;
using mirador::TrackPositionEvidence;
using mirador::TrackSemantics;
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

/// Noise-like per-pixel pattern (uint32 hashing, deterministic, no UB).
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

/// Deterministic pseudo-random 8x8 block (no periodicity, so distinct blocks
/// decorrelate under the M3-10 NCC).
std::vector<uint8_t> pseudo_block(uint64_t seed) {
    std::vector<uint8_t> block(64U);
    for (size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<uint8_t>((i * seed + seed * seed) % 256U);
    }
    return block;
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

ObjectTracker make_tracker(const ObjectTrackerOptions& options = {}) {
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

TrackSemantics semantics_of(std::string label, std::string text = {}) {
    TrackSemantics semantics;
    semantics.label = std::move(label);
    semantics.text = std::move(text);
    return semantics;
}

/// Hand-built caller evidence for the frozen decision table. The commit is
/// contractually forbidden from re-running the scan, so the channel outcomes
/// are test inputs here, exactly like backend-faked results elsewhere.
TrackVerification verification_of(uint64_t track_id, TrackState state_echo, AppearanceChannelOutcome appearance,
                                  int32_t offset_dx = 0, int32_t offset_dy = 0, double peak_ncc = 0.9,
                                  StructureChannelOutcome structure = StructureChannelOutcome::kNotSupplied) {
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
    verification.structure.outcome = structure;
    return verification;
}

TrackPositionEvidence position_of(PositionScenario scenario, bool inside_gate) {
    TrackPositionEvidence position;
    position.scenario = scenario;
    position.inside_gate = inside_gate;
    return position;
}

/// Observable pool state for purity/atomicity comparisons.
struct PoolSnapshot {
    size_t track_count = 0;
    int64_t used_bytes = 0;
    uint64_t evicted_count = 0;
    uint64_t evicted_observations = 0;
    uint64_t evicted_templates = 0;
    uint64_t evicted_negative_templates = 0;
    uint32_t layout_generation = 0;
    std::vector<uint64_t> ids;
};

PoolSnapshot snapshot_of(const ObjectTracker& tracker) {
    PoolSnapshot snapshot;
    snapshot.track_count = tracker.track_count();
    snapshot.used_bytes = tracker.byte_size();
    snapshot.evicted_count = tracker.evicted_track_count();
    snapshot.evicted_observations = tracker.evicted_observation_count();
    snapshot.evicted_templates = tracker.evicted_template_count();
    snapshot.evicted_negative_templates = tracker.evicted_negative_template_count();
    snapshot.layout_generation = tracker.layout_generation();
    snapshot.ids = tracker.track_ids();
    return snapshot;
}

/// One EXPECT per compared field; compares the pool counters and every
/// evidence field of every held track.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_pool_untouched(const PoolSnapshot& before, const ObjectTracker& tracker) {
    EXPECT_EQ(tracker.track_count(), before.track_count);
    EXPECT_EQ(tracker.byte_size(), before.used_bytes);
    EXPECT_EQ(tracker.evicted_track_count(), before.evicted_count);
    EXPECT_EQ(tracker.evicted_observation_count(), before.evicted_observations);
    EXPECT_EQ(tracker.evicted_template_count(), before.evicted_templates);
    EXPECT_EQ(tracker.evicted_negative_template_count(), before.evicted_negative_templates);
    EXPECT_EQ(tracker.layout_generation(), before.layout_generation);
    EXPECT_EQ(tracker.track_ids(), before.ids);
}

/// Deep per-track comparison for commit error paths (the M7-06 commits move
/// evidence fields, so the pool-level counters alone are not enough).
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
    EXPECT_EQ(track->semantics.label, copy.semantics.label);
    EXPECT_EQ(track->semantics.text, copy.semantics.text);
    EXPECT_EQ(track->position_history.size(), copy.position_history.size());
    ASSERT_EQ(track->templates.size(), copy.templates.size());
    for (size_t i = 0; i < copy.templates.size(); ++i) {
        EXPECT_EQ(track->templates[i].fingerprint, copy.templates[i].fingerprint);
        EXPECT_EQ(track->templates[i].frame_sequence, copy.templates[i].frame_sequence);
        EXPECT_EQ(track->templates[i].layout_generation, copy.templates[i].layout_generation);
        EXPECT_EQ(track->templates[i].capture_grade, copy.templates[i].capture_grade);
    }
    ASSERT_EQ(track->negative_templates.size(), copy.negative_templates.size());
    for (size_t i = 0; i < copy.negative_templates.size(); ++i) {
        EXPECT_EQ(track->negative_templates[i].fingerprint, copy.negative_templates[i].fingerprint);
        EXPECT_EQ(track->negative_templates[i].frame_sequence, copy.negative_templates[i].frame_sequence);
        EXPECT_EQ(track->negative_templates[i].capture_grade, copy.negative_templates[i].capture_grade);
    }
}

/// One EXPECT per compared field of the deterministic commit trace.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per compared field
void expect_commits_bitwise_equal(const TrackEvidenceCommit& lhs, const TrackEvidenceCommit& rhs) {
    EXPECT_EQ(lhs.track_id, rhs.track_id);
    EXPECT_EQ(lhs.previous_state, rhs.previous_state);
    EXPECT_EQ(lhs.state, rhs.state);
    EXPECT_EQ(lhs.grade, rhs.grade);
    EXPECT_EQ(lhs.scenario, rhs.scenario);
    EXPECT_EQ(lhs.impostor_hit, rhs.impostor_hit);
    EXPECT_EQ(lhs.semantics_conflict, rhs.semantics_conflict);
    EXPECT_EQ(lhs.template_captured, rhs.template_captured);
    EXPECT_EQ(lhs.negative_template_captured, rhs.negative_template_captured);
}

ChangeReport make_gate_report(const ChangeClassification classification, std::vector<RectI> regions) {
    ChangeReport report;
    report.classification = classification;
    report.changed_regions = std::move(regions);
    return report;
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

/// Byte account of a minimal track: identity + one adoption template + one
/// observation, empty semantics (thumb side fixed by the options).
int64_t minimal_track_bytes(int32_t thumb_side) {
    return ObjectTracker::kTrackOverheadBytes + ObjectTracker::kTemplateOverheadBytes +
           static_cast<int64_t>(thumb_side) * thumb_side + ObjectTracker::kObservationOverheadBytes;
}

// --- grade decision table: strong appearance confirms -------------------------------

/// End to end through the real verifier: the same frame reproduces the
/// adoption template byte for byte, so E1 is kStrong at offset (0, 0) and the
/// commit grades kConfirmed. The trace echoes the transition and the capture;
/// the confirming bookkeeping updates exactly the documented fields.
TEST(ObjectTrackerEvidenceFusionTest, CommitE1StrongSameFrameConfirmsWithBookkeeping) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1));

    const auto verified = tracker.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    ASSERT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kStrong);

    const auto committed = tracker.commit_track_evidence(
        7U, verified.value(), position_of(PositionScenario::kStationary, true), semantics_of("icon"), view, 2);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().track_id, 7U);
    EXPECT_EQ(committed.value().previous_state, TrackState::kTracking);
    EXPECT_EQ(committed.value().state, TrackState::kTracking);
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(committed.value().scenario, PositionScenario::kStationary);
    EXPECT_FALSE(committed.value().impostor_hit);
    EXPECT_FALSE(committed.value().semantics_conflict);
    EXPECT_TRUE(committed.value().template_captured);
    EXPECT_FALSE(committed.value().negative_template_captured);

    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->state, TrackState::kTracking);
    EXPECT_EQ(track->last_bounds, bounds);  // offset (0, 0): the window does not move
    const PointF adoption_center{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    EXPECT_EQ(track->predicted_center, adoption_center);
    EXPECT_FLOAT_EQ(track->confidence, static_cast<float>(verified.value().appearance.peak_ncc));
    EXPECT_EQ(track->last_verified_sequence, 2U);
    ASSERT_EQ(track->templates.size(), 2U);  // adoption + confirmed capture
    EXPECT_EQ(track->templates[1].frame_sequence, 2U);
    EXPECT_EQ(track->templates[1].capture_grade, EvidenceGrade::kConfirmed);
}

/// Hand-built strong evidence with a non-zero E1 offset: `last_bounds` moves
/// to the candidate window and `predicted_center` stays exactly its center
/// (the frozen invariant until the M7-07 motion model).
TEST(ObjectTrackerEvidenceFusionTest, CommitStrongMovesBoundsToCandidateWindow) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1));

    const auto committed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong, 6, 4, 0.95),
        position_of(PositionScenario::kStationary, true), semantics_of("icon"), view, 3);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed);

    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    const RectF moved{bounds.x + 6.0F, bounds.y + 4.0F, bounds.width, bounds.height};
    EXPECT_EQ(track->last_bounds, moved);
    const PointF moved_center{moved.x + moved.width / 2.0F, moved.y + moved.height / 2.0F};
    EXPECT_EQ(track->predicted_center, moved_center);
    EXPECT_FLOAT_EQ(track->confidence, 0.95F);
    EXPECT_EQ(track->last_verified_sequence, 3U);
}

/// E2-only confirmation (structure consistent, E1 without a candidate): the
/// window has no position of its own, so bounds and confidence keep their
/// prior values while the identity evidence still confirms.
TEST(ObjectTrackerEvidenceFusionTest, CommitE2OnlyConfirmationKeepsBoundsAndConfidence) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1));
    ASSERT_TRUE(tracker.record_structure_baseline(7U, TrackStructureDescriptors{0.5F, 0.5F, 0.5F}, 1).ok());
    const int64_t bytes_with_baseline = tracker.byte_size();

    const auto committed =
        tracker.commit_track_evidence(7U,
                                      verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone, 0, 0,
                                                      0.0, StructureChannelOutcome::kConsistent),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 4);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(committed.value().state, TrackState::kTracking);

    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->last_bounds, bounds);
    EXPECT_FLOAT_EQ(track->confidence, 0.9F);  // the adoption value is kept
    EXPECT_EQ(track->last_verified_sequence, 4U);
    EXPECT_EQ(track->templates.size(), 2U);  // capture from the unchanged-bounds window
    // The E2 baseline slot predates the commit; the only growth is the captured
    // template (128 = 64 overhead + 8x8 thumbnail).
    EXPECT_EQ(tracker.byte_size(), bytes_with_baseline + ObjectTracker::kTemplateOverheadBytes + 64);
}

/// Weak appearance inside the gate grades kTentative: confirming (state
/// recovers, fields move) but never writes templates.
TEST(ObjectTrackerEvidenceFusionTest, CommitWeakInsideGateIsTentativeWithoutTemplateWrites) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1));

    const auto committed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak, 2, 0, 0.7),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 5);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kTentative);
    EXPECT_EQ(committed.value().state, TrackState::kTracking);
    EXPECT_FALSE(committed.value().template_captured);

    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->templates.size(), 1U);  // kTentative never writes templates
    EXPECT_EQ(track->last_bounds, (RectF{18.0F, 16.0F, 8.0F, 8.0F}));
    EXPECT_FLOAT_EQ(track->confidence, 0.7F);
    EXPECT_EQ(track->last_verified_sequence, 5U);
}

/// Appearance without a candidate is the placeholder row: position prior only,
/// no identity claim, the track degrades and no evidence field moves.
TEST(ObjectTrackerEvidenceFusionTest, CommitAppearanceNoneInsideGateIsPlaceholderAndDegrades) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1));
    const TargetTrack before = *tracker.find_track(7U);

    const auto committed =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 6);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(committed.value().previous_state, TrackState::kTracking);
    EXPECT_EQ(committed.value().state, TrackState::kUncertain);
    EXPECT_FALSE(committed.value().template_captured);
    EXPECT_FALSE(committed.value().negative_template_captured);

    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->state, TrackState::kUncertain);
    // The placeholder changes no evidence field (the M7-03 freeze).
    EXPECT_EQ(track->last_bounds, before.last_bounds);
    EXPECT_EQ(track->predicted_center, before.predicted_center);
    EXPECT_EQ(track->confidence, before.confidence);
    EXPECT_EQ(track->last_verified_sequence, before.last_verified_sequence);
    EXPECT_EQ(track->templates.size(), before.templates.size());
    EXPECT_EQ(track->negative_templates.size(), before.negative_templates.size());
}

/// A strong candidate refused by the gate is still only a placeholder: under
/// DEC-019 section 2 the position gate is a necessary condition for identity
/// evidence, so the appearance channel alone cannot confirm.
TEST(ObjectTrackerEvidenceFusionTest, CommitStrongOutsideGateIsPlaceholderNotConfirmed) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    const auto committed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong, 4, 4, 0.95),
        position_of(PositionScenario::kStationary, false), std::nullopt, view, 6);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(committed.value().state, TrackState::kUncertain);
    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->last_bounds, (RectF{16.0F, 16.0F, 8.0F, 8.0F}));  // not moved
}

// --- grade decision table: semantic conflict veto -----------------------------------

/// DEC-010 gate predicate: both labels non-empty and different vetoes; an
/// empty label on either side is vacuously compatible; equal labels are
/// compatible. Confirmed grades in the compatible rows prove the gate passed.
TEST(ObjectTrackerEvidenceFusionTest, CommitSemanticConflictFollowsDec010Predicate) {
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);

    // Conflicting labels veto and degrade.
    ObjectTracker conflict = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(conflict, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    const auto vetoed = conflict.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view, 2);
    ASSERT_TRUE(vetoed.ok()) << vetoed.status().message();
    EXPECT_EQ(vetoed.value().grade, EvidenceGrade::kVetoed);
    EXPECT_TRUE(vetoed.value().semantics_conflict);
    EXPECT_EQ(vetoed.value().state, TrackState::kUncertain);

    // Empty candidate label is vacuously compatible: the same evidence confirms.
    ObjectTracker empty_candidate = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(empty_candidate, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    const auto vacuous = empty_candidate.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak),
        position_of(PositionScenario::kStationary, true), semantics_of(""), view, 2);
    ASSERT_TRUE(vacuous.ok()) << vacuous.status().message();
    EXPECT_EQ(vacuous.value().grade, EvidenceGrade::kTentative);
    EXPECT_FALSE(vacuous.value().semantics_conflict);

    // Empty track label is vacuously compatible too.
    ObjectTracker empty_track = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(empty_track, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, ""));
    const auto compatible = empty_track.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view, 2);
    ASSERT_TRUE(compatible.ok()) << compatible.status().message();
    EXPECT_EQ(compatible.value().grade, EvidenceGrade::kTentative);

    // Equal labels are compatible.
    ObjectTracker equal = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(equal, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    const auto same =
        equal.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong),
                                    position_of(PositionScenario::kStationary, true), semantics_of("icon"), view, 2);
    ASSERT_TRUE(same.ok()) << same.status().message();
    EXPECT_EQ(same.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_FALSE(same.value().semantics_conflict);
}

/// A semantic-conflict veto must not move the track: bounds, center,
/// confidence and the verification sequence all keep their values while the
/// candidate patch is pooled as a negative template.
TEST(ObjectTrackerEvidenceFusionTest, CommitVetoedDoesNotMoveTheTrack) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1, "icon"));
    const TargetTrack before = *tracker.find_track(7U);

    const auto committed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong, 6, 4, 0.95),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view, 2);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kVetoed);
    EXPECT_TRUE(committed.value().semantics_conflict);
    EXPECT_FALSE(committed.value().template_captured);
    EXPECT_TRUE(committed.value().negative_template_captured);

    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->state, TrackState::kUncertain);
    EXPECT_EQ(track->last_bounds, before.last_bounds);
    EXPECT_EQ(track->predicted_center, before.predicted_center);
    EXPECT_EQ(track->confidence, before.confidence);
    EXPECT_EQ(track->last_verified_sequence, before.last_verified_sequence);
    ASSERT_EQ(track->negative_templates.size(), 1U);
    EXPECT_EQ(track->negative_templates[0].frame_sequence, 2U);
    EXPECT_EQ(track->negative_templates[0].capture_grade, EvidenceGrade::kVetoed);
    EXPECT_EQ(track->templates.size(), 1U);
}

// --- grade decision table: impostor negative templates ------------------------------

/// The frozen impostor loop: (1) a semantic-conflict veto collects the
/// candidate patch as a negative template; (2) replaying the same candidate
/// with compatible semantics now hits the stored negative template and vetoes
/// WITHOUT recollecting — an impostor is collected exactly once, at its first
/// rejected confirmation attempt.
TEST(ObjectTrackerEvidenceFusionTest, ImpostorCollectedOnceThenHitsWithoutRecapture) {
    const std::vector<uint8_t> block = pseudo_block(53);
    GrayImage frame_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_b, 24, 20, block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}, 1, "icon"));

    // Commit 1: the candidate (frame B block at +16/+12) conflicts semantically,
    // so the candidate patch is pooled as a negative template.
    const auto first = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak, 16, 12, 0.7),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view_of(frame_b), 2);
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_EQ(first.value().grade, EvidenceGrade::kVetoed);
    EXPECT_FALSE(first.value().impostor_hit);
    EXPECT_TRUE(first.value().semantics_conflict);
    EXPECT_TRUE(first.value().negative_template_captured);
    ASSERT_EQ(tracker.find_track(7U)->negative_templates.size(), 1U);

    // The stored negative template must equal the documented pipeline output
    // for the candidate window (bounds + E1 offset).
    const auto expected = direct_template(view_of(frame_b), RectI{24, 20, 8, 8}, 8);
    ASSERT_TRUE(expected.ok()) << expected.status().message();
    EXPECT_EQ(tracker.find_track(7U)->negative_templates[0].fingerprint, expected.value());

    // Commit 2: the same candidate with compatible semantics now trips the
    // impostor veto against the stored negative template and stores nothing.
    const auto second = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kWeak, 16, 12, 0.7),
        position_of(PositionScenario::kStationary, true), semantics_of("icon"), view_of(frame_b), 3);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value().grade, EvidenceGrade::kVetoed);
    EXPECT_TRUE(second.value().impostor_hit);
    EXPECT_FALSE(second.value().semantics_conflict);
    EXPECT_FALSE(second.value().negative_template_captured);
    ASSERT_EQ(tracker.find_track(7U)->negative_templates.size(), 1U);
}

/// The impostor check runs only when the E1 channel found a candidate: a
/// stored negative template never vetoes an E2-only or appearance-less commit.
TEST(ObjectTrackerEvidenceFusionTest, ImpostorCheckSkippedWithoutCandidate) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    TrackTemplate impostor = track->templates[0];
    impostor.capture_grade = EvidenceGrade::kVetoed;
    ASSERT_TRUE(tracker.add_negative_template(7U, impostor).ok());

    const auto committed =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_FALSE(committed.value().impostor_hit);
    EXPECT_EQ(committed.value().state, TrackState::kUncertain);
}

/// DOD-04 parameter invalidation at the impostor gate: raising
/// `impostor_match_threshold` above the actual NCC of the candidate against
/// the stored negative template disables the veto on identical inputs, so the
/// same candidate confirms instead (two trackers differing only in the
/// threshold; the NCC of the identical-block candidate is near 1, the NCC of a
/// different block sits far below).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): threshold flip scenario dominates the metric
TEST(ObjectTrackerEvidenceFusionTest, ImpostorThresholdChangeFlipsTheVeto) {
    const std::vector<uint8_t> target_block = pseudo_block(53);
    const std::vector<uint8_t> other_block = pseudo_block(211);

    // Frame B carries the target block at the candidate window; frame C carries
    // an unrelated block there. The negative template stored below is the
    // UNRELATED block, so the candidate NCC against it is: ~1.0 for frame C
    // (same block), ~0 for frame B.
    GrayImage frame_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_b, 24, 20, target_block);
    GrayImage frame_c = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_c, 24, 20, other_block);

    const auto build = [&](double impostor_threshold) {
        ObjectTrackerOptions options;
        options.template_thumb_side = 8;
        options.impostor_match_threshold = impostor_threshold;
        ObjectTracker tracker = make_tracker(options);
        const GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
        EXPECT_TRUE(
            tracker.adopt_track(make_region(7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}, 0.9F, "icon"), view_of(frame_a), 1)
                .ok());
        // Pool the unrelated block as the impostor via a semantic-conflict
        // veto against frame C.
        const auto pooled = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak, 16, 12, 0.7),
            position_of(PositionScenario::kStationary, true), semantics_of("button"), view_of(frame_c), 2);
        EXPECT_TRUE(pooled.ok());
        EXPECT_TRUE(pooled.value().negative_template_captured) << "threshold " << impostor_threshold;
        return tracker;
    };

    // Default threshold 0.8: the frame C candidate (same block as the stored
    // negative) is vetoed as an impostor.
    ObjectTracker strict = build(0.8);
    const auto vetoed = strict.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kWeak, 16, 12, 0.7),
        position_of(PositionScenario::kStationary, true), semantics_of("icon"), view_of(frame_c), 3);
    ASSERT_TRUE(vetoed.ok()) << vetoed.status().message();
    EXPECT_EQ(vetoed.value().grade, EvidenceGrade::kVetoed);
    EXPECT_TRUE(vetoed.value().impostor_hit);

    // Threshold pinned at 1.0: the exact-same-block NCC stays above/at the
    // gate only if the implementation guarantees it — the differing-block
    // candidate (frame B) clears any threshold, which is the robust flip this
    // test pins: with a high threshold the frame B candidate is NOT an
    // impostor and confirms as tentative instead.
    ObjectTracker pinned = build(1.0);
    const auto cleared = pinned.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kWeak, 16, 12, 0.7),
        position_of(PositionScenario::kStationary, true), semantics_of("icon"), view_of(frame_b), 3);
    ASSERT_TRUE(cleared.ok()) << cleared.status().message();
    EXPECT_EQ(cleared.value().grade, EvidenceGrade::kTentative);
    EXPECT_FALSE(cleared.value().impostor_hit);
}

TEST(ObjectTrackerEvidenceFusionTest, CreateRejectsImpostorThresholdOutsideUnitRange) {
    ObjectTrackerOptions options;
    options.impostor_match_threshold = -0.1;
    EXPECT_EQ(ObjectTracker::create(options).status().code(), ErrorCode::kInvalidArgument);
    options.impostor_match_threshold = 1.1;
    EXPECT_EQ(ObjectTracker::create(options).status().code(), ErrorCode::kInvalidArgument);
    options.impostor_match_threshold = 0.0;
    EXPECT_TRUE(ObjectTracker::create(options).ok());
    options.impostor_match_threshold = 1.0;
    EXPECT_TRUE(ObjectTracker::create(options).ok());
}

// --- scenario conditioning -----------------------------------------------------------

/// kStationary and kCompensatedScroll carry the same behavioral weight: twin
/// trackers with identical inputs but the two scenarios produce identical
/// grades, transitions and bookkeeping; only the trace scenario echo differs.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): twin-tracker scenario dominates the metric
TEST(ObjectTrackerEvidenceFusionTest, CompensatedScrollBehavesAsStationary) {
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    const auto run = [&](PositionScenario scenario) {
        ObjectTracker tracker = make_tracker();
        EXPECT_TRUE(tracker.adopt_track(make_region(7U, bounds), view, 1).ok());
        const auto first = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
            position_of(scenario, true), std::nullopt, view, 2);
        EXPECT_TRUE(first.ok());
        const auto second = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kStrong, 2, 1, 0.9),
            position_of(scenario, true), std::nullopt, view, 3);
        EXPECT_TRUE(second.ok());
        return std::pair<TrackEvidenceCommit, TrackEvidenceCommit>{first.value(), second.value()};
    };

    const auto [stationary_first, stationary_second] = run(PositionScenario::kStationary);
    const auto [scroll_first, scroll_second] = run(PositionScenario::kCompensatedScroll);
    EXPECT_EQ(scroll_first.grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(scroll_first.state, TrackState::kUncertain);
    EXPECT_EQ(scroll_first.scenario, PositionScenario::kCompensatedScroll);
    EXPECT_EQ(scroll_second.grade, stationary_second.grade);
    EXPECT_EQ(scroll_second.state, stationary_second.state);
    EXPECT_EQ(scroll_second.grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(scroll_second.scenario, PositionScenario::kCompensatedScroll);
}

/// The generation-switch scenario zeroes the position prior: the gate is no
/// longer a necessary condition (appearance + semantics confirm without it),
/// and a kTracking track without confirming evidence degrades (design
/// section 6.4). The stationary contrast row pins the difference.
TEST(ObjectTrackerEvidenceFusionTest, GenerationSwitchZeroesThePositionPrior) {
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    // Strong appearance with the gate refused: stationary keeps the gate as a
    // necessary condition (placeholder), the generation switch voids it
    // (confirmed).
    ObjectTracker stationary = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(stationary, view, 7U, bounds, 1));
    const auto stationary_commit = stationary.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong, 4, 4, 0.95),
        position_of(PositionScenario::kStationary, false), std::nullopt, view, 2);
    ASSERT_TRUE(stationary_commit.ok());
    EXPECT_EQ(stationary_commit.value().grade, EvidenceGrade::kPlaceholder);

    ObjectTracker switched = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(switched, view, 7U, bounds, 1));
    const auto switch_commit = switched.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong, 4, 4, 0.95),
        position_of(PositionScenario::kGenerationSwitch, false), std::nullopt, view, 2);
    ASSERT_TRUE(switch_commit.ok()) << switch_commit.status().message();
    EXPECT_EQ(switch_commit.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(switch_commit.value().scenario, PositionScenario::kGenerationSwitch);
    EXPECT_EQ(switch_commit.value().state, TrackState::kTracking);

    // No confirming evidence under a generation switch: the kTracking track
    // still degrades to kUncertain (position prior alone claims nothing).
    ObjectTracker degraded = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(degraded, view, 7U, bounds, 1));
    const auto degraded_commit =
        degraded.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                       position_of(PositionScenario::kGenerationSwitch, false), std::nullopt, view, 2);
    ASSERT_TRUE(degraded_commit.ok());
    EXPECT_EQ(degraded_commit.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(degraded_commit.value().state, TrackState::kUncertain);

    // Weak appearance inside a void gate grades kTentative: confirming.
    ObjectTracker weak = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(weak, view, 7U, bounds, 1));
    const auto weak_commit =
        weak.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak),
                                   position_of(PositionScenario::kGenerationSwitch, false), std::nullopt, view, 2);
    ASSERT_TRUE(weak_commit.ok());
    EXPECT_EQ(weak_commit.value().grade, EvidenceGrade::kTentative);
}

// --- state machine --------------------------------------------------------------------

/// The `uncertain_frame_limit` boundary is observable on both sides: the
/// (L-1)-th consecutive insufficient commit leaves the track kUncertain, the
/// L-th transitions it to kLost. With limit 3: commits 1 and 2 keep
/// kUncertain, commit 3 flips to kLost.
TEST(ObjectTrackerEvidenceFusionTest, UncertainFrameLimitBoundaryObservableOnBothSides) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 3;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    const auto first =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().state, TrackState::kUncertain);

    const auto second =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().previous_state, TrackState::kUncertain);
    EXPECT_EQ(second.value().state, TrackState::kUncertain);  // the (L-1)-th: still kUncertain

    const auto third =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 4);
    ASSERT_TRUE(third.ok());
    EXPECT_EQ(third.value().previous_state, TrackState::kUncertain);
    EXPECT_EQ(third.value().state, TrackState::kLost);  // the L-th: the transition
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kLost);
}

/// The limit counts COMMITS, not wall-clock frames: two insufficient commits
/// carrying the same frame_sequence still exhaust a limit of 2.
TEST(ObjectTrackerEvidenceFusionTest, LimitCountsCommitsNotFrames) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    const auto first =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 7);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().state, TrackState::kUncertain);
    const auto second =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 7);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().state, TrackState::kLost);
}

/// A confirming commit resets the consecutive-insufficient counter: after two
/// placeholders and a confirmation (limit 3), two more placeholders still
/// leave the track kUncertain — only the third degrades it to kLost.
TEST(ObjectTrackerEvidenceFusionTest, ConfirmingCommitResetsTheInsufficientStreak) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 3;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    for (const uint64_t frame : {2U, 3U}) {
        const auto placeholder = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
            position_of(PositionScenario::kStationary, true), std::nullopt, view, frame);
        ASSERT_TRUE(placeholder.ok());
    }
    const auto confirmed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kStrong, 0, 0, 0.9),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 4);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_EQ(confirmed.value().state, TrackState::kTracking);

    const auto fourth =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 5);
    ASSERT_TRUE(fourth.ok());
    EXPECT_EQ(fourth.value().state, TrackState::kUncertain);
    const auto fifth =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 6);
    ASSERT_TRUE(fifth.ok());
    EXPECT_EQ(fifth.value().state, TrackState::kUncertain);  // streak reset observable
    const auto sixth =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 7);
    ASSERT_TRUE(sixth.ok());
    EXPECT_EQ(sixth.value().state, TrackState::kLost);
}

/// Placeholder and vetoed commits both count toward the limit: a limit of 2
/// with a veto in the middle still lands kLost on the second insufficient
/// commit.
TEST(ObjectTrackerEvidenceFusionTest, VetoedCommitsCountTowardTheLimit) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 2;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));

    const auto vetoed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view, 2);
    ASSERT_TRUE(vetoed.ok());
    EXPECT_EQ(vetoed.value().grade, EvidenceGrade::kVetoed);
    EXPECT_EQ(vetoed.value().state, TrackState::kUncertain);

    const auto placeholder =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(placeholder.ok());
    EXPECT_EQ(placeholder.value().state, TrackState::kLost);
}

/// Loss is sticky: a kLost track stays kLost under placeholder and vetoed
/// commits (the insufficient counter no longer moves it), while a confirming
/// grade recaptures it — the kLost -> kTracking state-machine rule, with the
/// gate void so the stale prior cannot refuse the recapture.
TEST(ObjectTrackerEvidenceFusionTest, LostIsStickyUntilAConfirmingGrade) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 1;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));

    const auto lost =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(lost.ok());
    EXPECT_EQ(lost.value().state, TrackState::kLost);

    // Placeholder keeps kLost; the confirmation below recaptures.
    const auto still_lost =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(still_lost.ok());
    EXPECT_EQ(still_lost.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(still_lost.value().previous_state, TrackState::kLost);
    EXPECT_EQ(still_lost.value().state, TrackState::kLost);

    // Recapture with the gate refused: a kLost track's stale prior must not
    // gate it (design section 7 — the M7-08 identity review is appearance
    // evidence).
    const auto recaptured = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong, 2, 2, 0.9),
        position_of(PositionScenario::kStationary, false), std::nullopt, view, 4);
    ASSERT_TRUE(recaptured.ok()) << recaptured.status().message();
    EXPECT_EQ(recaptured.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(recaptured.value().previous_state, TrackState::kLost);
    EXPECT_EQ(recaptured.value().state, TrackState::kTracking);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);
    EXPECT_EQ(tracker.find_track(7U)->last_verified_sequence, 4U);
}

/// A vetoed commit on a kLost track does not recapture: the excluded candidate
/// leaves the appearance channel with nothing to confirm, so the track stays
/// kLost (stickiness under vetoes).
TEST(ObjectTrackerEvidenceFusionTest, VetoedCommitKeepsLostTrackLost) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 1;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));

    const auto lost =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(lost.ok());
    ASSERT_EQ(lost.value().state, TrackState::kLost);

    const auto vetoed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kLost, AppearanceChannelOutcome::kStrong),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view, 3);
    ASSERT_TRUE(vetoed.ok());
    EXPECT_EQ(vetoed.value().grade, EvidenceGrade::kVetoed);
    EXPECT_EQ(vetoed.value().state, TrackState::kLost);
}

/// The caller-driven kLost -> kTerminated edge: terminating a kLost track
/// archives it with the identity record only (templates, negative templates,
/// history, baseline and the state slot released), and every further evidence
/// commit is rejected — failure stays visible, never silently dropped.
TEST(ObjectTrackerEvidenceFusionTest, TerminateFromLostArchivesAndRejectsFurtherEvidence) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 1;
    options.template_thumb_side = 8;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    ASSERT_TRUE(tracker.record_structure_baseline(7U, TrackStructureDescriptors{0.5F, 0.5F, 0.5F}, 1).ok());

    const auto lost =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(lost.ok());
    ASSERT_EQ(lost.value().state, TrackState::kLost);

    ASSERT_TRUE(tracker.terminate(7U, 9).ok());
    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->state, TrackState::kTerminated);
    EXPECT_EQ(track->terminated_sequence, 9U);
    EXPECT_TRUE(track->templates.empty());
    EXPECT_TRUE(track->negative_templates.empty());
    EXPECT_TRUE(track->position_history.empty());
    // Identity record + semantics only: the state slot went with the archive.
    EXPECT_EQ(tracker.byte_size(),
              ObjectTracker::kTrackOverheadBytes + static_cast<int64_t>(track->semantics.label.size()));

    const auto rejected = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTerminated, AppearanceChannelOutcome::kStrong),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 10);
    EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument);
}

/// Every commit advances the track's `layout_generation` to the pool's current
/// generation — the track has lived through the switch, confirming or not.
TEST(ObjectTrackerEvidenceFusionTest, EveryCommitAdvancesTheTrackLayoutGeneration) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    ASSERT_EQ(tracker.advance_layout_generation().value(), 1U);

    const auto placeholder =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(placeholder.ok());
    EXPECT_EQ(tracker.find_track(7U)->layout_generation, 1U);

    ASSERT_EQ(tracker.advance_layout_generation().value(), 2U);
    const auto confirmed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kStrong),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_EQ(tracker.find_track(7U)->layout_generation, 2U);
    // The observations recorded before the switch keep their generation.
    EXPECT_EQ(tracker.observations_in_generation(7U, 0U).size(), 1U);
    EXPECT_EQ(tracker.observations_in_generation(7U, 2U).size(), 0U);
}

// --- state bookkeeping: RULE-06 accounting -------------------------------------------

/// The state slot costs exactly `kStateSlotOverheadBytes` once: allocated on
/// the first insufficient commit, kept (zeroed) through the confirming reset,
/// not re-charged by later insufficient commits, released by terminate.
TEST(ObjectTrackerEvidenceFusionTest, StateSlotAccountingIsExactAndReleased) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_templates = 1;  // update policy off: captures must not move the bytes here
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    const int64_t adoption_bytes = minimal_track_bytes(8);
    ASSERT_EQ(tracker.byte_size(), adoption_bytes);

    // First insufficient commit allocates the slot.
    const auto first =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(tracker.byte_size(), adoption_bytes + ObjectTracker::kStateSlotOverheadBytes);

    // Confirming commit resets the streak; the slot stays (contract: released
    // by terminate/eviction/reset only).
    const auto confirmed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kStrong),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_EQ(tracker.byte_size(), adoption_bytes + ObjectTracker::kStateSlotOverheadBytes);

    // Later insufficient commits reuse the slot: no new charge.
    const auto second =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 4);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(tracker.byte_size(), adoption_bytes + ObjectTracker::kStateSlotOverheadBytes);

    // Repeated insufficient commits never grow the pool (RULE-06: no unbounded
    // state-advance growth).
    for (uint64_t frame = 5; frame < 12; ++frame) {
        const auto repeated = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
            position_of(PositionScenario::kStationary, true), std::nullopt, view, frame);
        ASSERT_TRUE(repeated.ok());
        EXPECT_EQ(tracker.byte_size(), adoption_bytes + ObjectTracker::kStateSlotOverheadBytes);
    }

    ASSERT_TRUE(tracker.terminate(7U, 12).ok());
    EXPECT_EQ(tracker.byte_size(), ObjectTracker::kTrackOverheadBytes);
}

/// An evicted track's state slot goes with it: a pool pinned to the exact
/// adoption bytes frees the victim's slot so the next adoption fits.
TEST(ObjectTrackerEvidenceFusionTest, TrackEvictionReleasesTheStateSlot) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_targets = 1;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 1U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    const auto lost =
        tracker.commit_track_evidence(1U, verification_of(1U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(lost.ok());
    ASSERT_EQ(tracker.byte_size(), minimal_track_bytes(8) + ObjectTracker::kStateSlotOverheadBytes);

    const auto adopted = tracker.adopt_track(make_region(2U, RectF{16.0F, 16.0F, 8.0F, 8.0F}), view, 3);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    EXPECT_EQ(adopted.value().evicted_track_ids, (std::vector<uint64_t>{1U}));
    EXPECT_EQ(tracker.byte_size(), minimal_track_bytes(8));
    EXPECT_EQ(tracker.evicted_track_count(), 1U);
}

/// The whole commit is atomic: a planned slot allocation that cannot fit the
/// pool budget fails with kBudgetExceeded and leaves the pool — counters and
/// every track field — completely untouched.
TEST(ObjectTrackerEvidenceFusionTest, BudgetExactPoolRejectsSlotAllocationAtomically) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);

    // Zero headroom: the track fits, the 16-byte slot does not.
    ObjectTrackerOptions exact_options = options;
    exact_options.pool_budget_bytes = minimal_track_bytes(8);
    ObjectTracker exact = make_tracker(exact_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(exact, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_EQ(exact.byte_size(), exact_options.pool_budget_bytes);
    const PoolSnapshot before = snapshot_of(exact);
    const TargetTrack track_before = *exact.find_track(7U);

    const auto rejected =
        exact.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                    position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(before, exact));
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(track_before, exact));
    EXPECT_EQ(exact.find_track(7U)->state, TrackState::kTracking);

    // One slot of headroom: the same commit succeeds and the track degrades.
    ObjectTrackerOptions roomy_options = options;
    roomy_options.pool_budget_bytes = minimal_track_bytes(8) + ObjectTracker::kStateSlotOverheadBytes;
    ObjectTracker roomy = make_tracker(roomy_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(roomy, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    const auto accepted =
        roomy.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                    position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(accepted.ok()) << accepted.status().message();
    EXPECT_EQ(accepted.value().state, TrackState::kUncertain);
    EXPECT_EQ(roomy.byte_size(), roomy_options.pool_budget_bytes);
}

/// Positive captures run the bounded template store: the second confirmed
/// capture evicts the oldest non-initial template (the adoption template at
/// index 0 is pinned) and counts the eviction explicitly.
TEST(ObjectTrackerEvidenceFusionTest, PositiveCapturesEvictOldestNonInitialExplicitly) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_templates = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    const auto first =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value().template_captured);
    EXPECT_EQ(tracker.evicted_template_count(), 0U);
    const VisualPatchFingerprint first_capture = tracker.find_track(7U)->templates[1].fingerprint;

    const auto second = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong, 2, 0, 0.85),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(second.value().template_captured);
    EXPECT_EQ(tracker.evicted_template_count(), 1U);
    ASSERT_EQ(tracker.find_track(7U)->templates.size(), 2U);
    // Index 0 is the pinned adoption template; the evicted entry was the first
    // capture, so the stored entry is the NEW patch (window moved by (2, 0)).
    const auto second_capture = direct_template(view_of(image), RectI{18, 16, 8, 8}, 8);
    ASSERT_TRUE(second_capture.ok());
    EXPECT_EQ(tracker.find_track(7U)->templates[1].fingerprint, second_capture.value());
    EXPECT_NE(tracker.find_track(7U)->templates[1].fingerprint, first_capture);
    // The full-set swap is byte-neutral: only one capture slot is held.
    EXPECT_EQ(tracker.byte_size(), minimal_track_bytes(8) + ObjectTracker::kTemplateOverheadBytes + 64);
}

/// With `max_templates == 1` the update policy is off: the confirmed commit
/// succeeds without a capture.
TEST(ObjectTrackerEvidenceFusionTest, MaxTemplatesOneDisablesPositiveCapture) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_templates = 1;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    const auto committed =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(committed.ok()) << committed.status().message();
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_FALSE(committed.value().template_captured);
    ASSERT_EQ(tracker.find_track(7U)->templates.size(), 1U);
    EXPECT_EQ(tracker.byte_size(), minimal_track_bytes(8));
}

/// With `max_negative_templates == 0` the negative store is off: the conflict
/// veto still fires but nothing is pooled.
TEST(ObjectTrackerEvidenceFusionTest, MaxNegativeTemplatesZeroDisablesNegativeCapture) {
    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_negative_templates = 0;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));

    const auto committed = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view, 2);
    ASSERT_TRUE(committed.ok());
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kVetoed);
    EXPECT_FALSE(committed.value().negative_template_captured);
    EXPECT_TRUE(tracker.find_track(7U)->negative_templates.empty());
}

/// Negative captures run the bounded negative store with explicit eviction:
/// two conflicting impostors over a one-slot store evict the first.
TEST(ObjectTrackerEvidenceFusionTest, NegativeCapturesEvictOldestExplicitly) {
    const std::vector<uint8_t> first_block = pseudo_block(53);
    const std::vector<uint8_t> second_block = pseudo_block(211);
    GrayImage frame_b = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_b, 24, 20, first_block);
    GrayImage frame_c = make_gray_image(64, 64, std::byte{40});
    write_gray_block(frame_c, 24, 20, second_block);

    ObjectTrackerOptions options;
    options.template_thumb_side = 8;
    options.max_negative_templates = 1;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage frame_a = make_gray_image(64, 64, std::byte{40});
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view_of(frame_a), 7U, RectF{8.0F, 8.0F, 8.0F, 8.0F}, 1, "icon"));

    const auto first_veto = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak, 16, 12, 0.7),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view_of(frame_b), 2);
    ASSERT_TRUE(first_veto.ok());
    ASSERT_TRUE(first_veto.value().negative_template_captured);
    EXPECT_EQ(tracker.evicted_negative_template_count(), 0U);

    const auto second_veto = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kWeak, 16, 12, 0.7),
        position_of(PositionScenario::kStationary, true), semantics_of("button"), view_of(frame_c), 3);
    ASSERT_TRUE(second_veto.ok());
    // A different patch: no impostor hit, so the conflict veto collects it and
    // the one-slot store evicts the first impostor.
    EXPECT_FALSE(second_veto.value().impostor_hit);
    EXPECT_TRUE(second_veto.value().negative_template_captured);
    EXPECT_EQ(tracker.evicted_negative_template_count(), 1U);
    ASSERT_EQ(tracker.find_track(7U)->negative_templates.size(), 1U);
    const auto second_capture = direct_template(view_of(frame_c), RectI{24, 20, 8, 8}, 8);
    ASSERT_TRUE(second_capture.ok());
    EXPECT_EQ(tracker.find_track(7U)->negative_templates[0].fingerprint, second_capture.value());
}

// --- DOD-04: baseline/template evolution invalidates existing grades -----------------

/// Re-recording the E2 baseline re-binds the comparison, so the same frame's
/// grade flips: with the stale baseline the E2 channel deviates (no E2-only
/// confirmation), after the caller re-records the adoption-frame proposal the
/// identical evidence confirms.
TEST(ObjectTrackerEvidenceFusionTest, BaselineChangeFlipsTheGrade) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = make_gray_image(64, 64, std::byte{40});
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_TRUE(tracker.record_structure_baseline(7U, TrackStructureDescriptors{0.5F, 0.5F, 0.5F}, 1).ok());

    const auto deviation =
        tracker.commit_track_evidence(7U,
                                      verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone, 0, 0,
                                                      0.0, StructureChannelOutcome::kDeviated),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(deviation.ok());
    EXPECT_EQ(deviation.value().grade, EvidenceGrade::kPlaceholder);  // no E2 confirmation
    EXPECT_EQ(deviation.value().state, TrackState::kUncertain);

    // The caller's baseline policy re-records on an identity-confirmed frame;
    // the identical channel evidence now confirms.
    ASSERT_TRUE(tracker.record_structure_baseline(7U, TrackStructureDescriptors{0.9F, 0.5F, 0.5F}, 2).ok());
    const auto confirmed =
        tracker.commit_track_evidence(7U,
                                      verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone, 0, 0,
                                                      0.0, StructureChannelOutcome::kConsistent),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_EQ(confirmed.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(confirmed.value().state, TrackState::kTracking);
}

/// DOD-04 at the verify/commit seam: two trackers identical except for
/// `ncc_strong_threshold` turn the same frames into different grades (the
/// NCC sits inside the weak band), so no grade is silently reused across
/// parameter differences. End to end through the real verifier.
TEST(ObjectTrackerEvidenceFusionTest, NccThresholdChangeFlipsTheGradeEndToEnd) {
    // 16x16 whole-view tracks: exactly one offset, and the frame pair's NCC
    // lands inside the weak band (recomputed below from the M3-10 formula).
    GrayImage frame_a = make_gray_image(16, 16, std::byte{0});
    fill_gray_rect(frame_a, RectI{14, 14, 2, 2}, 255);
    GrayImage frame_b = make_gray_image(16, 16, std::byte{0});
    fill_gray_rect(frame_b, RectI{0, 0, 2, 2}, 255);
    fill_gray_rect(frame_b, RectI{14, 14, 2, 2}, 255);

    const auto template_a = direct_template(view_of(frame_a), RectI{0, 0, 16, 16}, 8);
    const auto template_b = direct_template(view_of(frame_b), RectI{0, 0, 16, 16}, 8);
    ASSERT_TRUE(template_a.ok());
    ASSERT_TRUE(template_b.ok());
    // Sanity: the pair is inside the weak band for the default thresholds.
    double query_mean = 0.0;
    double entry_mean = 0.0;
    for (size_t i = 0; i < template_a.value().thumbnail_gray.size(); ++i) {
        query_mean += std::to_integer<uint8_t>(template_b.value().thumbnail_gray[i]);
        entry_mean += std::to_integer<uint8_t>(template_a.value().thumbnail_gray[i]);
    }
    query_mean /= static_cast<double>(template_a.value().thumbnail_gray.size());
    entry_mean /= static_cast<double>(template_a.value().thumbnail_gray.size());
    double covariance = 0.0;
    double query_variance = 0.0;
    double entry_variance = 0.0;
    for (size_t i = 0; i < template_a.value().thumbnail_gray.size(); ++i) {
        const double q = std::to_integer<uint8_t>(template_b.value().thumbnail_gray[i]) - query_mean;
        const double e = std::to_integer<uint8_t>(template_a.value().thumbnail_gray[i]) - entry_mean;
        covariance += q * e;
        query_variance += q * q;
        entry_variance += e * e;
    }
    const double ncc = covariance / std::sqrt(query_variance * entry_variance);
    ASSERT_GT(ncc, 0.6);
    ASSERT_LT(ncc, 0.8);

    ObjectTracker weak_tracker = make_tracker();  // defaults: strong at 0.8
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(weak_tracker, view_of(frame_a), 7U, RectF{0.0F, 0.0F, 16.0F, 16.0F}, 1));
    const auto weak_verification = weak_tracker.verify_track(7U, view_of(frame_b), std::nullopt);
    ASSERT_TRUE(weak_verification.ok());
    ASSERT_EQ(weak_verification.value().appearance.outcome, AppearanceChannelOutcome::kWeak);
    const auto weak_commit = weak_tracker.commit_track_evidence(7U, weak_verification.value(),
                                                                position_of(PositionScenario::kStationary, true),
                                                                std::nullopt, view_of(frame_b), 2);
    ASSERT_TRUE(weak_commit.ok());
    EXPECT_EQ(weak_commit.value().grade, EvidenceGrade::kTentative);

    ObjectTrackerOptions strong_options;
    strong_options.ncc_strong_threshold = 0.65;
    ObjectTracker strong_tracker = make_tracker(strong_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(strong_tracker, view_of(frame_a), 7U, RectF{0.0F, 0.0F, 16.0F, 16.0F}, 1));
    const auto strong_verification = strong_tracker.verify_track(7U, view_of(frame_b), std::nullopt);
    ASSERT_TRUE(strong_verification.ok());
    ASSERT_EQ(strong_verification.value().appearance.outcome, AppearanceChannelOutcome::kStrong);
    const auto strong_commit = strong_tracker.commit_track_evidence(7U, strong_verification.value(),
                                                                    position_of(PositionScenario::kStationary, true),
                                                                    std::nullopt, view_of(frame_b), 2);
    ASSERT_TRUE(strong_commit.ok());
    EXPECT_EQ(strong_commit.value().grade, EvidenceGrade::kConfirmed);
}

// --- validation, cancellation, atomicity ---------------------------------------------

/// Validation precedes cancellation (the frozen M7-06 order, the deliberate
/// contrast to adopt_track's M7-02 cancel-first entry): malformed calls report
/// kInvalidArgument even under a cancelled context, only fully valid calls
/// report the cancellation, and an expired deadline reports kTimeout.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one assert block per precedence case
TEST(ObjectTrackerEvidenceFusionTest, CommitValidationPrecedesCancellation) {
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);

    ObjectTracker tracker = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1));

    const auto verification = verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong);
    const auto position = position_of(PositionScenario::kStationary, true);

    // Malformed + cancelled: validation wins.
    const auto unknown = tracker.commit_track_evidence(99U, verification, position, std::nullopt, view, 2, cancelled);
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    ASSERT_TRUE(tracker.terminate(7U, 8).ok());
    const auto terminated = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTerminated, AppearanceChannelOutcome::kStrong), position, std::nullopt,
        view, 9, cancelled);
    EXPECT_EQ(terminated.status().code(), ErrorCode::kInvalidArgument);

    // Valid + cancelled / expired: explicit conversion, pool untouched.
    ObjectTracker live = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(live, view, 7U, bounds, 1));
    const PoolSnapshot before = snapshot_of(live);
    const TargetTrack track_before = *live.find_track(7U);
    const auto cancelled_call =
        live.commit_track_evidence(7U, verification, position, std::nullopt, view, 2, cancelled);
    EXPECT_EQ(cancelled_call.status().code(), ErrorCode::kCancelled);
    const auto timed_out = live.commit_track_evidence(7U, verification, position, std::nullopt, view, 2, expired);
    EXPECT_EQ(timed_out.status().code(), ErrorCode::kTimeout);
    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(before, live));
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(track_before, live));

    // The deliberate contrast: adopt_track polls cancellation first, so an
    // invalid region under a cancelled context reports kCancelled.
    const VisualRegion invalid_region = make_region(8U, RectF{0.0F, 0.0F, 0.0F, 0.0F});
    const auto adopt_cancelled = live.adopt_track(invalid_region, view, 2, cancelled);
    EXPECT_EQ(adopt_cancelled.status().code(), ErrorCode::kCancelled);

    // A valid commit still succeeds afterwards.
    const auto ok = live.commit_track_evidence(7U, verification, position, std::nullopt, view, 2);
    ASSERT_TRUE(ok.ok()) << ok.status().message();
}

/// Every commit error path leaves the pool completely untouched: unknown id,
/// foreign verification id, terminated track, invalid view and a candidate
/// window covering no pixel of the view.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one assert block per error case
TEST(ObjectTrackerEvidenceFusionTest, CommitErrorPathsLeaveThePoolUntouched) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1, "icon"));
    const TargetTrack track_before = *tracker.find_track(7U);
    const PoolSnapshot before = snapshot_of(tracker);
    const auto position = position_of(PositionScenario::kStationary, true);

    const auto unknown = tracker.commit_track_evidence(
        99U, verification_of(99U, TrackState::kTracking, AppearanceChannelOutcome::kStrong), position, std::nullopt,
        view, 2);
    EXPECT_EQ(unknown.status().code(), ErrorCode::kInvalidArgument);

    const auto foreign =
        tracker.commit_track_evidence(7U, verification_of(8U, TrackState::kTracking, AppearanceChannelOutcome::kStrong),
                                      position, std::nullopt, view, 2);
    EXPECT_EQ(foreign.status().code(), ErrorCode::kInvalidArgument);

    ImageView null_data;
    null_data.width = 64;
    null_data.height = 64;
    null_data.row_stride_bytes = 64;
    null_data.format = PixelFormat::kGray8;
    const auto invalid_view =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong),
                                      position, std::nullopt, null_data, 2);
    EXPECT_EQ(invalid_view.status().code(), ErrorCode::kInvalidArgument);

    // Candidate window (bounds + offset) pushed entirely off the view.
    const auto off_view = tracker.commit_track_evidence(
        7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong, 500, 500), position,
        std::nullopt, view, 2);
    EXPECT_EQ(off_view.status().code(), ErrorCode::kInvalidArgument);

    ASSERT_NO_FATAL_FAILURE(expect_pool_untouched(before, tracker));
    ASSERT_NO_FATAL_FAILURE(expect_track_untouched(track_before, tracker));

    // A valid commit still succeeds afterwards.
    const auto ok =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kStrong),
                                      position, std::nullopt, view, 2);
    ASSERT_TRUE(ok.ok()) << ok.status().message();
}

// --- determinism -----------------------------------------------------------------------

/// Identical inputs produce bit-identical commits and track records across
/// repeats and twin instances (no wall-clock, fixed evaluation order).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): scenario sequence dominates the metric
TEST(ObjectTrackerEvidenceFusionTest, CommitSequenceIsBitwiseDeterministic) {
    const GrayImage image = noise_image(64, 64);
    const GrayImage frame_b = noise_image(64, 64, 0);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    const auto build = [&]() {
        ObjectTracker tracker = make_tracker();
        EXPECT_TRUE(tracker.adopt_track(make_region(7U, bounds, 0.9F, "icon"), view_of(image), 1).ok());
        return tracker;
    };
    const auto run = [&](ObjectTracker& tracker) {
        std::vector<TrackEvidenceCommit> commits;
        const auto one = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
            position_of(PositionScenario::kStationary, true), std::nullopt, view_of(image), 2);
        commits.push_back(one.value());
        const auto two = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kStrong, 3, 1, 0.92),
            position_of(PositionScenario::kGenerationSwitch, false), std::nullopt, view_of(image), 3);
        commits.push_back(two.value());
        const auto three = tracker.commit_track_evidence(
            7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kWeak, 2, 2, 0.7),
            position_of(PositionScenario::kStationary, true), semantics_of("button"), view_of(frame_b), 4);
        commits.push_back(three.value());
        return commits;
    };

    ObjectTracker first = build();
    ObjectTracker second = build();
    const auto first_commits = run(first);
    const auto second_commits = run(second);
    ASSERT_EQ(first_commits.size(), 3U);
    for (size_t i = 0; i < first_commits.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(expect_commits_bitwise_equal(first_commits[i], second_commits[i]));
    }
    // The resulting pool records are identical too.
    EXPECT_EQ(first.track_ids(), second.track_ids());
    for (const uint64_t id : first.track_ids()) {
        ASSERT_NO_FATAL_FAILURE(expect_track_untouched(*first.find_track(id), second));
    }
}

// --- DOD-03 coordinate matrix ----------------------------------------------------------

/// Adoption + verification + confirmed commit across all four rotation
/// metadata values on an odd-sized view with non-contiguous stride: the single
/// presented coordinate space keeps the verdicts and bookkeeping identical,
/// and the confirmed track immediately re-verifies (round trip).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest ASSERT_* macro expansion dominates the metric
TEST(ObjectTrackerEvidenceFusionTest, CommitRotationMatrixOddSizeNonContiguousStride) {
    const RectF bounds{5.0F, 4.0F, 8.0F, 6.0F};
    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        ObjectTracker tracker = make_tracker();
        const GrayImage image = noise_image(21, 15, 7);
        const ImageView view = view_of(image, rotation);
        ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 9U, bounds));

        const auto verified = tracker.verify_track(9U, view, std::nullopt);
        ASSERT_TRUE(verified.ok()) << "rotation " << static_cast<int>(rotation) << ": " << verified.status().message();
        ASSERT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kStrong)
            << "rotation " << static_cast<int>(rotation);

        const auto committed = tracker.commit_track_evidence(
            9U, verified.value(), position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
        ASSERT_TRUE(committed.ok()) << "rotation " << static_cast<int>(rotation) << ": "
                                    << committed.status().message();
        EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed) << "rotation " << static_cast<int>(rotation);
        const TargetTrack* track = tracker.find_track(9U);
        ASSERT_NE(track, nullptr);
        EXPECT_EQ(track->state, TrackState::kTracking);
        EXPECT_EQ(track->last_bounds, bounds) << "rotation " << static_cast<int>(rotation);
        const PointF expected_center{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
        EXPECT_EQ(track->predicted_center, expected_center);
        EXPECT_EQ(track->last_verified_sequence, 2U);
        ASSERT_EQ(track->templates.size(), 2U);

        // Round trip: the committed track verifies and confirms again on the
        // same presented frame.
        const auto reverification = tracker.verify_track(9U, view, std::nullopt);
        ASSERT_TRUE(reverification.ok()) << "rotation " << static_cast<int>(rotation) << ": "
                                         << reverification.status().message();
        const auto recommitted = tracker.commit_track_evidence(
            9U, reverification.value(), position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
        ASSERT_TRUE(recommitted.ok()) << "rotation " << static_cast<int>(rotation) << ": "
                                      << recommitted.status().message();
        EXPECT_EQ(recommitted.value().grade, EvidenceGrade::kConfirmed) << "rotation " << static_cast<int>(rotation);
        EXPECT_EQ(tracker.find_track(9U)->last_verified_sequence, 3U);
    }
}

/// A flush corner track: the same-frame verification is strong at offset
/// (0, 0) and the confirmed commit keeps the flush bounds exactly.
TEST(ObjectTrackerEvidenceFusionTest, CommitFlushEdgeTrackKeepsClampedBounds) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(24, 24);
    const ImageView view = view_of(image);
    const RectF bounds{0.0F, 0.0F, 8.0F, 8.0F};
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 3U, bounds));

    const auto verified = tracker.verify_track(3U, view, std::nullopt);
    ASSERT_TRUE(verified.ok()) << verified.status().message();
    ASSERT_EQ(verified.value().appearance.outcome, AppearanceChannelOutcome::kStrong);
    ASSERT_EQ(verified.value().appearance.best_offset_dx, 0);
    ASSERT_EQ(verified.value().appearance.best_offset_dy, 0);

    const auto committed = tracker.commit_track_evidence(
        3U, verified.value(), position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(committed.ok());
    EXPECT_EQ(committed.value().grade, EvidenceGrade::kConfirmed);
    const TargetTrack* track = tracker.find_track(3U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->last_bounds, bounds);
}

// --- M7-02/M7-03 deferred paths on the now-constructible states ------------------------

/// record_observation is bookkeeping-only and accepts the non-terminated
/// inactive states: kUncertain and kLost tracks take observations without any
/// state/evidence field moving, and the bounded history still evicts
/// explicitly. Terminated tracks stay rejected.
TEST(ObjectTrackerEvidenceFusionTest, RecordObservationAcceptsUncertainAndLostTracks) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    // kTracking -> kUncertain.
    const auto degraded =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(degraded.ok());
    ASSERT_EQ(degraded.value().state, TrackState::kUncertain);
    const TargetTrack uncertain_before = *tracker.find_track(7U);

    ASSERT_TRUE(tracker.record_observation(7U, RectF{17.0F, 16.0F, 8.0F, 8.0F}, 0.4F, 3).ok());
    const TargetTrack* uncertain = tracker.find_track(7U);
    ASSERT_NE(uncertain, nullptr);
    EXPECT_EQ(uncertain->state, TrackState::kUncertain);
    EXPECT_EQ(uncertain->position_history.size(), 2U);
    EXPECT_EQ(uncertain->position_history.back().frame_sequence, 3U);
    EXPECT_EQ(uncertain->confidence, uncertain_before.confidence);  // bookkeeping only
    EXPECT_EQ(uncertain->last_bounds, uncertain_before.last_bounds);

    // kUncertain -> kLost.
    const auto lost =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 4);
    ASSERT_TRUE(lost.ok());
    ASSERT_EQ(lost.value().state, TrackState::kLost);
    ASSERT_TRUE(tracker.record_observation(7U, RectF{18.0F, 16.0F, 8.0F, 8.0F}, 0.3F, 5).ok());
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kLost);
    EXPECT_EQ(tracker.find_track(7U)->position_history.size(), 3U);

    // Bounded history still evicts explicitly from the inactive states.
    ObjectTrackerOptions bounded_options;
    bounded_options.max_position_history = 2;
    bounded_options.uncertain_frame_limit = 1;  // one insufficient commit: kLost
    ObjectTracker bounded = make_tracker(bounded_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(bounded, view, 5U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    const auto bounded_lost =
        bounded.commit_track_evidence(5U, verification_of(5U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(bounded_lost.ok());
    ASSERT_EQ(bounded_lost.value().state, TrackState::kLost);
    ASSERT_TRUE(bounded.record_observation(5U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 0.5F, 3).ok());
    EXPECT_EQ(bounded.evicted_observation_count(), 0U);  // history at 2 of 2
    ASSERT_TRUE(bounded.record_observation(5U, RectF{17.0F, 16.0F, 8.0F, 8.0F}, 0.5F, 4).ok());
    EXPECT_EQ(bounded.evicted_observation_count(), 1U);  // adoption entry evicted
    ASSERT_EQ(bounded.find_track(5U)->position_history.size(), 2U);
    EXPECT_EQ(bounded.find_track(5U)->position_history.front().frame_sequence, 3U);
}

/// evaluate_change_gate keeps reporting the inactive states explicitly (never
/// a silent skip) once they are constructible: kUncertain, kLost and
/// kTerminated tracks get kInactive entries with the state echo under every
/// classification, and the pure decision moves nothing.
TEST(ObjectTrackerEvidenceFusionTest, EvaluateChangeGateReportsInactiveStatesExplicitly) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 1U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 2U, RectF{40.0F, 16.0F, 8.0F, 8.0F}, 1));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 3U, RectF{52.0F, 16.0F, 8.0F, 8.0F}, 1));

    // 1: kUncertain (one insufficient commit), 2: kLost (two insufficient
    // commits exhaust the limit of 2), 3: kTerminated (caller).
    const auto uncertain =
        tracker.commit_track_evidence(1U, verification_of(1U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(uncertain.ok());
    ASSERT_EQ(uncertain.value().state, TrackState::kUncertain);
    const auto lost_first =
        tracker.commit_track_evidence(2U, verification_of(2U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(lost_first.ok());
    ASSERT_EQ(lost_first.value().state, TrackState::kUncertain);
    const auto lost =
        tracker.commit_track_evidence(2U, verification_of(2U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(lost.ok());
    ASSERT_EQ(lost.value().state, TrackState::kLost);
    ASSERT_TRUE(tracker.terminate(3U, 3).ok());

    for (const ChangeClassification classification :
         {ChangeClassification::kNone, ChangeClassification::kPartial, ChangeClassification::kGlobal}) {
        const auto gated = tracker.evaluate_change_gate(make_gate_report(classification, {RectI{0, 0, 64, 64}}));
        ASSERT_TRUE(gated.ok()) << gated.status().message();
        ASSERT_EQ(gated.value().tracks.size(), 3U);
        EXPECT_EQ(gated.value().tracks[0].track_id, 1U);
        EXPECT_EQ(gated.value().tracks[0].state, TrackState::kUncertain);
        EXPECT_EQ(gated.value().tracks[0].decision, ChangeGateDecision::kInactive);
        EXPECT_EQ(gated.value().tracks[1].track_id, 2U);
        EXPECT_EQ(gated.value().tracks[1].state, TrackState::kLost);
        EXPECT_EQ(gated.value().tracks[1].decision, ChangeGateDecision::kInactive);
        EXPECT_EQ(gated.value().tracks[2].track_id, 3U);
        EXPECT_EQ(gated.value().tracks[2].state, TrackState::kTerminated);
        EXPECT_EQ(gated.value().tracks[2].decision, ChangeGateDecision::kInactive);
    }

    // The gate is a pure decision: no state moved.
    EXPECT_EQ(tracker.find_track(1U)->state, TrackState::kUncertain);
    EXPECT_EQ(tracker.find_track(2U)->state, TrackState::kLost);
    EXPECT_EQ(tracker.find_track(3U)->state, TrackState::kTerminated);
}

/// verify_track accepts any non-terminated state (the M7-08 reuse boundary):
/// construct kUncertain and kLost tracks and verify both; kTerminated stays
/// rejected.
TEST(ObjectTrackerEvidenceFusionTest, VerifyTrackAcceptsUncertainAndLostTracks) {
    ObjectTrackerOptions options;
    options.uncertain_frame_limit = 2;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, RectF{16.0F, 16.0F, 8.0F, 8.0F}, 1));

    const auto uncertain =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kTracking, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(uncertain.ok());
    ASSERT_EQ(uncertain.value().state, TrackState::kUncertain);
    const auto uncertain_verification = tracker.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(uncertain_verification.ok()) << uncertain_verification.status().message();
    EXPECT_EQ(uncertain_verification.value().state, TrackState::kUncertain);

    const auto lost =
        tracker.commit_track_evidence(7U, verification_of(7U, TrackState::kUncertain, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(lost.ok());
    ASSERT_EQ(lost.value().state, TrackState::kLost);
    const auto lost_verification = tracker.verify_track(7U, view, std::nullopt);
    ASSERT_TRUE(lost_verification.ok()) << lost_verification.status().message();
    EXPECT_EQ(lost_verification.value().state, TrackState::kLost);

    ASSERT_TRUE(tracker.terminate(7U, 4).ok());
    const auto terminated_verification = tracker.verify_track(7U, view, std::nullopt);
    EXPECT_EQ(terminated_verification.status().code(), ErrorCode::kInvalidArgument);
}

// --- DEC-010 gate passthrough (StableIdTracker::advance, design section 6.5) -----------

VisualRegion id_region(float x, float y, float width, float height) {
    VisualRegion region;
    region.bounds = RectF{x, y, width, height};
    region.anchor = PointF{x + width / 2.0F, y + height / 2.0F};
    region.stable_id = 12345U;  // tracker output must not depend on input ids
    return region;
}

mirador::Result<StableIdReport> advance_regions(StableIdTracker& tracker, const std::vector<VisualRegion>& regions,
                                                std::span<const ConfirmedAssociation> associations = {},
                                                const StableIdOptions& options = {}) {
    return tracker.advance(std::span<const VisualRegion>{regions.data(), regions.size()}, options, {}, associations);
}

/// A tracking-confirmed pairing bypasses the IoU/center gate and the cost
/// ranking: a region that moved far outside every gate is retained with the
/// confirmed id, counts toward the retention tallies and suppresses the
/// generation bump.
TEST(StableIdConfirmedAssociationTest, ConfirmedAssociationBypassesGateAndRetains) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance_regions(tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());

    // Without the association the far region cannot match: fresh id, bump.
    StableIdTracker legacy;
    ASSERT_TRUE(advance_regions(legacy, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const auto legacy_report = advance_regions(legacy, {id_region(200.0F, 200.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(legacy_report.ok());
    EXPECT_EQ(legacy_report.value().assignments[0].event, IdEvent::kNew);
    EXPECT_TRUE(legacy_report.value().generation_bump);

    // With the confirmed association the same region retains id 1.
    const std::vector<ConfirmedAssociation> associations{{0U, 1U}};
    const auto report = advance_regions(tracker, {id_region(200.0F, 200.0F, 10.0F, 10.0F)}, associations);
    ASSERT_TRUE(report.ok()) << report.status().message();
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().retained_count, 1U);
    EXPECT_EQ(report.value().new_count, 0U);
    EXPECT_FALSE(report.value().generation_bump);
}

/// The pairing also beats the center-gate boundary that the static gate
/// rejects (displacement 25.5 > radius 25) — the time-consistency signal
/// stands in for the static gate.
TEST(StableIdConfirmedAssociationTest, ConfirmedAssociationBeatsCenterGateRejection) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance_regions(tracker, {id_region(0.0F, 0.0F, 30.0F, 40.0F)}).ok());
    const std::vector<ConfirmedAssociation> associations{{0U, 1U}};
    const auto report = advance_regions(tracker, {id_region(25.5F, 0.0F, 30.0F, 40.0F)}, associations);
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
    EXPECT_FALSE(report.value().generation_bump);
}

/// An association naming an untracked id is ignored: the region falls through
/// to the normal gate (far away -> fresh id, generation bump).
TEST(StableIdConfirmedAssociationTest, UntrackedAssociationFallsThroughToNormalGate) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance_regions(tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const std::vector<ConfirmedAssociation> associations{{0U, 999U}};
    const auto report = advance_regions(tracker, {id_region(200.0F, 200.0F, 10.0F, 10.0F)}, associations);
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kNew);
    EXPECT_EQ(report.value().assignments[0].stable_id, 2U);
    EXPECT_TRUE(report.value().generation_bump);
}

/// Pre-matched regions are skipped by the greedy pass: the confirmed far
/// region keeps id 1 while the region that statically overlaps the tracked
/// bounds gets a fresh id — no double pairing.
TEST(StableIdConfirmedAssociationTest, PreMatchedRegionIsSkippedByGreedyPass) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance_regions(tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());

    // Region 0 is confirmed against id 1 despite being far away; region 1
    // overlaps the tracked bounds and would greedily claim id 1 without the
    // pre-match.
    const std::vector<ConfirmedAssociation> associations{{0U, 1U}};
    const auto report = advance_regions(
        tracker, {id_region(200.0F, 200.0F, 10.0F, 10.0F), id_region(1.0F, 1.0F, 10.0F, 10.0F)}, associations);
    ASSERT_TRUE(report.ok()) << report.status().message();
    ASSERT_EQ(report.value().assignments.size(), 2U);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[1].event, IdEvent::kNew);
    EXPECT_EQ(report.value().assignments[1].stable_id, 2U);
    EXPECT_EQ(report.value().retained_count, 1U);
}

/// Pre-matched regions count as retained for the generation decision: two
/// confirmed far regions retain everything, so no generation bump — while the
/// same input without associations bumps.
TEST(StableIdConfirmedAssociationTest, PreMatchedRegionsCountForTheGenerationDecision) {
    StableIdTracker tracker;
    ASSERT_TRUE(
        advance_regions(tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F), id_region(100.0F, 100.0F, 10.0F, 10.0F)}).ok());

    const std::vector<ConfirmedAssociation> associations{{0U, 1U}, {1U, 2U}};
    const auto retained = advance_regions(
        tracker, {id_region(300.0F, 0.0F, 10.0F, 10.0F), id_region(0.0F, 300.0F, 10.0F, 10.0F)}, associations);
    ASSERT_TRUE(retained.ok());
    EXPECT_EQ(retained.value().retained_count, 2U);
    EXPECT_FALSE(retained.value().generation_bump);

    // Sanity: nothing moved after the confirmed advance; the ids travel.
    const auto again =
        advance_regions(tracker, {id_region(300.0F, 0.0F, 10.0F, 10.0F), id_region(0.0F, 300.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().assignments[0].stable_id, 1U);
    EXPECT_EQ(again.value().assignments[1].stable_id, 2U);
}

/// Association validation is explicit and state-preserving: out-of-range
/// region index, zero stable id, duplicate region index and duplicate stable
/// id all fail with kInvalidArgument and leave the tracked state untouched.
TEST(StableIdConfirmedAssociationTest, AssociationValidationErrorsKeepStateUntouched) {
    StableIdTracker tracker;
    ASSERT_TRUE(
        advance_regions(tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F), id_region(100.0F, 100.0F, 10.0F, 10.0F)}).ok());
    const std::vector<VisualRegion> current{id_region(0.0F, 0.0F, 10.0F, 10.0F)};

    const std::vector<ConfirmedAssociation> out_of_range_assoc{{5U, 1U}};
    const auto out_of_range = advance_regions(tracker, current, out_of_range_assoc);
    EXPECT_EQ(out_of_range.status().code(), ErrorCode::kInvalidArgument);

    const std::vector<ConfirmedAssociation> zero_id_assoc{{0U, 0U}};
    const auto zero_id = advance_regions(tracker, current, zero_id_assoc);
    EXPECT_EQ(zero_id.status().code(), ErrorCode::kInvalidArgument);

    const std::vector<ConfirmedAssociation> duplicate_index_assoc{{0U, 1U}, {0U, 2U}};
    const auto duplicate_index = advance_regions(tracker, current, duplicate_index_assoc);
    EXPECT_EQ(duplicate_index.status().code(), ErrorCode::kInvalidArgument);

    // Duplicate stable id across two associations: explicit error.
    const std::vector<VisualRegion> two{id_region(0.0F, 0.0F, 10.0F, 10.0F), id_region(200.0F, 200.0F, 10.0F, 10.0F)};
    const std::vector<ConfirmedAssociation> duplicate_id_assoc{{0U, 1U}, {1U, 1U}};
    const auto duplicate_stable_id = advance_regions(tracker, two, duplicate_id_assoc);
    EXPECT_EQ(duplicate_stable_id.status().code(), ErrorCode::kInvalidArgument);

    // The failed calls committed nothing.
    EXPECT_EQ(tracker.tracked_count(), 2U);
    EXPECT_EQ(tracker.last_id(), 2U);

    // A valid single association naming one of the tracked ids still works.
    const std::vector<ConfirmedAssociation> valid_assoc{{0U, 2U}};
    const auto valid = advance_regions(tracker, current, valid_assoc);
    ASSERT_TRUE(valid.ok()) << valid.status().message();
    EXPECT_EQ(valid.value().assignments[0].stable_id, 2U);
    EXPECT_EQ(valid.value().assignments[0].event, IdEvent::kRetained);
}

/// Duplicate associations naming untracked ids are explicit errors too (the
/// frozen design wording lists duplicate indexes/ids without a tracked-id
/// qualifier): structural validation precedes the tracked lookup.
TEST(StableIdConfirmedAssociationTest, DuplicateUntrackedAssociationsAreExplicitErrors) {
    const auto expect_invalid = [](const char* label, const mirador::Result<StableIdReport>& result) {
        if (result.ok()) {
            ADD_FAILURE() << label << ": the frozen design wording (object-tracking design section 6.5, "
                          << "duplicate indexes/ids are an explicit kInvalidArgument) is not enforced "
                          << "when the duplicated association names an untracked id";
            return;
        }
        EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument) << label;
    };

    // Duplicate region index where the first association names an untracked id.
    StableIdTracker index_tracker;
    ASSERT_TRUE(advance_regions(index_tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const std::vector<VisualRegion> two{id_region(0.0F, 0.0F, 10.0F, 10.0F), id_region(200.0F, 200.0F, 10.0F, 10.0F)};
    const std::vector<ConfirmedAssociation> duplicate_index{{1U, 999U}, {1U, 888U}};
    expect_invalid("duplicate region index over an untracked association",
                   advance_regions(index_tracker, two, duplicate_index));

    // Duplicate stable id across two associations, both untracked.
    StableIdTracker id_tracker;
    ASSERT_TRUE(advance_regions(id_tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const std::vector<ConfirmedAssociation> duplicate_id{{0U, 999U}, {1U, 999U}};
    expect_invalid("duplicate stable id over untracked associations", advance_regions(id_tracker, two, duplicate_id));

    // An untracked association sharing a region index with a pre-matched one.
    StableIdTracker mixed_tracker;
    ASSERT_TRUE(advance_regions(mixed_tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const std::vector<ConfirmedAssociation> mixed{{0U, 1U}, {0U, 999U}};
    expect_invalid("untracked association duplicating a pre-matched region index",
                   advance_regions(mixed_tracker, two, mixed));
}

/// The empty association list reproduces the previous behavior bit for bit:
/// retention through the static gate, splits and budget errors are unchanged.
TEST(StableIdConfirmedAssociationTest, EmptyAssociationListKeepsTheFrozenStaticSemantics) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance_regions(tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());

    const auto retained = advance_regions(tracker, {id_region(1.0F, 1.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(retained.ok());
    EXPECT_EQ(retained.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(retained.value().assignments[0].stable_id, 1U);

    // Static split path (center gate shrunk): unchanged by the extension.
    StableIdTracker split_tracker;
    StableIdOptions split_options;
    split_options.center_gate_ratio = 0.2;
    ASSERT_TRUE(advance_regions(split_tracker, {id_region(0.0F, 0.0F, 100.0F, 100.0F)}).ok());
    const auto split = advance_regions(
        split_tracker, {id_region(0.0F, 0.0F, 50.0F, 50.0F), id_region(50.0F, 50.0F, 50.0F, 50.0F)}, {}, split_options);
    ASSERT_TRUE(split.ok());
    EXPECT_EQ(split.value().split_count, 1U);
    EXPECT_TRUE(split.value().generation_bump);

    // max_regions budget error unchanged (the association validation runs
    // after the region-count check).
    StableIdTracker budget_tracker;
    ASSERT_TRUE(advance_regions(budget_tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    StableIdOptions budget_options;
    budget_options.max_regions = 1;
    const std::vector<ConfirmedAssociation> one_confirmed{{0U, 1U}};
    const auto over_budget =
        advance_regions(budget_tracker, {id_region(0.0F, 0.0F, 10.0F, 10.0F), id_region(100.0F, 0.0F, 10.0F, 10.0F)},
                        one_confirmed, budget_options);
    EXPECT_EQ(over_budget.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(budget_tracker.tracked_count(), 1U);
}

}  // namespace
