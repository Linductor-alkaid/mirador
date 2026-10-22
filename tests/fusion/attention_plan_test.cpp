// M9-03 attention-plan surface tests (DEC-026 section 2, Experimental): the
// engaged attention policy with its digest-driven monotonic revision and the
// frozen validation passthrough plus the M9-03 kFrame/kOriented ROI-space
// rule, the coordination byte budget with its formula and zero point, the
// injected non-owning tracker and the tracker-backed focus resolution matrix
// (snapshot win, stale-generation short-circuit, tracking/uncertain resolve,
// lost -> missing-localization, terminated/absent -> unknown, snapshot-only
// text/label/ROI conditions), the synchronous attention plan (input
// validation matrix, focus/bootstrap work with the importance echo and the
// identity fields, kind/priority/index ordering, per-condition resolution
// parallelism, revision echo, kIncomplete coverage, determinism, twin
// coordinators and the no-side-effect negative), the kFrame <-> kOriented
// conversion matrix over all four rotations (ROI-only, tracker-backed and
// full-frame ROIs with the oriented size swap), the peripheral
// reconsideration records (kNone/kGlobal/kPartial, focus-intersection
// suppression, touching/degenerate regions, report order) and the
// cancellation/deadline conversion with the idle-plan no-poll guarantee.
//
// Assertion helpers keep gtest macros out of hand-written loops wherever a
// function would otherwise nest them: macro-expanded control flow inside
// hand-written control flow is what readability-function-cognitive-complexity
// charges for, so per-record checks live in straight-line helpers and pure
// scans return bool for the caller to assert on.

#include <mirador/continuous_perception.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/object_tracker.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::AppearanceChannelOutcome;
using mirador::attention_policy_digest;
using mirador::AttentionCondition;
using mirador::AttentionPlan;
using mirador::AttentionPolicy;
using mirador::ChangeClassification;
using mirador::ContinuousPerception;
using mirador::ContinuousPerceptionOptions;
using mirador::CoordinateSpaceId;
using mirador::CoverageState;
using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::FocusResolution;
using mirador::FocusUnresolvedReason;
using mirador::Frame;
using mirador::ImageView;
using mirador::ImportancePolicy;
using mirador::kContinuousMaxCoordinationBytes;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PixelFormat;
using mirador::PlannedWork;
using mirador::PlannedWorkKind;
using mirador::PointF;
using mirador::PositionScenario;
using mirador::RectF;
using mirador::RectI;
using mirador::Result;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::TargetTrack;
using mirador::TrackPositionEvidence;
using mirador::TrackState;
using mirador::TrackVerification;
using mirador::VisualRegion;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// The tracker-pool position every tracker-backed fixture resolves to (inside
/// the 64x64 rig view; the presented-view space the pool reports).
constexpr RectF kRigTrackBounds{10.0F, 20.0F, 5.0F, 4.0F};

// --- Shared makers and assertion helpers ------------------------------------

VisualRegion make_region(uint64_t stable_id, RectF bounds, std::string text = {}, std::string label = {}) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.anchor = PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    region.confidence = 0.9F;
    region.text = std::move(text);
    region.label = std::move(label);
    return region;
}

SemanticSnapshot make_snapshot(std::vector<VisualRegion> regions, uint64_t generation = 7,
                               CoordinateSpaceId space = CoordinateSpaceId::kOriented) {
    SemanticSnapshot snapshot;
    snapshot.frame_sequence = 42;
    snapshot.generation = generation;
    snapshot.coordinate_space = space;
    snapshot.regions = std::move(regions);
    return snapshot;
}

/// Applies a change report to the snapshot (frame-space ROIs, as produced by
/// the change detector).
void apply_change(SemanticSnapshot* snapshot, ChangeClassification classification, std::vector<RectI> regions) {
    snapshot->change.classification = classification;
    snapshot->change.changed_regions = std::move(regions);
}

AttentionCondition roi_condition(RectF roi, CoordinateSpaceId space) {
    AttentionCondition condition;
    condition.roi = roi;
    condition.roi_space = space;
    return condition;
}

AttentionCondition id_condition(uint64_t stable_id, uint64_t generation) {
    AttentionCondition condition;
    condition.bind_stable_id = true;
    condition.stable_id = stable_id;
    condition.generation = generation;
    return condition;
}

AttentionCondition text_condition(std::string text) {
    AttentionCondition condition;
    condition.text_equals = std::move(text);
    return condition;
}

AttentionCondition label_condition(std::string label) {
    AttentionCondition condition;
    condition.label_equals = std::move(label);
    return condition;
}

AttentionPolicy policy_of(std::vector<AttentionCondition> conditions) {
    AttentionPolicy policy;
    policy.conditions = std::move(conditions);
    return policy;
}

ImportancePolicy importance_at(float priority, bool allow_bootstrap = false) {
    ImportancePolicy importance;
    importance.priority = priority;
    importance.allow_bootstrap_full_frame = allow_bootstrap;
    return importance;
}

/// Geometry-only frame: the plan reads rotation and dimensions, never pixels,
/// so the view data pointer stays null.
Frame make_plan_frame(int32_t width, int32_t height, Rotation rotation = Rotation::k0) {
    Frame frame;
    frame.image.width = width;
    frame.image.height = height;
    frame.image.format = PixelFormat::kRgba8;
    frame.image.rotation = rotation;
    frame.sequence = 1;
    return frame;
}

ContinuousPerception make_coordinator(const ContinuousPerceptionOptions& options = ContinuousPerceptionOptions{}) {
    auto created = ContinuousPerception::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_coordinator: " << created.status().message();
        return ContinuousPerception{};
    }
    return created.take_value();
}

ObjectTracker make_tracker_pool() {
    auto created = ObjectTracker::create();
    if (!created.ok()) {
        ADD_FAILURE() << "make_tracker_pool: " << created.status().message();
        return ObjectTracker{};
    }
    return created.take_value();
}

void expect_engaged(const Result<void>& engaged) {
    EXPECT_TRUE(engaged.ok()) << engaged.status().message();
}

void expect_rejected(const Result<void>& result, ErrorCode code) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), code);
    }
}

AttentionPlan expect_planned(const Result<AttentionPlan>& result) {
    EXPECT_TRUE(result.ok()) << result.status().message();
    if (!result.ok()) {
        return AttentionPlan{};
    }
    return result.value();
}

void expect_plan_rejected(const Result<AttentionPlan>& result, ErrorCode code) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), code);
    }
}

FocusResolution expect_resolved_focus(const Result<FocusResolution>& result) {
    EXPECT_TRUE(result.ok()) << result.status().message();
    if (!result.ok()) {
        return FocusResolution{};
    }
    EXPECT_TRUE(result.value().resolved);
    if (!result.value().resolved) {
        return FocusResolution{};
    }
    return result.value();
}

