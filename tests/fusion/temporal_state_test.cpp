// M9-05 temporal state and event surface tests (DEC-026 sections 6/8,
// Experimental): the per-entity state machine over accepted cycles. Covering
// the observation validity rules (token-backed observations advance only at a
// committed accept, covered observations advance at plan time with
// kValidatedReuse, everything else is unanalyzed, zero stable ids never enter
// the machine, and the plan/accept frame fences make repeated looks at one
// frame count once), the kContinuousConfirmationWindow debounce (one valid
// observation never confirms, two do, an unanalyzed gap restarts the streak,
// a single noisy observation and a replaced or cleared candidate stay
// silent), the event axes (kTextChanged / kAppearanceChanged over the
// confirmed text and label, kRelationChanged over the part-of hierarchy),
// the disappearance channel (bare covered-missing only, occlusion retaining
// the identity with its relation event, tracker loss as a silent queryable
// state), the notification filter keeping internal state evolving while
// gating publication (AC9-08), the source-reset termination of open
// lifecycles with explicit kSourceReset ends that bypass the filter and
// survive in a drainable queue (AC9-11), the bounded event drain with
// overflow drops, counters and the one-shot resync flag (AC9-09), the
// focus-switch query branches (unknown id, stale generation, confirmed-layer
// text/label and same-space ROI matching), the entity cap with closed-
// lifecycle eviction and counted refusals, the byte accounting of the new
// coordination face (zero point, size-counted queue) and the whole-batch
// region geometry rejection (DEC-021).
//
// The fixtures follow the cycle-binding house style: fake snapshots and
// hand-painted RGBA frames, no backend, no model, no threads. The suite
// originally carried four red pins for verification defects (stale presence
// streaks across absences, present-but-uncovered staleness, zero-size region
// events); the implementation now satisfies those contracts and the tests pin
// them directly.

#include <mirador/continuous_perception.hpp>

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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::AttentionCondition;
using mirador::AttentionPolicy;
using mirador::ContinuousPerception;
using mirador::ContinuousPerceptionOptions;
using mirador::CoordinateSpaceId;
using mirador::EventDrain;
using mirador::EventSemantic;
using mirador::ExecutionContext;
using mirador::FocusStateResolution;
using mirador::FocusUnresolvedReason;
using mirador::Frame;
using mirador::Freshness;
using mirador::ImportancePolicy;
using mirador::kContinuousConfirmationWindow;
using mirador::ObservationState;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::RectF;
using mirador::Result;
using mirador::Rotation;
using mirador::SemanticChangeKind;
using mirador::SemanticEvent;
using mirador::SemanticSnapshot;
using mirador::SourceResetReport;
using mirador::TemporalEntityState;
using mirador::TemporalSemanticState;
using mirador::VisualRegion;
using mirador::WorkAcceptReport;
using mirador::WorkPlan;
using mirador::WorkRejectionReason;
using mirador::WorkToken;

// --- Shared makers and assertion helpers ------------------------------------

constexpr RectF kFocusA{0.0F, 0.0F, 24.0F, 24.0F};
constexpr RectF kFocusB{26.0F, 26.0F, 8.0F, 8.0F};  // disjoint from kFocusA
constexpr RectF kInside{2.0F, 2.0F, 4.0F, 4.0F};    // strictly inside kFocusA
constexpr RectF kFar{30.0F, 40.0F, 8.0F, 8.0F};     // outside both foci

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
    snapshot.generation = generation;
    snapshot.coordinate_space = space;
    snapshot.regions = std::move(regions);
    return snapshot;
}

ContinuousPerceptionOptions options_with_source(std::string source_id) {
    ContinuousPerceptionOptions options;
    options.source_id = std::move(source_id);
    return options;
}

ContinuousPerception make_coordinator(const ContinuousPerceptionOptions& options = ContinuousPerceptionOptions{}) {
    auto created = ContinuousPerception::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_coordinator: " << created.status().message();
        return ContinuousPerception{};
    }
    return created.take_value();
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

AttentionPolicy policy_of(std::vector<AttentionCondition> conditions) {
    AttentionPolicy policy;
    policy.conditions = std::move(conditions);
    return policy;
}

AttentionPolicy focus_policy(RectF roi) {
    return policy_of({roi_condition(roi, CoordinateSpaceId::kOriented)});
}

ImportancePolicy silent_importance() {
    ImportancePolicy importance;
    importance.notify_enabled = false;
    return importance;
}

void expect_engaged(const Result<void>& engaged) {
    EXPECT_TRUE(engaged.ok()) << (engaged.ok() ? "" : engaged.status().message());
}

WorkPlan expect_cycle_planned(const Result<WorkPlan>& result) {
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.status().message());
    if (!result.ok()) {
        return WorkPlan{};
    }
    return result.value();
}

WorkAcceptReport expect_batch_accepted(const Result<WorkAcceptReport>& result) {
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.status().message());
    if (!result.ok()) {
        return WorkAcceptReport{};
    }
    EXPECT_TRUE(result.value().committed);
    return result.value();
}

std::vector<WorkToken> tokens_of(const WorkPlan& plan) {
    std::vector<WorkToken> tokens;
    tokens.reserve(plan.work.size());
    for (const auto& binding : plan.work) {
        tokens.push_back(binding.token);
    }
    return tokens;
}

/// Drains the queue and checks the frozen structural invariants of every
/// event: producer-side consistency (pairing, window order, bounds rule) and
/// the ascending gap-free sequence of a queue that never dropped.
EventDrain expect_drained(ContinuousPerception* coordinator) {
    const Result<EventDrain> drained = coordinator->drain_events();
    EXPECT_TRUE(drained.ok()) << (drained.ok() ? "" : drained.status().message());
    if (!drained.ok()) {
        return EventDrain{};
    }
    for (const SemanticEvent& event : drained.value().events) {
        EXPECT_TRUE(mirador::semantic_event_consistent(event))
            << "event " << event.event_sequence << " violates the frozen structural invariants";
    }
    for (size_t index = 1; index < drained.value().events.size(); ++index) {
        EXPECT_LT(drained.value().events[index - 1].event_sequence, drained.value().events[index].event_sequence)
            << "drain is not ascending by event_sequence";
    }
    return drained.value();
}

/// The one queued event, field-checked against the frozen shape.
void expect_single_event(const EventDrain& drain, EventSemantic semantic, SemanticChangeKind kind, uint64_t stable_id,
                         uint64_t instance_generation, uint64_t observed_frame, uint64_t confirmed_frame) {
    ASSERT_EQ(drain.events.size(), 1U);
    const SemanticEvent& event = drain.events.front();
    EXPECT_TRUE(event.semantic == semantic && event.change_kind == kind);
    EXPECT_EQ(event.stable_id, stable_id);
    EXPECT_EQ(event.instance_generation, instance_generation);
    EXPECT_TRUE(event.window.observed_frame == observed_frame && event.window.confirmed_frame == confirmed_frame);
}

const TemporalEntityState* find_entity(const TemporalSemanticState& state, uint64_t stable_id) {
    for (const TemporalEntityState& entity : state.entities) {
        if (entity.stable_id == stable_id) {
            return &entity;
        }
    }
    return nullptr;
}