FocusUnresolvedReason expect_unresolved_reason(const Result<FocusResolution>& result) {
    EXPECT_TRUE(result.ok()) << result.status().message();
    if (!result.ok()) {
        return FocusUnresolvedReason::kUnknownStableId;
    }
    EXPECT_FALSE(result.value().resolved);
    return result.value().reason;
}

void expect_resolve_rejected(const Result<FocusResolution>& result, ErrorCode code) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), code);
    }
}

/// Frozen placement fields of one work item: kind, converted ROI and space.
void expect_work_placement(const PlannedWork& work, PlannedWorkKind kind, RectF roi, CoordinateSpaceId space) {
    EXPECT_EQ(work.kind, kind);
    EXPECT_EQ(work.roi, roi);
    EXPECT_EQ(work.space, space);
}

/// Frozen binding fields of one work item: caller priority, originating
/// condition and the identity echo (zero for ROI-only conditions and
/// bootstrap work, per the PlannedWork contract).
void expect_work_binding(const PlannedWork& work, float priority, size_t condition_index, uint64_t stable_id,
                         uint64_t generation) {
    EXPECT_EQ(work.priority, priority);
    EXPECT_EQ(work.condition_index, condition_index);
    EXPECT_EQ(work.stable_id, stable_id);
    EXPECT_EQ(work.generation, generation);
}

/// Frozen shape of one kFocus work item: the converted ROI in the snapshot
/// space plus the caller priority and identity echo.
void expect_focus_work(const PlannedWork& work, RectF roi, float priority, size_t condition_index,
                       CoordinateSpaceId space, uint64_t stable_id = 0, uint64_t generation = 0) {
    expect_work_placement(work, PlannedWorkKind::kFocus, roi, space);
    expect_work_binding(work, priority, condition_index, stable_id, generation);
}

/// Frozen shape of one kBootstrap work item: the full-frame ROI and zero
/// identity.
void expect_bootstrap_work(const PlannedWork& work, RectF full_frame, float priority, size_t condition_index,
                           CoordinateSpaceId space) {
    expect_work_placement(work, PlannedWorkKind::kBootstrap, full_frame, space);
    expect_work_binding(work, priority, condition_index, 0U, 0U);
}

// --- Tracker rig -------------------------------------------------------------
//
// Every TrackState is driven through the public ObjectTracker pipeline exactly
// like the M7-06 evidence-fusion suite does: adoption produces kTracking, one
// placeholder-grade commit degrades kTracking to kUncertain (or straight to
// kLost under uncertain_frame_limit == 1, whose L-th commit is the
// transition), and terminate() closes the identity as kTerminated.

/// Owning fixtures of one tracker pool: the pixel buffer and view must outlive
/// every pool call.
struct TrackerRig {
    ObjectTracker tracker;
    std::vector<std::byte> pixels;
    int32_t width = 64;
    int32_t height = 64;
    int64_t stride = 64;
    ImageView view;
};

ImageView rig_view_of(TrackerRig& rig) {
    ImageView view;
    view.data = rig.pixels.data();
    view.width = rig.width;
    view.height = rig.height;
    view.row_stride_bytes = rig.stride;
    view.format = PixelFormat::kGray8;
    view.rotation = Rotation::k0;
    return view;
}

/// Caller-declared verification evidence of the placeholder row: no appearance
/// candidate, the position prior only (the commit is contractually forbidden
/// from re-running the scan).
TrackVerification placeholder_verification(uint64_t track_id) {
    TrackVerification verification;
    verification.track_id = track_id;
    verification.state = TrackState::kTracking;
    verification.verification_roi = RectI{0, 0, 1, 1};
    verification.appearance.outcome = AppearanceChannelOutcome::kNone;
    verification.appearance.peak_sidelobe_ratio = 10.0;
    verification.appearance.best_template_index = 0U;
    return verification;
}

TrackPositionEvidence stationary_position() {
    TrackPositionEvidence position;
    position.scenario = PositionScenario::kStationary;
    position.inside_gate = true;
    return position;
}

/// Builds a pool holding track `id` at `bounds` in the requested lifecycle
/// state; fails the test when the public pipeline cannot reach that state.
TrackerRig make_track_rig(uint64_t id, const RectF& bounds, TrackState target) {
    TrackerRig rig;
    rig.pixels.assign(static_cast<size_t>(rig.stride) * static_cast<size_t>(rig.height), std::byte{0});
    rig.view = rig_view_of(rig);
    ObjectTrackerOptions options;
    if (target == TrackState::kLost) {
        options.uncertain_frame_limit = 1;  // the first insufficient commit is the transition
    }
    auto created = ObjectTracker::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_track_rig: " << created.status().message();
        return rig;
    }
    rig.tracker = created.take_value();
    const auto adopted = rig.tracker.adopt_track(make_region(id, bounds, "OK", "btn"), rig.view, 1);
    if (!adopted.ok()) {
        ADD_FAILURE() << "adopt " << id << ": " << adopted.status().message();
        return rig;
    }
    if (target == TrackState::kUncertain || target == TrackState::kLost) {
        const auto committed = rig.tracker.commit_track_evidence(id, placeholder_verification(id),
                                                                 stationary_position(), std::nullopt, rig.view, 2);
        if (!committed.ok()) {
            ADD_FAILURE() << "commit " << id << ": " << committed.status().message();
        }
    }
    if (target == TrackState::kTerminated && !rig.tracker.terminate(id, 99).ok()) {
        ADD_FAILURE() << "terminate " << id << " failed";
    }
    const TargetTrack* track = rig.tracker.find_track(id);
    if (track == nullptr || track->state != target) {
        ADD_FAILURE() << "rig did not reach the requested track state";
    }
    return rig;
}

// --- A. Policy state ----------------------------------------------------------

TEST(PolicyEngagementTest, FirstEngagementStartsRevisionAtOneAndStoresThePolicy) {
    ContinuousPerception coordinator = make_coordinator();
    EXPECT_FALSE(coordinator.has_attention_policy());
    EXPECT_EQ(coordinator.policy_revision(), 0U);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), 0);

    const AttentionPolicy policy = policy_of({id_condition(7, 3)});
    expect_engaged(coordinator.set_attention_policy(policy));
    EXPECT_TRUE(coordinator.has_attention_policy());
    EXPECT_EQ(coordinator.policy_revision(), 1U);
    EXPECT_EQ(coordinator.attention_policy(), policy);
    EXPECT_GT(coordinator.coordination_owned_bytes(), 0);
}

TEST(PolicyEngagementTest, IdenticalResetIsIdempotentAndRevisionIsMonotonic) {
    ContinuousPerception coordinator = make_coordinator();
    const AttentionPolicy first = policy_of({roi_condition(RectF{1.0F, 1.0F, 5.0F, 5.0F}, CoordinateSpaceId::kFrame)});

    expect_engaged(coordinator.set_attention_policy(first));
    EXPECT_EQ(coordinator.policy_revision(), 1U);
    // Re-setting the identical policy is idempotent: same digest, no bump.
    expect_engaged(coordinator.set_attention_policy(first));
    EXPECT_EQ(coordinator.policy_revision(), 1U);

    const AttentionPolicy second = policy_of({text_condition("Save")});
    expect_engaged(coordinator.set_attention_policy(second));
    EXPECT_EQ(coordinator.policy_revision(), 2U);
    expect_engaged(coordinator.set_attention_policy(second));
    EXPECT_EQ(coordinator.policy_revision(), 2U);

    // Returning to the first policy changes the digest against the stored one.
    expect_engaged(coordinator.set_attention_policy(first));
    EXPECT_EQ(coordinator.policy_revision(), 3U);
    EXPECT_EQ(coordinator.attention_policy(), first);
}

TEST(PolicyEngagementTest, FirstEngagementOfAnEmptyPolicyStillCountsAsAnEngagement) {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(AttentionPolicy{}));
    EXPECT_TRUE(coordinator.has_attention_policy());
    EXPECT_EQ(coordinator.policy_revision(), 1U);
    EXPECT_GT(coordinator.coordination_owned_bytes(), 0);
}

TEST(PolicyEngagementTest, ValidationErrorsPassThroughAndLeaveTheStateUntouched) {
    ContinuousPerception coordinator = make_coordinator();

    // Frozen validate_attention_policy errors, verbatim.
    expect_rejected(coordinator.set_attention_policy(policy_of({AttentionCondition{}})), ErrorCode::kInvalidArgument);

    const AttentionCondition zero_id = id_condition(0, 3);
    expect_rejected(coordinator.set_attention_policy(policy_of({zero_id})), ErrorCode::kInvalidArgument);

    AttentionCondition nan_roi = text_condition("Save");
    nan_roi.roi = RectF{kNaN, 0.0F, 10.0F, 10.0F};
    expect_rejected(coordinator.set_attention_policy(policy_of({nan_roi})), ErrorCode::kInvalidArgument);

    AttentionPolicy zero_cap;
    zero_cap.max_condition_bytes = 0;
    expect_rejected(coordinator.set_attention_policy(zero_cap), ErrorCode::kInvalidArgument);

    AttentionPolicy loose_cap;
    loose_cap.max_condition_bytes = kContinuousMaxCoordinationBytes + 1;
    expect_rejected(coordinator.set_attention_policy(loose_cap), ErrorCode::kBudgetExceeded);

    AttentionPolicy too_many;
    for (int32_t index = 0; index < 65; ++index) {
        too_many.conditions.push_back(roi_condition(RectF{1.0F, 1.0F, 2.0F, 2.0F}, CoordinateSpaceId::kFrame));
    }
    expect_rejected(coordinator.set_attention_policy(too_many), ErrorCode::kBudgetExceeded);

    // Nothing above may have engaged a policy or moved the revision.
    EXPECT_FALSE(coordinator.has_attention_policy());
    EXPECT_EQ(coordinator.policy_revision(), 0U);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), 0);
}

TEST(PolicyEngagementTest, DisplaySpaceConditionRoisAreRejectedIncludingMixedCriteria) {
    ContinuousPerception coordinator = make_coordinator();

    const AttentionPolicy display_only =
        policy_of({roi_condition(RectF{5.0F, 5.0F, 10.0F, 10.0F}, CoordinateSpaceId::kDisplay)});
    expect_rejected(coordinator.set_attention_policy(display_only), ErrorCode::kInvalidArgument);

    // A higher-priority criterion on the same condition does not mask the
    // display-space ROI rule.
    AttentionCondition mixed = id_condition(3, 1);
    mixed.roi = RectF{5.0F, 5.0F, 10.0F, 10.0F};
    mixed.roi_space = CoordinateSpaceId::kDisplay;
    expect_rejected(coordinator.set_attention_policy(policy_of({mixed})), ErrorCode::kInvalidArgument);

    // Any non-kFrame/kOriented space is rejected for a positive-size ROI.
    const AttentionCondition user_space = roi_condition(RectF{5.0F, 5.0F, 10.0F, 10.0F}, CoordinateSpaceId::kUserBase);
    expect_rejected(coordinator.set_attention_policy(policy_of({user_space})), ErrorCode::kInvalidArgument);

    // A zero-size ROI carries no spatial condition: the rule does not fire.
    AttentionCondition textual = text_condition("Save");
    textual.roi_space = CoordinateSpaceId::kDisplay;
    expect_engaged(coordinator.set_attention_policy(policy_of({textual})));
    EXPECT_EQ(coordinator.policy_revision(), 1U);
}

TEST(PolicyEngagementTest, CoordinationBudgetRejectionKeepsThePreviousPolicyAndRevision) {
    ContinuousPerceptionOptions options;
    options.source_id = "cam";
    options.max_coordination_bytes = 1024;
    ContinuousPerception coordinator = make_coordinator(options);

    const AttentionPolicy small = policy_of({roi_condition(RectF{1.0F, 1.0F, 5.0F, 5.0F}, CoordinateSpaceId::kFrame)});
    expect_engaged(coordinator.set_attention_policy(small));
    ASSERT_TRUE(coordinator.has_attention_policy());
    ASSERT_EQ(coordinator.policy_revision(), 1U);
    const uint64_t revision = coordinator.policy_revision();
    const int64_t bytes = coordinator.coordination_owned_bytes();

    const AttentionCondition heavy = text_condition(std::string(2048, 'a'));
    expect_rejected(coordinator.set_attention_policy(policy_of({heavy})), ErrorCode::kBudgetExceeded);

    // The rejection is a no-op: previous policy, revision and byte account.
    EXPECT_TRUE(coordinator.has_attention_policy());
    EXPECT_EQ(coordinator.policy_revision(), revision);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes);
    EXPECT_EQ(coordinator.attention_policy(), small);
}