void expect_entity_observation(const TemporalSemanticState& state, uint64_t stable_id, ObservationState observation,
                               Freshness freshness, uint64_t observed_frame) {
    const TemporalEntityState* entity = find_entity(state, stable_id);
    ASSERT_NE(entity, nullptr) << "entity " << stable_id << " is not tracked";
    EXPECT_EQ(entity->observation, observation);
    EXPECT_EQ(entity->freshness, freshness);
    EXPECT_EQ(entity->observed_frame, observed_frame);
}

// --- Cycle frame rig (hand-painted RGBA, mirrors the cycle-binding tests) ----

struct CycleRig {
    std::vector<std::uint8_t> pixels;
    int32_t width = 48;
    int32_t height = 64;
    int64_t stride = 192;
};

CycleRig make_rig() {
    CycleRig rig;
    rig.pixels.assign(static_cast<size_t>(rig.stride) * static_cast<size_t>(rig.height), 235U);
    return rig;
}

Frame rig_frame(const CycleRig& rig, uint64_t sequence) {
    Frame frame;
    frame.image.data = reinterpret_cast<const std::byte*>(rig.pixels.data());
    frame.image.width = rig.width;
    frame.image.height = rig.height;
    frame.image.row_stride_bytes = rig.stride;
    frame.image.format = PixelFormat::kRgba8;
    frame.image.rotation = Rotation::k0;
    frame.sequence = sequence;
    return frame;
}

void rig_paint(CycleRig* rig, const RectF& region, uint8_t value) {
    const auto x0 = static_cast<int32_t>(region.x);
    const auto y0 = static_cast<int32_t>(region.y);
    const auto x1 = static_cast<int32_t>(region.x + region.width);
    const auto y1 = static_cast<int32_t>(region.y + region.height);
    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
            rig->pixels[static_cast<size_t>(y) * static_cast<size_t>(rig->stride) + static_cast<size_t>(x) * 4U] =
                value;
        }
    }
}

/// Canonical two-observation confirmation of one region inside its focus:
/// frame `base` is planned and accepted (token-backed presence 1), frame
/// `base + 1` is a static covered plan (presence 2 at plan time, begin
/// queued). The caller owns the queue from there.
void confirm_appearance(ContinuousPerception* coordinator, const SemanticSnapshot& snapshot, CycleRig* rig,
                        uint64_t base, const ImportancePolicy& importance = {}) {
    const Frame first = rig_frame(*rig, base);
    const WorkPlan plan = expect_cycle_planned(coordinator->plan_cycle(snapshot, first, importance));
    EXPECT_EQ(plan.work.size(), 1U);
    expect_batch_accepted(coordinator->accept_work(tokens_of(plan), first));
    static_cast<void>(expect_cycle_planned(coordinator->plan_cycle(snapshot, rig_frame(*rig, base + 1U), importance)));
}

// --- Tracker rig (drives the real pool to kLost, mirrors attention-plan) ----

struct TrackerRig {
    mirador::ObjectTracker tracker;
    std::vector<std::byte> pixels;
    int32_t width = 64;
    int32_t height = 64;
    int64_t stride = 64;
    mirador::ImageView view;
};

mirador::ImageView rig_view_of(TrackerRig& rig) {
    mirador::ImageView view;
    view.data = rig.pixels.data();
    view.width = rig.width;
    view.height = rig.height;
    view.row_stride_bytes = rig.stride;
    view.format = PixelFormat::kGray8;
    view.rotation = Rotation::k0;
    return view;
}

mirador::TrackVerification placeholder_verification(uint64_t track_id) {
    mirador::TrackVerification verification;
    verification.track_id = track_id;
    verification.state = mirador::TrackState::kTracking;
    verification.verification_roi = mirador::RectI{0, 0, 1, 1};
    verification.appearance.outcome = mirador::AppearanceChannelOutcome::kNone;
    verification.appearance.peak_sidelobe_ratio = 10.0;
    verification.appearance.best_template_index = 0U;
    return verification;
}

mirador::TrackPositionEvidence stationary_position() {
    mirador::TrackPositionEvidence position;
    position.scenario = mirador::PositionScenario::kStationary;
    position.inside_gate = true;
    return position;
}

/// A pool holding track `id` in the kLost state (one placeholder commit under
/// uncertain_frame_limit == 1 is the frozen transition).
TrackerRig make_lost_track_rig(uint64_t id, const RectF& bounds) {
    TrackerRig rig;
    rig.pixels.assign(static_cast<size_t>(rig.stride) * static_cast<size_t>(rig.height), std::byte{0});
    rig.view = rig_view_of(rig);
    mirador::ObjectTrackerOptions options;
    options.uncertain_frame_limit = 1;
    auto created = mirador::ObjectTracker::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_lost_track_rig: " << created.status().message();
        return rig;
    }
    rig.tracker = created.take_value();
    if (const auto adopted = rig.tracker.adopt_track(make_region(id, bounds, "OK", "btn"), rig.view, 1);
        !adopted.ok()) {
        ADD_FAILURE() << "adopt " << id << ": " << adopted.status().message();
        return rig;
    }
    if (const auto committed = rig.tracker.commit_track_evidence(id, placeholder_verification(id),
                                                                 stationary_position(), std::nullopt, rig.view, 2);
        !committed.ok()) {
        ADD_FAILURE() << "commit " << id << ": " << committed.status().message();
    }
    const mirador::TargetTrack* track = rig.tracker.find_track(id);
    if (track == nullptr || track->state != mirador::TrackState::kLost) {
        ADD_FAILURE() << "rig did not reach kLost";
    }
    return rig;
}

ExecutionContext cancelled_context() {
    ExecutionContext context;
    context.is_cancelled = [] { return true; };
    return context;
}

// --- A. Observation validity and frame fences --------------------------------

TEST(ObservationValidityTest, TokenBackedObservationAdvancesOnlyAfterAdoption) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});

    // Planned but unaccepted: the region is registered, nothing advanced.
    const Frame first = rig_frame(rig, 1U);
    const WorkPlan planned = expect_cycle_planned(coordinator.plan_cycle(snapshot, first));
    ASSERT_EQ(planned.work.size(), 1U);
    const WorkToken first_token = planned.work.front().token;
    expect_entity_observation(coordinator.query_temporal_state(), 900U, ObservationState::kUnanalyzed,
                              Freshness::kUnvalidated, 0U);
    EXPECT_EQ(coordinator.query_temporal_state().pending_events, 0U);

    // A newer plan replaces the in-flight set; the old token is stale now.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U))));
    const Result<WorkAcceptReport> stale = coordinator.accept_work({&first_token, 1}, first);
    ASSERT_TRUE(stale.ok());
    ASSERT_EQ(stale.value().item_rejections.size(), 1U);
    EXPECT_EQ(stale.value().item_rejections.front(), WorkRejectionReason::kStaleFrameSequence);
    // Rejected evidence never validates an observation.
    expect_entity_observation(coordinator.query_temporal_state(), 900U, ObservationState::kUnanalyzed,
                              Freshness::kUnvalidated, 0U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // The committed batch advances its token-backed observation (kFresh).
    const WorkPlan replanned = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    expect_batch_accepted(coordinator.accept_work(tokens_of(replanned), rig_frame(rig, 2U)));
    expect_entity_observation(coordinator.query_temporal_state(), 900U, ObservationState::kObserved, Freshness::kFresh,
                              2U);
    EXPECT_EQ(coordinator.query_temporal_state().confirmed_frame, 0U);  // one observation is not a confirmation
}