TEST(PolicyEngagementTest, OwnedBytesFollowTheFormulaAndGrowWithMatcherText) {
    ContinuousPerception coordinator = make_coordinator();
    EXPECT_EQ(coordinator.coordination_owned_bytes(), 0);

    AttentionCondition condition = text_condition("abc");
    condition.label_equals = "btn";
    expect_engaged(coordinator.set_attention_policy(policy_of({condition})));
    ASSERT_TRUE(coordinator.has_attention_policy());

    // The documented formula over the engaged policy's own buffers.
    const AttentionPolicy& engaged = coordinator.attention_policy();
    auto expected = static_cast<int64_t>(sizeof(AttentionPolicy));
    expected += static_cast<int64_t>(engaged.conditions.capacity() * sizeof(AttentionCondition));
    for (const AttentionCondition& stored : engaged.conditions) {
        expected += static_cast<int64_t>(sizeof(AttentionCondition));
        expected += static_cast<int64_t>(stored.text_equals.capacity());
        expected += static_cast<int64_t>(stored.label_equals.capacity());
    }
    const int64_t small_bytes = coordinator.coordination_owned_bytes();
    EXPECT_EQ(small_bytes, expected);
    EXPECT_GT(small_bytes, static_cast<int64_t>(sizeof(AttentionPolicy)));

    // Longer matcher text grows the account (and the digest: revision bumps).
    expect_engaged(coordinator.set_attention_policy(policy_of({text_condition(std::string(64, 'x'))})));
    EXPECT_GT(coordinator.coordination_owned_bytes(), small_bytes);
    EXPECT_EQ(coordinator.policy_revision(), 2U);
}

TEST(PolicyEngagementTest, MaxConditionBytesChangeBumpsTheDigestDrivenRevision) {
    ContinuousPerception coordinator = make_coordinator();
    const AttentionPolicy base = policy_of({roi_condition(RectF{1.0F, 1.0F, 5.0F, 5.0F}, CoordinateSpaceId::kFrame)});
    expect_engaged(coordinator.set_attention_policy(base));
    EXPECT_EQ(coordinator.policy_revision(), 1U);

    AttentionPolicy re_capped = base;
    re_capped.max_condition_bytes = 4096;
    EXPECT_NE(attention_policy_digest(re_capped), attention_policy_digest(base));
    expect_engaged(coordinator.set_attention_policy(re_capped));
    EXPECT_EQ(coordinator.policy_revision(), 2U);
    // Idempotent on the second identical set.
    expect_engaged(coordinator.set_attention_policy(re_capped));
    EXPECT_EQ(coordinator.policy_revision(), 2U);
}

// --- B. Tracker injection -------------------------------------------------------

TEST(TrackerInjectionTest, FreshCoordinatorHasNoTrackerAttached) {
    const ContinuousPerception coordinator = make_coordinator();
    EXPECT_EQ(coordinator.tracker(), nullptr);
}

TEST(TrackerInjectionTest, AttachReadbackAndNullDetach) {
    ContinuousPerception coordinator = make_coordinator();
    const ObjectTracker tracker = make_tracker_pool();

    coordinator.attach_tracker(&tracker);
    EXPECT_EQ(coordinator.tracker(), &tracker);

    coordinator.attach_tracker(nullptr);
    EXPECT_EQ(coordinator.tracker(), nullptr);
}

// --- C. resolve_focus matrix ------------------------------------------------------

TEST(ResolveFocusMatrixTest, SnapshotHitBeatsTheAttachedTracker) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot =
        make_snapshot({make_region(11, RectF{1.0F, 2.0F, 3.0F, 4.0F}, "OK")}, 7, CoordinateSpaceId::kOriented);

    const FocusResolution resolution = expect_resolved_focus(coordinator.resolve_focus(id_condition(11, 7), snapshot));
    EXPECT_EQ(resolution.focus.stable_id, 11U);
    EXPECT_EQ(resolution.focus.generation, 7U);
    EXPECT_EQ(resolution.focus.roi, (RectF{1.0F, 2.0F, 3.0F, 4.0F}));
    EXPECT_EQ(resolution.focus.space, snapshot.coordinate_space);
}

TEST(ResolveFocusMatrixTest, StaleGenerationShortCircuitsBeforeTheTracker) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    // The id is absent from the snapshot AND the generation is stale: the
    // frozen order reports the stale generation without consulting the pool.
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    EXPECT_EQ(expect_unresolved_reason(coordinator.resolve_focus(id_condition(11, 6), snapshot)),
              FocusUnresolvedReason::kStaleGeneration);
}

TEST(ResolveFocusMatrixTest, WithoutATrackerAnUnknownIdStaysUnknown) {
    const ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    EXPECT_EQ(expect_unresolved_reason(coordinator.resolve_focus(id_condition(11, 7), snapshot)),
              FocusUnresolvedReason::kUnknownStableId);
}

TEST(ResolveFocusMatrixTest, TrackingTrackResolvesToTheCurrentPoolPosition) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kFrame);

    const FocusResolution resolution = expect_resolved_focus(coordinator.resolve_focus(id_condition(11, 7), snapshot));
    EXPECT_EQ(resolution.focus.stable_id, 11U);
    EXPECT_EQ(resolution.focus.generation, 7U);  // snapshot generation echo
    EXPECT_EQ(resolution.focus.roi, kRigTrackBounds);
    EXPECT_EQ(resolution.focus.space, CoordinateSpaceId::kOriented);  // the pool's presented-view space
}

TEST(ResolveFocusMatrixTest, UncertainTrackResolvesToTheCurrentPoolPosition) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kUncertain);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    const FocusResolution resolution = expect_resolved_focus(coordinator.resolve_focus(id_condition(11, 7), snapshot));
    EXPECT_EQ(resolution.focus.stable_id, 11U);
    EXPECT_EQ(resolution.focus.generation, 7U);
    EXPECT_EQ(resolution.focus.roi, kRigTrackBounds);
    EXPECT_EQ(resolution.focus.space, CoordinateSpaceId::kOriented);
}

TEST(ResolveFocusMatrixTest, LostTrackReportsMissingLocalization) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kLost);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    EXPECT_EQ(expect_unresolved_reason(coordinator.resolve_focus(id_condition(11, 7), snapshot)),
              FocusUnresolvedReason::kMissingLocalization);
}

TEST(ResolveFocusMatrixTest, TerminatedTrackStaysUnknownStableId) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTerminated);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    EXPECT_EQ(expect_unresolved_reason(coordinator.resolve_focus(id_condition(11, 7), snapshot)),
              FocusUnresolvedReason::kUnknownStableId);
}

TEST(ResolveFocusMatrixTest, AbsentTrackStaysUnknownStableId) {
    const ObjectTracker tracker = make_tracker_pool();
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    EXPECT_EQ(expect_unresolved_reason(coordinator.resolve_focus(id_condition(11, 7), snapshot)),
              FocusUnresolvedReason::kUnknownStableId);
}

TEST(ResolveFocusMatrixTest, TextAndLabelConditionsNeverConsultTheTracker) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    EXPECT_EQ(expect_unresolved_reason(coordinator.resolve_focus(text_condition("Ghost"), snapshot)),
              FocusUnresolvedReason::kNotObserved);
    EXPECT_EQ(expect_unresolved_reason(coordinator.resolve_focus(label_condition("menu"), snapshot)),
              FocusUnresolvedReason::kNotObserved);
}

TEST(ResolveFocusMatrixTest, RoiOnlyConditionNeverConsultsTheTracker) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    const FocusResolution resolution = expect_resolved_focus(
        coordinator.resolve_focus(roi_condition(RectF{5.0F, 5.0F, 20.0F, 20.0F}, CoordinateSpaceId::kFrame), snapshot));
    EXPECT_EQ(resolution.focus.roi, (RectF{5.0F, 5.0F, 20.0F, 20.0F}));
    EXPECT_EQ(resolution.focus.space, CoordinateSpaceId::kFrame);  // as declared, without conversion
}

TEST(ResolveFocusMatrixTest, MalformedConditionErrorsPassThroughWithTrackerAttached) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    expect_resolve_rejected(coordinator.resolve_focus(AttentionCondition{}, snapshot), ErrorCode::kInvalidArgument);
}

// --- D. plan_attention ------------------------------------------------------------

TEST(AttentionPlanTest, UnengagedPolicyPlansNoWorkAtRevisionZero) {
    const ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    const AttentionPlan plan = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)));
    EXPECT_EQ(plan.policy_revision, 0U);
    EXPECT_TRUE(plan.work.empty());
    EXPECT_TRUE(plan.resolutions.empty());
    EXPECT_TRUE(plan.reconsiderations.empty());
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
}

TEST(AttentionPlanTest, ResolvedRoiOnlyWorkEchoesImportanceWithoutIdentity) {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{10.0F, 20.0F, 5.0F, 4.0F}, CoordinateSpaceId::kFrame)})));
    ASSERT_EQ(coordinator.policy_revision(), 1U);
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    const AttentionPlan plan =
        expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60), importance_at(1.5F)));
    ASSERT_EQ(plan.work.size(), 1U);
    // Rotation k0 declares the contract shape: kFrame ROI as-is, snapshot
    // space, priority echo, and NO identity binding for an ROI-only condition
    // (both stable_id and generation are 0 per the PlannedWork contract).
    expect_focus_work(plan.work[0], RectF{10.0F, 20.0F, 5.0F, 4.0F}, 1.5F, 0U, CoordinateSpaceId::kOriented);
    ASSERT_EQ(plan.resolutions.size(), 1U);
    EXPECT_TRUE(plan.resolutions[0].resolved);
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
    EXPECT_EQ(plan.policy_revision, 1U);
}

TEST(AttentionPlanTest, BootstrapRequiresCallerAuthorizationAndUsesTheOrientedFullFrame) {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(policy_of({text_condition("Ghost")})));
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    const Frame frame = make_plan_frame(80, 60, Rotation::k90);

    const AttentionPlan unauthorized =
        expect_planned(coordinator.plan_attention(snapshot, frame, importance_at(2.0F, false)));
    EXPECT_TRUE(unauthorized.work.empty());
    ASSERT_EQ(unauthorized.resolutions.size(), 1U);
    EXPECT_FALSE(unauthorized.resolutions[0].resolved);
    EXPECT_EQ(unauthorized.resolutions[0].reason, FocusUnresolvedReason::kNotObserved);

    const AttentionPlan authorized =
        expect_planned(coordinator.plan_attention(snapshot, frame, importance_at(2.0F, true)));
    ASSERT_EQ(authorized.work.size(), 1U);
    // The oriented full frame of an 80x60 raw frame at k90 swaps the sides.
    expect_bootstrap_work(authorized.work[0], RectF{0.0F, 0.0F, 60.0F, 80.0F}, 2.0F, 0U, CoordinateSpaceId::kOriented);
    EXPECT_EQ(authorized.resolutions, unauthorized.resolutions);
    EXPECT_EQ(authorized.coverage, CoverageState::kIncomplete);
}

TEST(AttentionPlanTest, NonBootstrapUnresolvedReasonsNeverProduceWork) {
    const ImportancePolicy bootstrap = importance_at(1.0F, true);

    // kStaleGeneration without a tracker.
    ContinuousPerception stale = make_coordinator();
    expect_engaged(stale.set_attention_policy(policy_of({id_condition(11, 6)})));
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    const AttentionPlan stale_plan = expect_planned(stale.plan_attention(snapshot, make_plan_frame(80, 60), bootstrap));
    EXPECT_TRUE(stale_plan.work.empty());
    ASSERT_EQ(stale_plan.resolutions.size(), 1U);
    EXPECT_EQ(stale_plan.resolutions[0].reason, FocusUnresolvedReason::kStaleGeneration);

    // kMissingLocalization from a lost track.
    const TrackerRig lost_rig = make_track_rig(11, kRigTrackBounds, TrackState::kLost);
    ContinuousPerception lost = make_coordinator();
    lost.attach_tracker(&lost_rig.tracker);
    expect_engaged(lost.set_attention_policy(policy_of({id_condition(11, 7)})));
    const AttentionPlan lost_plan = expect_planned(lost.plan_attention(snapshot, make_plan_frame(80, 60), bootstrap));
    EXPECT_TRUE(lost_plan.work.empty());
    ASSERT_EQ(lost_plan.resolutions.size(), 1U);
    EXPECT_EQ(lost_plan.resolutions[0].reason, FocusUnresolvedReason::kMissingLocalization);

    // kUnknownStableId without any tracker.
    ContinuousPerception unknown = make_coordinator();
    expect_engaged(unknown.set_attention_policy(policy_of({id_condition(11, 7)})));
    const AttentionPlan unknown_plan =
        expect_planned(unknown.plan_attention(snapshot, make_plan_frame(80, 60), bootstrap));
    EXPECT_TRUE(unknown_plan.work.empty());
    ASSERT_EQ(unknown_plan.resolutions.size(), 1U);
    EXPECT_EQ(unknown_plan.resolutions[0].reason, FocusUnresolvedReason::kUnknownStableId);
}