TEST(ObservationValidityTest, CoveredRegionAdvancesAtPlanTimeWithValidatedReuse) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});
    confirm_appearance(&coordinator, snapshot, &rig, 1U);

    // The static covered cycle advanced at plan time: freshness is
    // kValidatedReuse and the confirmation event is already queued.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U))));
    const TemporalSemanticState state = coordinator.query_temporal_state();
    expect_entity_observation(state, 900U, ObservationState::kObserved, Freshness::kValidatedReuse, 3U);
    EXPECT_EQ(state.latest_observed_frame, 3U);
    EXPECT_EQ(state.confirmed_frame, 2U);
    EXPECT_EQ(state.pending_events, 1U);
    // Queries are pure reads: repeated queries neither advance nor consume.
    EXPECT_EQ(coordinator.query_temporal_state(), state);
    EXPECT_EQ(coordinator.query_temporal_state().pending_events, 1U);
    const EventDrain drain = expect_drained(&coordinator);
    expect_single_event(drain, EventSemantic::kBegin, SemanticChangeKind::kAppeared, 900U, 1U, 1U, 2U);
}

TEST(ObservationValidityTest, ZeroStableIdRegionsNeverEnterTheMachine) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot anonymous = make_snapshot({make_region(0U, kFocusA, "anon")});

    const Frame first = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(anonymous, first));
    ASSERT_EQ(plan.work.size(), 1U);
    expect_batch_accepted(coordinator.accept_work(tokens_of(plan), first));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(anonymous, rig_frame(rig, 2U))));

    const TemporalSemanticState state = coordinator.query_temporal_state();
    EXPECT_TRUE(state.entities.empty());
    EXPECT_EQ(state.pending_events, 0U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
}

TEST(ObservationValidityTest, RepeatedPlanOfTheSameFrameNeverCountsTwice) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});

    // The first observation, then a re-plan of the SAME frame: the fence must
    // keep the re-look from becoming the second confirmation observation.
    const Frame first = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, first));
    expect_batch_accepted(coordinator.accept_work(tokens_of(plan), first));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, first)));
    EXPECT_EQ(coordinator.query_temporal_state().confirmed_frame, 0U);
    const TemporalSemanticState replanned_state = coordinator.query_temporal_state();
    EXPECT_EQ(find_entity(replanned_state, 900U)->observed_frame, 1U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // The confirmation therefore needs a genuinely later frame.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kBegin, SemanticChangeKind::kAppeared, 900U, 1U,
                        1U, 2U);
}

// DEFECT pin: a snapshot region that is neither inside surviving coverage nor
// inside an issued work ROI is an unanalyzed observation — the entity must
// report kUnanalyzed with kUnvalidated freshness and every debounce streak
// must reset. The implementation never advances such an item at all, so the
// query keeps reporting the stale kObserved/kValidatedReuse state of the last
// covered cycle. Fails until present-but-uncovered items are classified.
TEST(ObservationValidityTest, PresentButUncoveredCycleReportsUnanalyzed) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot settled = make_snapshot({make_region(900U, kFocusA, "t")});
    confirm_appearance(&coordinator, settled, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    // The entity moves outside every coverage entry and outside the focus:
    // the sighting is unanalyzed, never a validated-reuse carry-over.
    const SemanticSnapshot moved = make_snapshot({make_region(900U, kFar, "t")});
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(moved, rig_frame(rig, 3U))));
    expect_entity_observation(coordinator.query_temporal_state(), 900U, ObservationState::kUnanalyzed,
                              Freshness::kUnvalidated, 2U);
}

TEST(ObservationValidityTest, RepeatedAcceptOfTheSameBatchNeverCountsTwice) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});

    const Frame first = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, first));
    const std::vector<WorkToken> tokens = tokens_of(plan);
    expect_batch_accepted(coordinator.accept_work(tokens, first));
    const Result<WorkAcceptReport> duplicate = coordinator.accept_work(tokens, first);
    ASSERT_TRUE(duplicate.ok());
    ASSERT_EQ(duplicate.value().item_rejections.size(), 1U);
    EXPECT_EQ(duplicate.value().item_rejections.front(), WorkRejectionReason::kDuplicateSubmission);
    // One advanced observation, no confirmation, no event.
    EXPECT_EQ(coordinator.query_temporal_state().confirmed_frame, 0U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // The confirmation therefore happens at the second frame, not the first.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kBegin, SemanticChangeKind::kAppeared, 900U, 1U,
                        1U, 2U);
}

// --- B. Confirmation window ---------------------------------------------------

TEST(ConfirmationWindowTest, FirstValidObservationDoesNotConfirm) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});

    const Frame first = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, first));
    expect_batch_accepted(coordinator.accept_work(tokens_of(plan), first));

    const TemporalSemanticState state = coordinator.query_temporal_state();
    const TemporalEntityState* entity = find_entity(state, 900U);
    ASSERT_NE(entity, nullptr);
    EXPECT_EQ(entity->confirmed_frame, 0U);
    EXPECT_EQ(entity->instance_generation, 0U);
    EXPECT_EQ(state.confirmed_frame, 0U);
    EXPECT_EQ(state.published_frame, 0U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
}

TEST(ConfirmationWindowTest, WindowConstantIsTwo) {
    // The debounce structure is frozen; the number is pinned until M9-06.
    EXPECT_EQ(kContinuousConfirmationWindow, 2U);
}

TEST(ConfirmationWindowTest, UnanalyzedGapRestartsTheDebounceStreak) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot present = make_snapshot({make_region(900U, kFocusA, "t")});
    const SemanticSnapshot absent = make_snapshot({});
    confirm_appearance(&coordinator, present, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    // Moving the only focus away leaves the entity's area uncovered: an
    // unanalyzed cycle that must reset every streak (state kUnanalyzed).
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusB)));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(absent, rig_frame(rig, 3U))));
    expect_entity_observation(coordinator.query_temporal_state(), 900U, ObservationState::kUnanalyzed,
                              Freshness::kUnvalidated, 2U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // Back on the old focus the absent entity is token-backed; the adopted
    // batch validates the covered-missing observation (absence 1).
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    const Frame fourth = rig_frame(rig, 4U);
    const WorkPlan replanned = expect_cycle_planned(coordinator.plan_cycle(absent, fourth));
    ASSERT_EQ(replanned.work.size(), 1U);
    expect_batch_accepted(coordinator.accept_work(tokens_of(replanned), fourth));

    // The next static cycle is the second covered-missing observation: the
    // confirmed identity disappears under the coverage rule.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(absent, rig_frame(rig, 5U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kEnd, SemanticChangeKind::kDisappeared, 900U, 1U,
                        4U, 5U);

    // The streak was reset by the unanalyzed gap, so the re-appearance needs
    // two fresh observations again.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(present, rig_frame(rig, 6U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(present, rig_frame(rig, 7U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kBegin, SemanticChangeKind::kAppeared, 900U, 2U,
                        6U, 7U);
}