TEST(AttentionPlanTest, WorkOrderingIsFocusThenBootstrapThenConditionIndex) {
    ContinuousPerception coordinator = make_coordinator();
    const std::vector<AttentionCondition> conditions{
        id_condition(9, 7),                                                            // resolved from the snapshot
        text_condition("Ghost"),                                                       // bootstrap candidate
        roi_condition(RectF{0.0F, 0.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented),  // resolved ROI-only
        text_condition("Wisp"),                                                        // bootstrap candidate
    };
    expect_engaged(coordinator.set_attention_policy(policy_of(conditions)));
    const SemanticSnapshot snapshot = make_snapshot({make_region(9, RectF{2.0F, 2.0F, 4.0F, 4.0F}, "OK")}, 7);

    const AttentionPlan plan =
        expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(100, 100), importance_at(2.5F, true)));
    ASSERT_EQ(plan.work.size(), 4U);
    expect_focus_work(plan.work[0], RectF{2.0F, 2.0F, 4.0F, 4.0F}, 2.5F, 0U, CoordinateSpaceId::kOriented, 9U, 7U);
    expect_focus_work(plan.work[1], RectF{0.0F, 0.0F, 10.0F, 10.0F}, 2.5F, 2U, CoordinateSpaceId::kOriented);
    expect_bootstrap_work(plan.work[2], RectF{0.0F, 0.0F, 100.0F, 100.0F}, 2.5F, 1U, CoordinateSpaceId::kOriented);
    expect_bootstrap_work(plan.work[3], RectF{0.0F, 0.0F, 100.0F, 100.0F}, 2.5F, 3U, CoordinateSpaceId::kOriented);
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
}

TEST(AttentionPlanTest, ResolutionsRunParallelToThePolicyConditions) {
    ContinuousPerception coordinator = make_coordinator();
    const std::vector<AttentionCondition> conditions{
        roi_condition(RectF{1.0F, 1.0F, 5.0F, 5.0F}, CoordinateSpaceId::kOriented),  // resolved, no identity
        text_condition("OK"),                                                        // text match: identity echo
        id_condition(9, 7),                                                          // stable-id match
        label_condition("ghost"),                                                    // label match, untracked region
    };
    expect_engaged(coordinator.set_attention_policy(policy_of(conditions)));
    const SemanticSnapshot snapshot =
        make_snapshot({make_region(9, RectF{2.0F, 2.0F, 4.0F, 4.0F}, "OK"),
                       make_region(0, RectF{40.0F, 40.0F, 4.0F, 4.0F}, "untracked", "ghost")},
                      7);

    const AttentionPlan plan = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)));
    ASSERT_EQ(plan.resolutions.size(), conditions.size());
    EXPECT_TRUE(plan.resolutions[0].resolved);
    EXPECT_EQ(plan.resolutions[0].focus.roi, (RectF{1.0F, 1.0F, 5.0F, 5.0F}));
    EXPECT_TRUE(plan.resolutions[1].resolved);
    EXPECT_EQ(plan.resolutions[1].focus.stable_id, 9U);
    EXPECT_TRUE(plan.resolutions[2].resolved);
    EXPECT_EQ(plan.resolutions[2].focus.stable_id, 9U);
    EXPECT_EQ(plan.resolutions[2].focus.generation, 7U);
    EXPECT_TRUE(plan.resolutions[3].resolved);
    EXPECT_EQ(plan.resolutions[3].focus.stable_id, 0U);  // untracked region carries no identity

    // The work items echo the same pattern: text/label resolutions with a
    // stable id carry it with the snapshot generation; a resolution without
    // an identity (ROI-only, or an untracked region) reports zeros.
    ASSERT_EQ(plan.work.size(), 4U);
    expect_focus_work(plan.work[0], RectF{1.0F, 1.0F, 5.0F, 5.0F}, 0.0F, 0U, CoordinateSpaceId::kOriented);
    expect_focus_work(plan.work[1], RectF{2.0F, 2.0F, 4.0F, 4.0F}, 0.0F, 1U, CoordinateSpaceId::kOriented, 9U, 7U);
    expect_focus_work(plan.work[2], RectF{2.0F, 2.0F, 4.0F, 4.0F}, 0.0F, 2U, CoordinateSpaceId::kOriented, 9U, 7U);
    expect_focus_work(plan.work[3], RectF{40.0F, 40.0F, 4.0F, 4.0F}, 0.0F, 3U, CoordinateSpaceId::kOriented);
}

TEST(AttentionPlanTest, PlansEchoTheEngagedPolicyRevision) {
    ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{1.0F, 1.0F, 5.0F, 5.0F}, CoordinateSpaceId::kFrame)})));
    const AttentionPlan first = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)));
    EXPECT_EQ(first.policy_revision, 1U);

    expect_engaged(coordinator.set_attention_policy(policy_of({text_condition("Save")})));
    const AttentionPlan second = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)));
    EXPECT_EQ(second.policy_revision, 2U);
}

TEST(AttentionPlanTest, RepeatedPlansAreDeterministicWithoutStateSideEffects) {
    const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception coordinator = make_coordinator();
    coordinator.attach_tracker(&rig.tracker);
    expect_engaged(coordinator.set_attention_policy(policy_of(
        {id_condition(11, 7), roi_condition(RectF{30.0F, 30.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented)})));

    SemanticSnapshot snapshot = make_snapshot({make_region(5, RectF{50.0F, 50.0F, 8.0F, 8.0F}, "Hi")}, 7);
    apply_change(&snapshot, ChangeClassification::kPartial, {RectI{0, 0, 4, 4}, RectI{100, 100, 4, 4}});
    const SemanticSnapshot snapshot_copy = snapshot;  // NOLINT(performance-unnecessary-copy-initialization)
    const Frame frame = make_plan_frame(200, 200);
    const ImportancePolicy importance = importance_at(1.0F, true);

    const uint64_t revision_before = coordinator.policy_revision();
    const int64_t bytes_before = coordinator.coordination_owned_bytes();
    const TargetTrack* track_before = rig.tracker.find_track(11);
    ASSERT_NE(track_before, nullptr);
    const TrackState state_before = track_before->state;
    const RectF bounds_before = track_before->last_bounds;

    const AttentionPlan first = expect_planned(coordinator.plan_attention(snapshot, frame, importance));
    const AttentionPlan second = expect_planned(coordinator.plan_attention(snapshot, frame, importance));
    EXPECT_EQ(first, second);

    // Nothing on the coordinator, the snapshot or the pool moved.
    EXPECT_EQ(coordinator.policy_revision(), revision_before);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes_before);
    EXPECT_EQ(snapshot, snapshot_copy);
    EXPECT_EQ(snapshot.change.changed_regions, snapshot_copy.change.changed_regions);
    const TargetTrack* track_after = rig.tracker.find_track(11);
    ASSERT_NE(track_after, nullptr);
    EXPECT_EQ(track_after->state, state_before);
    EXPECT_EQ(track_after->last_bounds, bounds_before);
}

TEST(AttentionPlanTest, TwinCoordinatorsProduceEqualPlans) {
    const TrackerRig rig_a = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    const TrackerRig rig_b = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
    ContinuousPerception first = make_coordinator();
    ContinuousPerception second = make_coordinator();
    first.attach_tracker(&rig_a.tracker);
    second.attach_tracker(&rig_b.tracker);
    const AttentionPolicy policy = policy_of({id_condition(11, 7)});
    expect_engaged(first.set_attention_policy(policy));
    expect_engaged(second.set_attention_policy(policy));

    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kFrame);
    const AttentionPlan plan_a =
        expect_planned(first.plan_attention(snapshot, make_plan_frame(80, 60, Rotation::k90), importance_at(1.5F)));
    const AttentionPlan plan_b =
        expect_planned(second.plan_attention(snapshot, make_plan_frame(80, 60, Rotation::k90), importance_at(1.5F)));
    EXPECT_EQ(plan_a, plan_b);
}

TEST(AttentionPlanTest, DisplaySnapshotsAreRejected) {
    const ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kDisplay);

    expect_plan_rejected(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)), ErrorCode::kInvalidArgument);
}

TEST(AttentionPlanTest, UnsizedFramesAreRejected) {
    const ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    const std::array<std::pair<int32_t, int32_t>, 4> unsized{{{0, 10}, {10, 0}, {-5, 10}, {10, -5}}};

    for (const auto& [width, height] : unsized) {
        expect_plan_rejected(coordinator.plan_attention(snapshot, make_plan_frame(width, height)),
                             ErrorCode::kInvalidArgument);
    }
}

TEST(AttentionPlanTest, NonFinitePrioritiesAreRejected) {
    const ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    const std::array<float, 3> non_finite{{kNaN, kInf, -kInf}};

    for (const float priority : non_finite) {
        expect_plan_rejected(coordinator.plan_attention(snapshot, make_plan_frame(80, 60), importance_at(priority)),
                             ErrorCode::kInvalidArgument);
    }
}

// --- E. Coordinate conversion matrix ------------------------------------------------

TEST(AttentionPlanCoordinatesTest, RoiOnlyFocusConvertsAcrossAllRotations) {
    // Frame-space ROI of an 80x60 raw frame, hand-computed per rotation:
    // k90 maps [x, x+w) x [y, y+h) onto [h-y-h, h-y) x [x, x+w), k270 onto
    // [y, y+h) x [w-x-w, w-x), k180 mirrors both axes.
    const std::array<std::pair<Rotation, RectF>, 4> cases{{
        {Rotation::k0, RectF{10.0F, 20.0F, 5.0F, 4.0F}},
        {Rotation::k90, RectF{36.0F, 10.0F, 4.0F, 5.0F}},
        {Rotation::k180, RectF{65.0F, 36.0F, 5.0F, 4.0F}},
        {Rotation::k270, RectF{20.0F, 65.0F, 4.0F, 5.0F}},
    }};
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    for (const auto& [rotation, expected_roi] : cases) {
        ContinuousPerception coordinator = make_coordinator();
        expect_engaged(coordinator.set_attention_policy(
            policy_of({roi_condition(RectF{10.0F, 20.0F, 5.0F, 4.0F}, CoordinateSpaceId::kFrame)})));
        const AttentionPlan plan = expect_planned(
            coordinator.plan_attention(snapshot, make_plan_frame(80, 60, rotation), importance_at(1.5F)));
        ASSERT_EQ(plan.work.size(), 1U);
        expect_focus_work(plan.work[0], expected_roi, 1.5F, 0U, CoordinateSpaceId::kOriented);
    }
}

TEST(AttentionPlanCoordinatesTest, TrackerFocusConvertsBackAcrossAllRotations) {
    // The pool reports kRigTrackBounds in kOriented; the plan converts them
    // into the kFrame snapshot space through the inverse rotation.
    const std::array<std::pair<Rotation, RectF>, 4> cases{{
        {Rotation::k0, RectF{10.0F, 20.0F, 5.0F, 4.0F}},
        {Rotation::k90, RectF{20.0F, 45.0F, 4.0F, 5.0F}},
        {Rotation::k180, RectF{65.0F, 36.0F, 5.0F, 4.0F}},
        {Rotation::k270, RectF{56.0F, 10.0F, 4.0F, 5.0F}},
    }};
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kFrame);

    for (const auto& [rotation, expected_roi] : cases) {
        const TrackerRig rig = make_track_rig(11, kRigTrackBounds, TrackState::kTracking);
        ContinuousPerception coordinator = make_coordinator();
        coordinator.attach_tracker(&rig.tracker);
        expect_engaged(coordinator.set_attention_policy(policy_of({id_condition(11, 7)})));
        const AttentionPlan plan = expect_planned(
            coordinator.plan_attention(snapshot, make_plan_frame(80, 60, rotation), importance_at(1.5F)));
        ASSERT_EQ(plan.work.size(), 1U);
        expect_focus_work(plan.work[0], expected_roi, 1.5F, 0U, CoordinateSpaceId::kFrame, 11U, 7U);
    }
}

TEST(AttentionPlanCoordinatesTest, GlobalReconsiderationFullFrameSwapsWithTheOrientedSpace) {
    const ContinuousPerception coordinator = make_coordinator();

    SemanticSnapshot oriented = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    apply_change(&oriented, ChangeClassification::kGlobal, {RectI{1, 1, 2, 2}});
    const AttentionPlan oriented_plan =
        expect_planned(coordinator.plan_attention(oriented, make_plan_frame(80, 60, Rotation::k90)));
    ASSERT_EQ(oriented_plan.reconsiderations.size(), 1U);
    EXPECT_EQ(oriented_plan.reconsiderations[0].changed_region, (RectF{0.0F, 0.0F, 60.0F, 80.0F}));
    EXPECT_EQ(oriented_plan.reconsiderations[0].space, CoordinateSpaceId::kOriented);
    EXPECT_TRUE(oriented_plan.reconsiderations[0].global);

    SemanticSnapshot frame_space = make_snapshot({}, 7, CoordinateSpaceId::kFrame);
    apply_change(&frame_space, ChangeClassification::kGlobal, {RectI{1, 1, 2, 2}});
    const AttentionPlan frame_plan =
        expect_planned(coordinator.plan_attention(frame_space, make_plan_frame(80, 60, Rotation::k90)));
    ASSERT_EQ(frame_plan.reconsiderations.size(), 1U);
    EXPECT_EQ(frame_plan.reconsiderations[0].changed_region, (RectF{0.0F, 0.0F, 80.0F, 60.0F}));
    EXPECT_TRUE(frame_plan.reconsiderations[0].global);
}

// --- F. Peripheral monitoring ----------------------------------------------------

TEST(PeripheralMonitoringTest, NoneChangeYieldsNoReconsiderations) {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{0.0F, 0.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented)})));
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);  // kNone by default

    const AttentionPlan plan = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)));
    EXPECT_TRUE(plan.reconsiderations.empty());
}