TEST(ConfirmationWindowTest, SingleNoisyObservationAndReplacedCandidatesStaySilent) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto labeled = [](const char* label) { return make_snapshot({make_region(900U, kFocusA, "t", label)}); };
    confirm_appearance(&coordinator, labeled("btn"), &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    // One differing observation, then back to the confirmed value: cleared.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(labeled("card"), rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(labeled("btn"), rig_frame(rig, 4U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // A candidate replaced by a different observation restarts its count.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(labeled("glitch"), rig_frame(rig, 5U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(labeled("card"), rig_frame(rig, 6U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // Only the second consistent "card" confirms, from the restarted streak.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(labeled("card"), rig_frame(rig, 7U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kAppearanceChanged,
                        900U, 1U, 6U, 7U);
}

// --- C. Change axes ------------------------------------------------------------

TEST(ChangeAxesTest, TextChangeConfirmsWithTextChangedAxis) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto with_text = [](const char* text) { return make_snapshot({make_region(900U, kFocusA, text)}); };
    confirm_appearance(&coordinator, with_text("one"), &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 4U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kTextChanged, 900U,
                        1U, 3U, 4U);
    EXPECT_EQ(coordinator.query_temporal_state().confirmed_frame, 4U);
}

TEST(ChangeAxesTest, LabelChangeConfirmsWithAppearanceChangedAxis) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto labeled = [](const char* label) { return make_snapshot({make_region(900U, kFocusA, "t", label)}); };
    confirm_appearance(&coordinator, labeled("btn"), &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(labeled("icon"), rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(labeled("icon"), rig_frame(rig, 4U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kAppearanceChanged,
                        900U, 1U, 3U, 4U);
}

/// Both axis updates of one confirmation share the frozen window shape.
void expect_axis_update(const SemanticEvent& event, SemanticChangeKind kind) {
    EXPECT_EQ(event.change_kind, kind);
    EXPECT_EQ(event.semantic, EventSemantic::kUpdate);
    EXPECT_TRUE(event.window.observed_frame == 3U && event.window.confirmed_frame == 4U);
}

TEST(ChangeAxesTest, SimultaneousTextAndLabelChangeEmitBothAxes) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto with = [](const char* text, const char* label) {
        return make_snapshot({make_region(900U, kFocusA, text, label)});
    };
    confirm_appearance(&coordinator, with("one", "btn"), &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with("two", "icon"), rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with("two", "icon"), rig_frame(rig, 4U))));
    const EventDrain drain = expect_drained(&coordinator);
    ASSERT_EQ(drain.events.size(), 2U);
    // One update event per changed axis, both confirmed by the same window.
    // The text axis is published before the appearance axis.
    expect_axis_update(drain.events[0], SemanticChangeKind::kTextChanged);
    expect_axis_update(drain.events[1], SemanticChangeKind::kAppearanceChanged);
}

TEST(ChangeAxesTest, HierarchyChangeConfirmsWithRelationChangedAxis) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    // The focus covers the child only, so the parent stays present-but-
    // untracked and contributes its part-of edge without any events of its own.
    expect_engaged(coordinator.set_attention_policy(focus_policy(kInside)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kInside, 40U);
    const auto nested = [](bool with_parent) {
        std::vector<VisualRegion> regions{make_region(900U, kInside)};
        if (with_parent) {
            regions.push_back(make_region(901U, kFocusA));
        }
        return make_snapshot(std::move(regions));
    };
    confirm_appearance(&coordinator, nested(true), &rig, 1U);
    expect_single_event(expect_drained(&coordinator), EventSemantic::kBegin, SemanticChangeKind::kAppeared, 900U, 1U,
                        1U, 2U);

    // The parent leaves: the child's part-of parent set changes twice.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(nested(false), rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(nested(false), rig_frame(rig, 4U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kRelationChanged,
                        900U, 1U, 3U, 4U);

    // The parent returns: the restoration is a relation change of its own.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(nested(true), rig_frame(rig, 5U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(nested(true), rig_frame(rig, 6U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kRelationChanged,
                        900U, 1U, 5U, 6U);
}

// --- D. Disappearance and occlusion ---------------------------------------------

TEST(DisappearanceTest, BareCoveredMissingConfirmsDisappearanceAfterTheWindow) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot present = make_snapshot({make_region(900U, kFocusA, "t")});
    const SemanticSnapshot absent = make_snapshot({});
    confirm_appearance(&coordinator, present, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(absent, rig_frame(rig, 3U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);  // one missing frame is not a disappearance
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(absent, rig_frame(rig, 4U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kEnd, SemanticChangeKind::kDisappeared, 900U, 1U,
                        3U, 4U);

    // The lifecycle is closed: further absences stay silent, the identity
    // stays queryable with its instance generation.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(absent, rig_frame(rig, 5U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
    const TemporalSemanticState closed_state = coordinator.query_temporal_state();
    const TemporalEntityState* entity = find_entity(closed_state, 900U);
    ASSERT_NE(entity, nullptr);
    EXPECT_EQ(entity->instance_generation, 1U);
    EXPECT_EQ(coordinator.query_temporal_state().confirmed_frame, 2U);
}

TEST(DisappearanceTest, UncoveredMissingNeverConfirmsDisappearance) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot present = make_snapshot({make_region(900U, kFocusA, "t")});
    confirm_appearance(&coordinator, present, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    // The focus moves away: the entity's area is no longer validly covered,
    // so every absence stays unknown and no end may ever confirm.
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusB)));
    for (const uint64_t frame : {3U, 4U, 5U, 6U}) {
        static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(make_snapshot({}), rig_frame(rig, frame))));
        expect_entity_observation(coordinator.query_temporal_state(), 900U, ObservationState::kUnanalyzed,
                                  Freshness::kUnvalidated, 2U);
        EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
    }
    const TemporalSemanticState surviving = coordinator.query_temporal_state();
    const TemporalEntityState* entity = find_entity(surviving, 900U);
    ASSERT_NE(entity, nullptr);
    EXPECT_EQ(entity->instance_generation, 1U);  // lifecycle never closed
}

TEST(DisappearanceTest, OccludedMissingRetainsIdentityAndEmitsOneRelationChange) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot present = make_snapshot({make_region(900U, kFocusA, "t")});
    // An anonymous region (stable id 0) inside the entity bounds: an
    // occluder that itself never enters the state machine.
    const SemanticSnapshot occluded = make_snapshot({make_region(0U, kInside)});
    confirm_appearance(&coordinator, present, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(occluded, rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(occluded, rig_frame(rig, 4U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kRelationChanged,
                        900U, 1U, 3U, 4U);
    const TemporalSemanticState occluded_state = coordinator.query_temporal_state();
    const TemporalEntityState* occluding = find_entity(occluded_state, 900U);
    ASSERT_NE(occluding, nullptr);
    EXPECT_EQ(occluding->observation, ObservationState::kOccluded);
    EXPECT_EQ(occluding->bounds, kFocusA);  // identity and position retained

    // Persistent occlusion never repeats the event.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(occluded, rig_frame(rig, 5U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
    const TemporalSemanticState still_occluded = coordinator.query_temporal_state();
    EXPECT_EQ(find_entity(still_occluded, 900U)->observation, ObservationState::kOccluded);
}

// The frozen window applies to the occlusion flip-back too: the two occluded
// cycles confirm the relation change, and flipping back to observed needs two
// consecutive re-observations of its own, with the re-observation window.
TEST(DisappearanceTest, OcclusionFlipBackWaitsForTwoReobservations) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot present = make_snapshot({make_region(900U, kFocusA, "t")});
    const SemanticSnapshot occluded = make_snapshot({make_region(0U, kInside)});
    confirm_appearance(&coordinator, present, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(occluded, rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(occluded, rig_frame(rig, 4U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kRelationChanged,
                        900U, 1U, 3U, 4U);

    // First re-observation: not yet a confirmed relation change.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(present, rig_frame(rig, 5U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
    // Second re-observation confirms the flip with the re-observation window.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(present, rig_frame(rig, 6U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kRelationChanged,
                        900U, 1U, 5U, 6U);
    const TemporalSemanticState flipped = coordinator.query_temporal_state();
    EXPECT_EQ(find_entity(flipped, 900U)->observation, ObservationState::kObserved);
}

// A re-appearing identity is a new appearance: instance generation 2 opens
// only after the confirmation window of its own, carrying the re-observation
// window instead of any pre-disappearance frame.
TEST(DisappearanceTest, ReappearanceAfterDisappearanceOpensANewInstanceAfterTheWindow) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot present = make_snapshot({make_region(900U, kFocusA, "t")});
    const SemanticSnapshot absent = make_snapshot({});
    confirm_appearance(&coordinator, present, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(absent, rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(absent, rig_frame(rig, 4U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kEnd, SemanticChangeKind::kDisappeared, 900U, 1U,
                        3U, 4U);

    // First re-observation: still inside the confirmation window.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(present, rig_frame(rig, 5U))));
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
    // Second re-observation opens instance generation 2 with its own window.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(present, rig_frame(rig, 6U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kBegin, SemanticChangeKind::kAppeared, 900U, 2U,
                        5U, 6U);
    const TemporalSemanticState reopened = coordinator.query_temporal_state();
    const TemporalEntityState* entity = find_entity(reopened, 900U);
    ASSERT_NE(entity, nullptr);
    EXPECT_EQ(entity->instance_generation, 2U);
    EXPECT_EQ(entity->confirmed_frame, 6U);
}

/// One lost-track cycle: the identity is queryable in its own state, the
/// lifecycle stays untouched, and nothing is published.
void expect_lost_cycle(ContinuousPerception* coordinator, const SemanticSnapshot& absent, const CycleRig& rig,
                       uint64_t frame) {
    static_cast<void>(expect_cycle_planned(coordinator->plan_cycle(absent, rig_frame(rig, frame))));
    const TemporalSemanticState lost_state = coordinator->query_temporal_state();
    const TemporalEntityState* entity = find_entity(lost_state, 900U);
    ASSERT_NE(entity, nullptr);
    EXPECT_EQ(entity->observation, ObservationState::kTrackingLost);
    EXPECT_EQ(entity->freshness, Freshness::kUnvalidated);
    EXPECT_EQ(entity->confirmed_frame, 2U);  // lifecycle untouched
    EXPECT_EQ(expect_drained(coordinator).events.size(), 0U);
}

TEST(DisappearanceTest, TrackerLossIsASilentQueryableState) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    const TrackerRig tracker_rig = make_lost_track_rig(900U, kFocusA);
    coordinator.attach_tracker(&tracker_rig.tracker);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot present = make_snapshot({make_region(900U, kFocusA, "t")});
    const SemanticSnapshot absent = make_snapshot({});
    confirm_appearance(&coordinator, present, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    // The lost track claims no position: its own queryable state, and no
    // disappearance step in either direction.
    expect_lost_cycle(&coordinator, absent, rig, 3U);
    expect_lost_cycle(&coordinator, absent, rig, 4U);
}

// --- E. Notification filter (AC9-08) ---------------------------------------------

TEST(NotificationFilterTest, FilteredCyclesKeepEvolvingConfirmedStateWithoutEvents) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto with_text = [](const char* text) { return make_snapshot({make_region(900U, kFocusA, text)}); };
    const ImportancePolicy silent = silent_importance();
    confirm_appearance(&coordinator, with_text("one"), &rig, 1U, silent);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // The confirmation and a text change advanced internally, silently.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 3U), silent)));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 4U), silent)));
    const TemporalSemanticState filtered = coordinator.query_temporal_state();
    EXPECT_EQ(filtered.confirmed_frame, 4U);
    EXPECT_EQ(filtered.entities.front().instance_generation, 1U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // The filter is per cycle: a notified cycle publishes again.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("three"), rig_frame(rig, 5U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("three"), rig_frame(rig, 6U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kUpdate, SemanticChangeKind::kTextChanged, 900U,
                        1U, 5U, 6U);
}

TEST(NotificationFilterTest, SourceResetEndsBypassTheFilter) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});
    confirm_appearance(&coordinator, snapshot, &rig, 1U, silent_importance());
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);

    // The epoch termination is a lifecycle fact, not a notification.
    static_cast<void>(coordinator.notify_source_reset());
    expect_single_event(expect_drained(&coordinator), EventSemantic::kEnd, SemanticChangeKind::kSourceReset, 900U, 1U,
                        0U, 0U);
}

// --- F. Source reset termination (AC9-11) ------------------------------------------

/// One explicit reset termination: sequence-ordered, lifecycle-closing, zero
/// bounds and zero window frames under the OLD epoch — never a disappearance.
void expect_reset_end(const SemanticEvent& event, uint64_t stable_id, uint64_t sequence) {
    EXPECT_EQ(event.event_sequence, sequence);
    EXPECT_EQ(event.stable_id, stable_id);
    EXPECT_TRUE(event.semantic == EventSemantic::kEnd && event.change_kind == SemanticChangeKind::kSourceReset);
    EXPECT_EQ(event.bounds, RectF{});
    EXPECT_EQ(event.window.source_epoch, 1U);
    EXPECT_TRUE(event.window.observed_frame == 0U && event.window.confirmed_frame == 0U);
}