TEST(PeripheralMonitoringTest, GlobalChangeCollapsesToOneFullFrameRecordIgnoringRegions) {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{0.0F, 0.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented)})));
    SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kFrame);
    apply_change(&snapshot, ChangeClassification::kGlobal, {RectI{1, 1, 2, 2}, RectI{50, 50, 3, 3}});

    const AttentionPlan plan = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)));
    ASSERT_EQ(plan.reconsiderations.size(), 1U);
    EXPECT_EQ(plan.reconsiderations[0].changed_region, (RectF{0.0F, 0.0F, 80.0F, 60.0F}));
    EXPECT_EQ(plan.reconsiderations[0].space, CoordinateSpaceId::kFrame);
    EXPECT_TRUE(plan.reconsiderations[0].global);
}

TEST(PeripheralMonitoringTest, PartialRegionsOutsideEveryFocusAreRecordedInReportOrder) {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{0.0F, 0.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented)})));
    SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    // Region [1] intersects the focus (25 px) and is suppressed; [0] and [2]
    // stay peripheral and keep the report order.
    apply_change(&snapshot, ChangeClassification::kPartial,
                 {RectI{100, 100, 5, 5}, RectI{5, 5, 4, 4}, RectI{50, 50, 2, 2}});

    const AttentionPlan plan =
        expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(200, 200, Rotation::k0)));
    ASSERT_EQ(plan.reconsiderations.size(), 2U);
    EXPECT_EQ(plan.reconsiderations[0].changed_region, (RectF{100.0F, 100.0F, 5.0F, 5.0F}));
    EXPECT_EQ(plan.reconsiderations[1].changed_region, (RectF{50.0F, 50.0F, 2.0F, 2.0F}));
    EXPECT_EQ(plan.reconsiderations[0].space, CoordinateSpaceId::kOriented);
    EXPECT_FALSE(plan.reconsiderations[0].global);
    EXPECT_FALSE(plan.reconsiderations[1].global);
}

TEST(PeripheralMonitoringTest, TouchingAndDegenerateRegionsStayPeripheral) {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{0.0F, 0.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented)})));
    SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    // [0] overlaps the focus by 25 px (suppressed); [1] only touches the focus
    // edge (intersection area 0 -> peripheral); [2] is degenerate (area 0 ->
    // peripheral).
    apply_change(&snapshot, ChangeClassification::kPartial,
                 {RectI{5, 5, 10, 10}, RectI{10, 5, 5, 5}, RectI{5, 5, 0, 4}});

    const AttentionPlan plan = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(80, 60)));
    ASSERT_EQ(plan.reconsiderations.size(), 2U);
    EXPECT_EQ(plan.reconsiderations[0].changed_region, (RectF{10.0F, 5.0F, 5.0F, 5.0F}));
    EXPECT_EQ(plan.reconsiderations[1].changed_region, (RectF{5.0F, 5.0F, 0.0F, 4.0F}));
}

TEST(PeripheralMonitoringTest, PeripheralMonitoringRunsWithoutAnEngagedPolicy) {
    const ContinuousPerception coordinator = make_coordinator();
    SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    apply_change(&snapshot, ChangeClassification::kPartial, {RectI{100, 100, 5, 5}});

    const AttentionPlan plan = expect_planned(coordinator.plan_attention(snapshot, make_plan_frame(200, 200)));
    EXPECT_TRUE(plan.work.empty());
    ASSERT_EQ(plan.reconsiderations.size(), 1U);
    EXPECT_EQ(plan.reconsiderations[0].changed_region, (RectF{100.0F, 100.0F, 5.0F, 5.0F}));
}

// --- G. Cancellation, deadline and validation order -------------------------------

ExecutionContext cancelled_context() {
    ExecutionContext context;
    context.is_cancelled = [] { return true; };
    return context;
}

ExecutionContext expired_context() {
    ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    return context;
}

ContinuousPerception coordinator_with_roi_policy() {
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{0.0F, 0.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented)})));
    return coordinator;
}

TEST(AttentionPlanContextTest, CancellationConvertsToKCancelled) {
    const ContinuousPerception coordinator = coordinator_with_roi_policy();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    expect_plan_rejected(
        coordinator.plan_attention(snapshot, make_plan_frame(80, 60), ImportancePolicy{}, cancelled_context()),
        ErrorCode::kCancelled);
}

TEST(AttentionPlanContextTest, ExpiredDeadlineConvertsToKTimeout) {
    const ContinuousPerception coordinator = coordinator_with_roi_policy();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    expect_plan_rejected(
        coordinator.plan_attention(snapshot, make_plan_frame(80, 60), ImportancePolicy{}, expired_context()),
        ErrorCode::kTimeout);
}

TEST(AttentionPlanContextTest, CancellationPrecedesTheDeadline) {
    const ContinuousPerception coordinator = coordinator_with_roi_policy();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    ExecutionContext both = cancelled_context();
    both.deadline = expired_context().deadline;

    expect_plan_rejected(coordinator.plan_attention(snapshot, make_plan_frame(80, 60), ImportancePolicy{}, both),
                         ErrorCode::kCancelled);
}

TEST(AttentionPlanContextTest, AnIdlePlanNeverPollsTheContext) {
    // No conditions and a kNone change report: the plan never reaches a poll
    // point, so even a cancelled context produces the full empty plan.
    const ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);

    const AttentionPlan plan = expect_planned(
        coordinator.plan_attention(snapshot, make_plan_frame(80, 60), ImportancePolicy{}, cancelled_context()));
    EXPECT_TRUE(plan.work.empty());
    EXPECT_TRUE(plan.resolutions.empty());
    EXPECT_TRUE(plan.reconsiderations.empty());
}

TEST(AttentionPlanContextTest, TheRegionScanPollsTheContext) {
    // With no policy the only poll point is the changed-region scan: one
    // kPartial region is enough to convert cancellation and deadlines.
    const ContinuousPerception coordinator = make_coordinator();
    SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kOriented);
    apply_change(&snapshot, ChangeClassification::kPartial, {RectI{50, 50, 5, 5}});

    expect_plan_rejected(
        coordinator.plan_attention(snapshot, make_plan_frame(200, 200), ImportancePolicy{}, cancelled_context()),
        ErrorCode::kCancelled);
    expect_plan_rejected(
        coordinator.plan_attention(snapshot, make_plan_frame(200, 200), ImportancePolicy{}, expired_context()),
        ErrorCode::kTimeout);
}

TEST(AttentionPlanContextTest, InputValidationPrecedesTheFirstContextPoll) {
    const ContinuousPerception coordinator = make_coordinator();
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kDisplay);

    expect_plan_rejected(
        coordinator.plan_attention(snapshot, make_plan_frame(80, 60), ImportancePolicy{}, cancelled_context()),
        ErrorCode::kInvalidArgument);
}

}  // namespace