TEST(SourceResetTerminationTest, ResetEndsEveryOpenLifecycleExplicitly) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(policy_of(
        {roi_condition(kFocusA, CoordinateSpaceId::kOriented), roi_condition(kFocusB, CoordinateSpaceId::kOriented)})));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    rig_paint(&rig, kFocusB, 60U);
    const SemanticSnapshot duo = make_snapshot({make_region(900U, kFocusA, "a"), make_region(901U, kFocusB, "b")});

    const Frame first = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(duo, first));
    ASSERT_EQ(plan.work.size(), 2U);
    expect_batch_accepted(coordinator.accept_work(tokens_of(plan), first));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(duo, rig_frame(rig, 2U))));
    const EventDrain begins = expect_drained(&coordinator);
    ASSERT_EQ(begins.events.size(), 2U);

    const SourceResetReport report = coordinator.notify_source_reset();
    EXPECT_EQ(report.previous_epoch, 1U);
    EXPECT_EQ(report.new_epoch, 2U);

    // Both open lifecycles terminate explicitly, in ascending stable-id order.
    const EventDrain drain = expect_drained(&coordinator);
    ASSERT_EQ(drain.events.size(), 2U);
    const uint64_t sequence_after_begins = begins.events.back().event_sequence + 1U;
    expect_reset_end(drain.events[0], 900U, sequence_after_begins);
    expect_reset_end(drain.events[1], 901U, sequence_after_begins + 1U);
}

TEST(SourceResetTerminationTest, ResetClearsCycleStateButKeepsTheQueueDrainable) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});
    confirm_appearance(&coordinator, snapshot, &rig, 1U);

    // The queue survives the reset: the pending begin stays drainable next
    // to the termination event.
    static_cast<void>(coordinator.notify_source_reset());
    const TemporalSemanticState state = coordinator.query_temporal_state();
    EXPECT_TRUE(state.entities.empty());
    EXPECT_EQ(state.latest_observed_frame, 0U);
    EXPECT_EQ(state.confirmed_frame, 0U);
    EXPECT_EQ(state.pending_events, 2U);

    const EventDrain drain = expect_drained(&coordinator);
    ASSERT_EQ(drain.events.size(), 2U);
    EXPECT_EQ(drain.events[0].change_kind, SemanticChangeKind::kAppeared);
    EXPECT_EQ(drain.events[0].window, (mirador::ObservationWindow{"cam", 1U, 1U, 2U}));
    EXPECT_EQ(drain.events[1].change_kind, SemanticChangeKind::kSourceReset);
    EXPECT_EQ(coordinator.query_temporal_state().pending_events, 0U);

    // The frame fences cleared: frame sequences restart under the new epoch,
    // and the resync report is one-shot.
    const Frame first = rig_frame(rig, 1U);
    const WorkPlan replanned = expect_cycle_planned(coordinator.plan_cycle(snapshot, first));
    EXPECT_TRUE(replanned.resync_required);
    ASSERT_EQ(replanned.work.size(), 1U);
    EXPECT_EQ(replanned.work.front().token.source_epoch, 2U);
    expect_batch_accepted(coordinator.accept_work(tokens_of(replanned), first));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U))));
    expect_single_event(expect_drained(&coordinator), EventSemantic::kBegin, SemanticChangeKind::kAppeared, 900U, 1U,
                        1U, 2U);
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U))));
    EXPECT_FALSE(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 4U))).resync_required);
}

// --- G. Event queue and drain (AC9-09) -----------------------------------------------

/// The frozen three-event history of one entity: gap-free sequences from 1
/// and the begin / text / appearance axis order.
void expect_confirmed_history(const EventDrain& drain) {
    ASSERT_EQ(drain.events.size(), 3U);
    const std::vector<SemanticChangeKind> kinds{SemanticChangeKind::kAppeared, SemanticChangeKind::kTextChanged,
                                                SemanticChangeKind::kAppearanceChanged};
    for (size_t index = 0; index < kinds.size(); ++index) {
        EXPECT_EQ(drain.events[index].event_sequence, index + 1U);  // gap-free from 1
        EXPECT_EQ(drain.events[index].change_kind, kinds[index]);
    }
    EXPECT_EQ(drain.dropped_total, 0U);
}

TEST(EventQueueTest, DrainReturnsAscendingSequenceAndClearsTheQueue) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto with = [](const char* text, const char* label) {
        return make_snapshot({make_region(900U, kFocusA, text, label)});
    };
    confirm_appearance(&coordinator, with("one", "btn"), &rig, 1U);
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with("two", "btn"), rig_frame(rig, 3U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with("two", "btn"), rig_frame(rig, 4U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with("two", "icon"), rig_frame(rig, 5U))));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with("two", "icon"), rig_frame(rig, 6U))));

    expect_confirmed_history(expect_drained(&coordinator));
    EXPECT_EQ(coordinator.query_temporal_state().pending_events, 0U);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
}

/// After the overflow: the queue holds only the newer event, the loss is
/// counted, and the resync report was consumed one plan_cycle ago.
void expect_overflow_outcome(ContinuousPerception* coordinator, const SemanticSnapshot& snapshot, const CycleRig& rig) {
    const TemporalSemanticState overflowed = coordinator->query_temporal_state();
    EXPECT_EQ(overflowed.pending_events, 1U);
    EXPECT_EQ(overflowed.dropped_events, 1U);
    static_cast<void>(expect_cycle_planned(coordinator->plan_cycle(snapshot, rig_frame(rig, 5U))));
    EXPECT_FALSE(expect_cycle_planned(coordinator->plan_cycle(snapshot, rig_frame(rig, 6U))).resync_required);
}

TEST(EventQueueTest, OverflowDropsTheOldestEventCountsItAndFlagsResyncOnce) {
    ContinuousPerceptionOptions options = options_with_source("cam");
    options.max_events = 1;
    ContinuousPerception coordinator = make_coordinator(options);
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto with_text = [](const char* text) { return make_snapshot({make_region(900U, kFocusA, text)}); };
    confirm_appearance(&coordinator, with_text("one"), &rig, 1U);
    ASSERT_EQ(coordinator.query_temporal_state().pending_events, 1U);
    EXPECT_EQ(coordinator.query_temporal_state().dropped_events, 0U);

    // The confirmed text change cannot fit: the oldest event is evicted.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 3U))));
    const WorkPlan confirming = expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 4U)));
    // The cycle that had to drop reports the structural break, exactly once.
    EXPECT_TRUE(confirming.resync_required);
    expect_overflow_outcome(&coordinator, with_text("two"), rig);

    // The loss is visible at the drain: a non-zero dropped_total forbids
    // claiming the complete sequence.
    const EventDrain drain = expect_drained(&coordinator);
    ASSERT_EQ(drain.events.size(), 1U);
    EXPECT_EQ(drain.events.front().event_sequence, 2U);  // the surviving newer event
    EXPECT_EQ(drain.dropped_total, 1U);
}

TEST(EventQueueTest, CancelledContextConvertsToKCancelled) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    const Result<EventDrain> cancelled = coordinator.drain_events(cancelled_context());
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.status().code(), mirador::ErrorCode::kCancelled);
}

// --- H. Query surface ------------------------------------------------------------------

TEST(FocusStateQueryTest, WithoutPolicyReturnsEmpty) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    EXPECT_TRUE(coordinator.query_focus_states().empty());
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    confirm_appearance(&coordinator, make_snapshot({make_region(900U, kFocusA, "t")}), &rig, 1U);
    // Still one resolution per engaged condition; without any, empty.
    static_cast<void>(coordinator.set_attention_policy(AttentionPolicy{}));
    EXPECT_TRUE(coordinator.query_focus_states().empty());
}

/// The query layers of the three-entity board: closed 900, unconfirmed 901
/// and open 902, in ascending stable-id order.
void expect_board_layers(const TemporalSemanticState& state) {
    ASSERT_EQ(state.entities.size(), 3U);
    EXPECT_EQ(state.entities[0].stable_id, 900U);
    EXPECT_EQ(state.entities[1].stable_id, 901U);
    EXPECT_EQ(state.entities[2].stable_id, 902U);
    EXPECT_TRUE(state.entities[0].instance_generation == 1U && state.entities[0].confirmed_frame == 2U);
    EXPECT_TRUE(state.entities[1].confirmed_frame == 0U && state.entities[2].instance_generation == 1U);
}

TEST(TemporalStateQueryTest, EntitiesAreAscendingAndIncludeUnconfirmedAndClosed) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(policy_of(
        {roi_condition(kFocusA, CoordinateSpaceId::kOriented), roi_condition(kFocusB, CoordinateSpaceId::kOriented)})));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    rig_paint(&rig, kFocusB, 60U);
    const auto board = [](bool with_900) {
        std::vector<VisualRegion> regions;
        if (with_900) {
            regions.push_back(make_region(900U, kFocusA, "a"));
        }
        regions.push_back(make_region(902U, kFocusB, "c"));
        return make_snapshot(std::move(regions));
    };

    const Frame first = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(board(true), first));
    ASSERT_EQ(plan.work.size(), 2U);
    expect_batch_accepted(coordinator.accept_work(tokens_of(plan), first));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(board(true), rig_frame(rig, 2U))));

    // An untracked newcomer outside every focus registers unconfirmed.
    std::vector<VisualRegion> with_901{make_region(901U, kFar, "b"), make_region(902U, kFocusB, "c")};
    static_cast<void>(
        expect_cycle_planned(coordinator.plan_cycle(make_snapshot(std::move(with_901)), rig_frame(rig, 3U))));
    // 900's covered area stays analyzed while it is missing, so its lifecycle
    // closes with a bare covered-missing disappearance.
    static_cast<void>(expect_cycle_planned(
        coordinator.plan_cycle(make_snapshot({make_region(902U, kFocusB, "c")}), rig_frame(rig, 4U))));

    const TemporalSemanticState state = coordinator.query_temporal_state();
    // Ascending by stable id; the closed lifecycle and the never-confirmed
    // sight stay queryable beside the open one.
    expect_board_layers(state);
    expect_entity_observation(state, 902U, ObservationState::kObserved, Freshness::kValidatedReuse, 4U);
    // Queries are pure: a second query returns an equal state.
    EXPECT_EQ(coordinator.query_temporal_state(), state);
}

TEST(FocusStateQueryTest, IdConditionReportsUnknownStaleAndResolved) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    confirm_appearance(&coordinator, make_snapshot({make_region(900U, kFocusA, "t")}), &rig, 1U);
    expect_engaged(coordinator.set_attention_policy(
        policy_of({id_condition(900U, 7U), id_condition(42U, 7U), id_condition(900U, 6U)})));

    const std::vector<FocusStateResolution> resolutions = coordinator.query_focus_states();
    ASSERT_EQ(resolutions.size(), 3U);
    EXPECT_TRUE(resolutions[0].resolved);
    EXPECT_EQ(resolutions[0].stable_ids, std::vector<uint64_t>{900U});
    EXPECT_FALSE(resolutions[1].resolved);
    EXPECT_EQ(resolutions[1].reason, FocusUnresolvedReason::kUnknownStableId);
    EXPECT_FALSE(resolutions[2].resolved);
    EXPECT_EQ(resolutions[2].reason, FocusUnresolvedReason::kStaleGeneration);
}

TEST(FocusStateQueryTest, TextAndLabelConditionsMatchTheConfirmedLayerOnly) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    // 900 confirms "hello"; 901 is only sighted unconfirmed with equal text.
    confirm_appearance(&coordinator, make_snapshot({make_region(900U, kFocusA, "hello", "btn")}), &rig, 1U);
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(
        make_snapshot({make_region(900U, kFocusA, "hello", "btn"), make_region(901U, kFar, "hello")}),
        rig_frame(rig, 3U))));

    AttentionCondition by_text;
    by_text.text_equals = "hello";
    AttentionCondition by_label;
    by_label.label_equals = "btn";
    AttentionCondition by_both;
    by_both.text_equals = "hello";
    by_both.label_equals = "btn";
    AttentionCondition by_miss;
    by_miss.text_equals = "nope";
    expect_engaged(coordinator.set_attention_policy(policy_of({by_text, by_label, by_both, by_miss})));
    const std::vector<FocusStateResolution> resolutions = coordinator.query_focus_states();
    ASSERT_EQ(resolutions.size(), 4U);
    for (size_t index = 0; index < 3U; ++index) {
        EXPECT_TRUE(resolutions[index].resolved) << "condition " << index;
        EXPECT_EQ(resolutions[index].stable_ids, std::vector<uint64_t>{900U})
            << "condition " << index << " must match the confirmed layer only";
    }
    EXPECT_FALSE(resolutions[3].resolved);
    EXPECT_EQ(resolutions[3].reason, FocusUnresolvedReason::kNotObserved);
}

TEST(FocusStateQueryTest, RoiConditionsMatchSameSpaceIntersectionOnly) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    confirm_appearance(&coordinator, make_snapshot({make_region(900U, kFocusA, "t")}), &rig, 1U);

    const AttentionCondition intersecting =
        roi_condition(RectF{10.0F, 10.0F, 30.0F, 30.0F}, CoordinateSpaceId::kOriented);
    AttentionCondition wrong_space = intersecting;
    wrong_space.roi_space = CoordinateSpaceId::kFrame;  // same numbers, other space
    const AttentionCondition disjoint = roi_condition(RectF{40.0F, 50.0F, 8.0F, 8.0F}, CoordinateSpaceId::kOriented);
    expect_engaged(coordinator.set_attention_policy(policy_of({intersecting, wrong_space, disjoint})));
    const std::vector<FocusStateResolution> resolutions = coordinator.query_focus_states();
    ASSERT_EQ(resolutions.size(), 3U);
    EXPECT_TRUE(resolutions[0].resolved);
    EXPECT_EQ(resolutions[0].stable_ids, std::vector<uint64_t>{900U});
    EXPECT_FALSE(resolutions[1].resolved);  // cross-space matching is not attempted
    EXPECT_EQ(resolutions[1].reason, FocusUnresolvedReason::kNotObserved);
    EXPECT_FALSE(resolutions[2].resolved);
    EXPECT_EQ(resolutions[2].reason, FocusUnresolvedReason::kNotObserved);
}

// --- I. Budgets and byte accounting -------------------------------------------------

TEST(TemporalBudgetTest, FreshCoordinatorOwnsZeroBytes) {
    const ContinuousPerception plain = make_coordinator();
    EXPECT_EQ(plain.coordination_owned_bytes(), 0);
    const ContinuousPerception configured = make_coordinator(options_with_source("a-longer-source-identity-string"));
    EXPECT_EQ(configured.coordination_owned_bytes(), 0);  // configuration is caller-owned
}

TEST(TemporalBudgetTest, EventQueueIsAccountedBySizeNotCapacity) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const auto with_text = [](const char* text) { return make_snapshot({make_region(900U, kFocusA, text)}); };
    confirm_appearance(&coordinator, with_text("one"), &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 3U))));

    // Queuing the one confirmation event grows the account by exactly that
    // event's owned bytes (size-counted queue), and draining returns them.
    const int64_t before = coordinator.coordination_owned_bytes();
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(with_text("two"), rig_frame(rig, 4U))));
    const int64_t queued = coordinator.coordination_owned_bytes();
    const EventDrain drain = expect_drained(&coordinator);
    ASSERT_EQ(drain.events.size(), 1U);
    EXPECT_EQ(queued - before, mirador::semantic_event_owned_bytes(drain.events.front()));
    EXPECT_EQ(coordinator.coordination_owned_bytes(), before);
}

TEST(TemporalBudgetTest, EntityCapEvictsClosedLifecyclesAndCountsRefusals) {
    ContinuousPerceptionOptions options = options_with_source("cam");
    options.max_entities = 1;
    ContinuousPerception coordinator = make_coordinator(options);
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot inhabitant = make_snapshot({make_region(900U, kFocusA, "a")});
    const SemanticSnapshot newcomer = make_snapshot({make_region(901U, kFar, "b")});
    confirm_appearance(&coordinator, inhabitant, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));

    // The live identity cannot be replaced: the newcomer is refused, visibly.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(newcomer, rig_frame(rig, 3U))));
    const TemporalSemanticState refused = coordinator.query_temporal_state();
    ASSERT_EQ(refused.entities.size(), 1U);
    EXPECT_EQ(refused.entities.front().stable_id, 900U);
    EXPECT_EQ(refused.untracked_entities, 1U);

    // 900 disappears (its area stays covered while it is missing), then the
    // newcomer's arrival evicts the closed lifecycle — also counted.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(make_snapshot({}), rig_frame(rig, 4U))));
    ASSERT_EQ(expect_drained(&coordinator).events.size(), 1U);  // the disappearance end
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(newcomer, rig_frame(rig, 5U))));
    const TemporalSemanticState evicted = coordinator.query_temporal_state();
    ASSERT_EQ(evicted.entities.size(), 1U);
    EXPECT_EQ(evicted.entities.front().stable_id, 901U);
    EXPECT_EQ(evicted.untracked_entities, 2U);  // one refusal plus one eviction
}

// --- J. Whole-batch region geometry rejection (DEC-021) -------------------------------

TEST(WholeBatchValidationTest, NonFiniteOrNegativeRegionRejectsTheWholeBatch) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "t")});
    confirm_appearance(&coordinator, snapshot, &rig, 1U);
    static_cast<void>(expect_drained(&coordinator));
    const int64_t bytes_before = coordinator.coordination_owned_bytes();
    const TemporalSemanticState state_before = coordinator.query_temporal_state();

    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    VisualRegion non_finite = make_region(901U, kInside);
    non_finite.bounds = RectF{kNaN, 0.0F, 10.0F, 10.0F};
    VisualRegion negative = make_region(902U, kInside);
    negative.bounds = RectF{0.0F, 0.0F, -5.0F, 10.0F};
    const SemanticSnapshot illegal = make_snapshot({make_region(903U, kFocusA, "ok"), non_finite, negative});

    const Result<WorkPlan> cycle = coordinator.plan_cycle(illegal, rig_frame(rig, 3U));
    ASSERT_FALSE(cycle.ok());
    EXPECT_EQ(cycle.status().code(), mirador::ErrorCode::kInvalidArgument);
    const Result<mirador::AttentionPlan> attention = coordinator.plan_attention(illegal, rig_frame(rig, 3U));
    ASSERT_FALSE(attention.ok());
    EXPECT_EQ(attention.status().code(), mirador::ErrorCode::kInvalidArgument);

    // The whole batch was rejected: nothing entered the machine.
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes_before);
    EXPECT_EQ(coordinator.query_temporal_state(), state_before);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
}

// Degenerate geometry is rejected at the plan boundary, never partially
// processed (DEC-021): a zero-size region would make every event about it
// violate the frozen structural rule (positive bounds are reserved for
// non-reset events), so the whole batch is refused before any state changes.
TEST(WholeBatchValidationTest, RejectsZeroSizeRegionsWholeBatch) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(focus_policy(kFocusA)));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot degenerate = make_snapshot({make_region(900U, RectF{2.0F, 2.0F, 0.0F, 0.0F}, "dot")});
    const int64_t bytes_before = coordinator.coordination_owned_bytes();

    const Result<WorkPlan> cycle = coordinator.plan_cycle(degenerate, rig_frame(rig, 1U));
    ASSERT_FALSE(cycle.ok());
    EXPECT_EQ(cycle.status().code(), mirador::ErrorCode::kInvalidArgument);
    const Result<mirador::AttentionPlan> attention = coordinator.plan_attention(degenerate, rig_frame(rig, 1U));
    ASSERT_FALSE(attention.ok());
    EXPECT_EQ(attention.status().code(), mirador::ErrorCode::kInvalidArgument);

    // The rejected batch left nothing behind: no entity, no event, no bytes.
    EXPECT_TRUE(coordinator.query_temporal_state().entities.empty());
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes_before);
    EXPECT_EQ(expect_drained(&coordinator).events.size(), 0U);
}

// --- K. Determinism ---------------------------------------------------------------------

TEST(DeterminismTest, TwinCoordinatorsProduceEqualTemporalHistories) {
    const auto with_text = [](const char* text) { return make_snapshot({make_region(900U, kFocusA, text)}); };
    CycleRig rig_a = make_rig();
    CycleRig rig_b = make_rig();
    rig_paint(&rig_a, kFocusA, 40U);
    rig_paint(&rig_b, kFocusA, 40U);

    ContinuousPerception first = make_coordinator(options_with_source("cam"));
    ContinuousPerception second = make_coordinator(options_with_source("cam"));
    for (ContinuousPerception* coordinator : {&first, &second}) {
        expect_engaged(coordinator->set_attention_policy(focus_policy(kFocusA)));
        confirm_appearance(coordinator, with_text("one"), &rig_a, 1U);
        static_cast<void>(expect_cycle_planned(coordinator->plan_cycle(with_text("two"), rig_frame(rig_a, 3U))));
        static_cast<void>(expect_cycle_planned(coordinator->plan_cycle(with_text("two"), rig_frame(rig_a, 4U))));
        static_cast<void>(coordinator->notify_source_reset());
    }

    const EventDrain drain_a = expect_drained(&first);
    const EventDrain drain_b = expect_drained(&second);
    EXPECT_EQ(drain_a.events.size(), drain_b.events.size());
    for (size_t index = 0; index < drain_a.events.size() && index < drain_b.events.size(); ++index) {
        EXPECT_EQ(drain_a.events[index], drain_b.events[index]);
    }
    EXPECT_EQ(drain_a.events.size(), 3U);  // begin, text change, source-reset end
    EXPECT_EQ(first.query_temporal_state(), second.query_temporal_state());
    EXPECT_EQ(first.coordination_owned_bytes(), second.coordination_owned_bytes());
}

}  // namespace
