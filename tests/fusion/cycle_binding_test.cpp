// M9-04 cycle-binding surface tests (DEC-026 sections 3/5, Experimental): the
// tokenized plan/accept loop over the incremental coverage ledger. Covering
// the plan side — input validation with the pixel-residency requirements, the
// coverage re-verification order (identity components, kGlobal ledger reset
// with its one-shot resync report, partial change ROI intersection, exact FNV
// content re-digests catching the fingerprint-hidden single-region drift),
// reuse reporting with the queryable validating frame, focus suppression into
// reuse, bootstrap work that is never suppressed, and the issued WorkToken
// echo of every binding field; the accept side — pass-0 structural aborts, the
// frozen staleness order over foreign sources / stale epochs / policy
// revisions / generations / frame sequences, not-in-flight rejection,
// duplicate submission, exact content mismatch, whole-batch atomicity with
// per-item reasons, cumulative rejection counters, same-region ledger refresh
// and the empty-batch trivial commit; the latest-pending replacement
// accounting, the source reset ending epoch/cycle state, the tightened byte
// budget gates on plan and accept with their no-side-effect guarantees, the
// coordination byte account (zero point, growth, twin determinism, static
// determinism), cancellation/deadline conversion with the poll-point matrix,
// the kOriented/kFrame digest round trips across all four rotations and a
// padded stride, the AC9-07 five-fps latest-pending backlog simulation over a
// virtual 60 fps stream, and the M9-02/M9-03 surface isolation.
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
#include <tuple>
#include <utility>
#include <vector>

namespace {

using mirador::AttentionCondition;
using mirador::AttentionPlan;
using mirador::AttentionPolicy;
using mirador::build_relation_view;
using mirador::ChangeClassification;
using mirador::ContinuousPerception;
using mirador::ContinuousPerceptionOptions;
using mirador::CoordinateSpaceId;
using mirador::CoverageState;
using mirador::ErrorCode;
using mirador::EventDrain;
using mirador::ExecutionContext;
using mirador::Frame;
using mirador::ImportancePolicy;
using mirador::PixelFormat;
using mirador::PlannedWork;
using mirador::PlannedWorkBinding;
using mirador::PlannedWorkKind;
using mirador::PointF;
using mirador::RectF;
using mirador::RectI;
using mirador::Result;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::SourceResetReport;
using mirador::VisualRegion;
using mirador::WorkAcceptReport;
using mirador::WorkInvalidation;
using mirador::WorkInvalidationReason;
using mirador::WorkPlan;
using mirador::WorkRejectionReason;
using mirador::WorkReuse;
using mirador::WorkToken;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

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

AttentionCondition text_condition(std::string text) {
    AttentionCondition condition;
    condition.text_equals = std::move(text);
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

ContinuousPerception make_coordinator(const ContinuousPerceptionOptions& options = ContinuousPerceptionOptions{}) {
    auto created = ContinuousPerception::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_coordinator: " << created.status().message();
        return ContinuousPerception{};
    }
    return created.take_value();
}

ContinuousPerceptionOptions options_with_source(std::string source_id) {
    ContinuousPerceptionOptions options;
    options.source_id = std::move(source_id);
    return options;
}

void expect_engaged(const Result<void>& engaged) {
    EXPECT_TRUE(engaged.ok()) << engaged.status().message();
}

void expect_cycle_rejected(const Result<WorkPlan>& result, ErrorCode code) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), code);
    }
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

/// A structurally valid but whole-batch-rejected report: the call itself
/// succeeded, nothing committed, and every item carries its reason.
void expect_batch_rejected(const Result<WorkAcceptReport>& result,
                           const std::vector<std::optional<WorkRejectionReason>>& reasons) {
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.status().message());
    if (!result.ok()) {
        return;
    }
    EXPECT_FALSE(result.value().committed);
    EXPECT_EQ(result.value().item_rejections, reasons);
    EXPECT_EQ(result.value().adopted_count, 0U);
}

/// A pass-0 / frame-level abort: the whole call failed with `code`.
void expect_accept_aborted(const Result<WorkAcceptReport>& result, ErrorCode code) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), code);
    }
}

void expect_rejection_count(const ContinuousPerception& coordinator, WorkRejectionReason reason, uint64_t expected) {
    EXPECT_EQ(coordinator.rejection_count(reason), expected);
}

/// Cumulative rejections over every reason (for no-rejection negatives).
[[nodiscard]] uint64_t total_rejections(const ContinuousPerception& coordinator) {
    uint64_t total = 0;
    for (const WorkRejectionReason reason :
         {WorkRejectionReason::kForeignSource, WorkRejectionReason::kStaleSourceEpoch,
          WorkRejectionReason::kStalePolicyRevision, WorkRejectionReason::kStaleGeneration,
          WorkRejectionReason::kStaleFrameSequence, WorkRejectionReason::kDuplicateSubmission,
          WorkRejectionReason::kContentMismatch}) {
        total += coordinator.rejection_count(reason);
    }
    return total;
}

std::vector<WorkToken> tokens_of(const WorkPlan& plan) {
    std::vector<WorkToken> tokens;
    tokens.reserve(plan.work.size());
    for (const PlannedWorkBinding& binding : plan.work) {
        tokens.push_back(binding.token);
    }
    return tokens;
}

WorkToken token_with_source(WorkToken token, std::string source_id) {
    token.source_id = std::move(source_id);
    return token;
}

WorkToken token_with_epoch(WorkToken token, uint64_t epoch) {
    token.source_epoch = epoch;
    return token;
}

WorkToken token_with_frame(WorkToken token, uint64_t frame_sequence) {
    token.frame_sequence = frame_sequence;
    return token;
}

/// The identity half of a token's frozen binding fields.
void expect_token_identity(const WorkToken& token, const std::string& source_id, uint64_t epoch,
                           uint64_t frame_sequence, uint64_t generation, uint64_t policy_revision,
                           uint64_t request_digest) {
    EXPECT_EQ(token.source_id, source_id);
    EXPECT_EQ(token.source_epoch, epoch);
    EXPECT_EQ(token.frame_sequence, frame_sequence);
    EXPECT_EQ(token.generation, generation);
    EXPECT_EQ(token.policy_revision, policy_revision);
    EXPECT_EQ(token.request_digest, request_digest);
}

/// Every frozen binding field of an issued token, checked field by field.
void expect_token_binding(const WorkToken& token, const std::string& source_id, uint64_t epoch, uint64_t frame_sequence,
                          uint64_t generation, uint64_t policy_revision, uint64_t request_digest, const RectF& roi,
                          CoordinateSpaceId space) {
    expect_token_identity(token, source_id, epoch, frame_sequence, generation, policy_revision, request_digest);
    EXPECT_EQ(token.roi, roi);
    EXPECT_EQ(token.roi_space, space);
}

void expect_reuse_entry(const WorkReuse& entry, const RectF& roi, CoordinateSpaceId space, uint64_t generation,
                        uint64_t validated_frame) {
    EXPECT_EQ(entry.roi, roi);
    EXPECT_EQ(entry.space, space);
    EXPECT_EQ(entry.generation, generation);
    EXPECT_EQ(entry.validated_frame, validated_frame);
}

void expect_invalidation(const WorkInvalidation& record, const RectF& roi, CoordinateSpaceId space,
                         WorkInvalidationReason reason, bool global) {
    EXPECT_EQ(record.roi, roi);
    EXPECT_EQ(record.space, space);
    EXPECT_EQ(record.reason, reason);
    EXPECT_EQ(record.global, global);
}

// --- Cycle frame rig ----------------------------------------------------------
//
// The cycle entry points read primary-plane pixels (exact ROI digests), so the
// fixtures own their buffers and paint through them. The 48x64 RGBA default
// keeps every digest cheap; the rotation and stride fields drive the
// coordinate matrix.

struct CycleRig {
    std::vector<std::uint8_t> pixels;
    int32_t width = 48;
    int32_t height = 64;
    int64_t stride = 192;  // width * 4 (RGBA8), exact fit by default
    Rotation rotation = Rotation::k0;
};

CycleRig make_rig(Rotation rotation = Rotation::k0, int64_t stride = 192) {
    CycleRig rig;
    rig.rotation = rotation;
    rig.stride = stride;
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
    frame.image.rotation = rig.rotation;
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

void rig_paint_pixel(CycleRig* rig, int32_t x, int32_t y, uint8_t value) {
    rig->pixels[static_cast<size_t>(y) * static_cast<size_t>(rig->stride) + static_cast<size_t>(x) * 4U] = value;
}

/// Overwrites the inter-row padding a padded stride carries: digest reads must
/// stay inside the row width or the static-cycle re-verification breaks.
void rig_paint_padding(CycleRig* rig, uint8_t value) {
    const auto row_bytes = static_cast<int64_t>(rig->width) * 4;
    for (int32_t y = 0; y < rig->height; ++y) {
        for (int64_t byte = row_bytes; byte < rig->stride; ++byte) {
            rig->pixels[static_cast<size_t>(y) * static_cast<size_t>(rig->stride) + static_cast<size_t>(byte)] = value;
        }
    }
}

/// A structurally valid NV12 view (populated secondary plane): passes
/// `validate` but must be rejected by the cycle entry points as multi-plane.
Frame make_nv12_frame(std::vector<std::uint8_t>* luma, std::vector<std::uint8_t>* chroma) {
    luma->assign(static_cast<size_t>(48) * static_cast<size_t>(64), 128U);
    chroma->assign(static_cast<size_t>(48) * static_cast<size_t>(32), 128U);
    Frame frame;
    frame.image.data = reinterpret_cast<const std::byte*>(luma->data());
    frame.image.width = 48;
    frame.image.height = 64;
    frame.image.row_stride_bytes = 48;
    frame.image.format = PixelFormat::kNv12;
    frame.image.secondary_plane.data = reinterpret_cast<const std::byte*>(chroma->data());
    frame.image.secondary_plane.row_stride_bytes = 48;
    frame.sequence = 1;
    return frame;
}

/// A sized RGBA view without resident pixels: the cycle contract requires
/// primary-plane pixels for the content binding.
Frame make_null_pixel_frame() {
    Frame frame;
    frame.image.width = 48;
    frame.image.height = 64;
    frame.image.row_stride_bytes = 192;
    frame.image.format = PixelFormat::kRgba8;
    frame.sequence = 1;
    return frame;
}

// --- Cycle fixtures ------------------------------------------------------------

constexpr RectF kFocusA{0.0F, 0.0F, 24.0F, 24.0F};
constexpr RectF kFocusB{24.0F, 28.0F, 16.0F, 16.0F};

void engage_focus_policy(ContinuousPerception* coordinator, const RectF& roi) {
    expect_engaged(coordinator->set_attention_policy(policy_of({roi_condition(roi, CoordinateSpaceId::kOriented)})));
}

/// Canonical one-focus fill: plan the first frame and accept its single work
/// item (the ledger then holds one coverage entry). Returns the accepted token.
WorkToken fill_and_accept(ContinuousPerception* coordinator, const SemanticSnapshot& snapshot, CycleRig* rig,
                          uint64_t sequence) {
    const Frame frame = rig_frame(*rig, sequence);
    const WorkPlan plan = expect_cycle_planned(coordinator->plan_cycle(snapshot, frame));
    EXPECT_EQ(plan.work.size(), 1U);
    if (plan.work.size() != 1U) {
        return WorkToken{};
    }
    expect_batch_accepted(coordinator->accept_work(tokens_of(plan), frame));
    return plan.work.front().token;
}

/// Plans one cycle without accepting: its single work item stays in flight.
WorkToken plan_only(ContinuousPerception* coordinator, const SemanticSnapshot& snapshot, const Frame& frame) {
    const WorkPlan plan = expect_cycle_planned(coordinator->plan_cycle(snapshot, frame));
    EXPECT_EQ(plan.work.size(), 1U);
    if (plan.work.size() != 1U) {
        return WorkToken{};
    }
    return plan.work.front().token;
}

/// Measured coordination accounts of the canonical one-focus fixture on a
/// default-budget coordinator: after engaging the policy, and after planning
/// (but not accepting) its single work item. Tight-budget coordinators size
/// themselves from these numbers instead of hard-coded struct sizes.
struct BudgetProbe {
    int64_t engaged = 0;
    int64_t planned = 0;
};

BudgetProbe probe_fixture_bytes() {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    BudgetProbe probe;
    probe.engaged = coordinator.coordination_owned_bytes();
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U)));
    EXPECT_EQ(plan.work.size(), 1U);
    probe.planned = coordinator.coordination_owned_bytes();
    return probe;
}

// --- Coordinate oracles ---------------------------------------------------------
//
// The test's private inverse of make_rotation (x_o = h - y_r, y_o = x_r for
// k90; mirrored for k270): the raw-pixel footprint an oriented ROI digests.

RectF oriented_footprint(const RectF& roi, Rotation rotation, int32_t raw_width, int32_t raw_height) {
    const auto raw_w = static_cast<float>(raw_width);
    const auto raw_h = static_cast<float>(raw_height);
    switch (rotation) {
        case Rotation::k90:
            return RectF{roi.y, raw_h - roi.x - roi.width, roi.height, roi.width};
        case Rotation::k180:
            return RectF{raw_w - roi.x - roi.width, raw_h - roi.y - roi.height, roi.width, roi.height};
        case Rotation::k270:
            return RectF{raw_w - roi.y - roi.height, roi.x, roi.height, roi.width};
        case Rotation::k0:
            break;
    }
    return roi;
}

PointF presented_point(int32_t raw_x, int32_t raw_y, Rotation rotation, int32_t raw_width, int32_t raw_height) {
    const auto x = static_cast<float>(raw_x);
    const auto y = static_cast<float>(raw_y);
    switch (rotation) {
        case Rotation::k90:
            return PointF{static_cast<float>(raw_height) - y, x};
        case Rotation::k180:
            return PointF{static_cast<float>(raw_width) - x, static_cast<float>(raw_height) - y};
        case Rotation::k270:
            return PointF{y, static_cast<float>(raw_width) - x};
        case Rotation::k0:
            break;
    }
    return PointF{x, y};
}

// --- Context helpers -------------------------------------------------------------

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

// --- A. Coverage ledger and reuse (AC9-03) ----------------------------------------

TEST(CoverageLedgerTest, InitialFillThenStaticFrameIssuesNoWork) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});

    const WorkToken accepted = fill_and_accept(&coordinator, snapshot, &rig, 1U);
    EXPECT_EQ(accepted.frame_sequence, 1U);

    // Static frame: the exact re-digests verify the entry, the focus candidate
    // is suppressed into reuse, and no work is issued (AC9-03).
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.work.empty());
    ASSERT_EQ(plan.reuse.size(), 2U);  // region reuse + suppressed focus candidate
    for (const WorkReuse& entry : plan.reuse) {
        expect_reuse_entry(entry, kFocusA, CoordinateSpaceId::kOriented, 7U, 1U);
    }
    EXPECT_TRUE(plan.invalidations.empty());
    EXPECT_EQ(plan.replaced_inflight, 0U);
    EXPECT_FALSE(plan.resync_required);
    EXPECT_EQ(plan.coverage, CoverageState::kComplete);
    EXPECT_EQ(total_rejections(coordinator), 0U);
}

TEST(CoverageLedgerTest, SingleRegionPixelDriftInvalidatesAndReissues) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig fill_rig = make_rig();
    rig_paint(&fill_rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &fill_rig, 1U);

    // The region pixel drifts while the change report stays kNone: the exact
    // re-digest catches what the fingerprint hid (DEC-026 section 3).
    CycleRig drift_rig = make_rig();
    rig_paint(&drift_rig, kFocusA, 41U);
    const WorkPlan drift = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(drift_rig, 3U)));
    ASSERT_EQ(drift.invalidations.size(), 1U);
    expect_invalidation(drift.invalidations.front(), kFocusA, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kContentMismatch, false);
    EXPECT_TRUE(drift.reuse.empty());
    ASSERT_EQ(drift.work.size(), 1U);
    EXPECT_EQ(drift.work.front().token.frame_sequence, 3U);
    EXPECT_EQ(drift.coverage, CoverageState::kIncomplete);

    expect_batch_accepted(coordinator.accept_work(tokens_of(drift), rig_frame(drift_rig, 3U)));

    // The refreshed entry validates against the new frame (AC9-03).
    const WorkPlan revived = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(drift_rig, 4U)));
    EXPECT_TRUE(revived.work.empty());
    ASSERT_EQ(revived.reuse.size(), 2U);
    expect_reuse_entry(revived.reuse.front(), kFocusA, CoordinateSpaceId::kOriented, 7U, 3U);
    EXPECT_EQ(revived.coverage, CoverageState::kComplete);
}

TEST(CoverageLedgerTest, PartialChangeRoiIntersectionInvalidatesOnlyTheHitEntry) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(policy_of(
        {roi_condition(kFocusA, CoordinateSpaceId::kOriented), roi_condition(kFocusB, CoordinateSpaceId::kOriented)})));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    rig_paint(&rig, kFocusB, 60U);
    SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA), make_region(901U, kFocusB)});
    const Frame fill_frame = rig_frame(rig, 1U);
    const WorkPlan fill = expect_cycle_planned(coordinator.plan_cycle(snapshot, fill_frame));
    ASSERT_EQ(fill.work.size(), 2U);
    expect_batch_accepted(coordinator.accept_work(tokens_of(fill), fill_frame));

    // Pixels identical; only the change report claims a change inside A.
    apply_change(&snapshot, ChangeClassification::kPartial, {RectI{0, 0, 4, 4}});
    const WorkPlan partial = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    ASSERT_EQ(partial.invalidations.size(), 1U);
    expect_invalidation(partial.invalidations.front(), kFocusA, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kRegionChanged, false);
    // B survives: its region reuse stays, its candidate is suppressed, and only
    // A is re-issued — while the attention view keeps both candidates.
    ASSERT_EQ(partial.work.size(), 1U);
    EXPECT_EQ(partial.work.front().work.roi, kFocusA);
    EXPECT_EQ(partial.attention.work.size(), 2U);
    ASSERT_EQ(partial.reuse.size(), 2U);
    expect_reuse_entry(partial.reuse.front(), kFocusB, CoordinateSpaceId::kOriented, 7U, 1U);
    EXPECT_EQ(partial.coverage, CoverageState::kIncomplete);

    expect_batch_accepted(coordinator.accept_work(tokens_of(partial), rig_frame(rig, 2U)));
    // The next processed frame carries a fresh (empty) change report — the
    // persisted one above would otherwise re-invalidate A every cycle.
    SemanticSnapshot settled = snapshot;
    apply_change(&settled, ChangeClassification::kNone, {});
    const WorkPlan refilled = expect_cycle_planned(coordinator.plan_cycle(settled, rig_frame(rig, 3U)));
    EXPECT_TRUE(refilled.work.empty());
    EXPECT_EQ(refilled.coverage, CoverageState::kComplete);
    // A now validates against frame 2, B still against frame 1 (both the region
    // reuse pass and the suppressed-candidate pass keep the order).
    ASSERT_EQ(refilled.reuse.size(), 4U);
    expect_reuse_entry(refilled.reuse[0], kFocusA, CoordinateSpaceId::kOriented, 7U, 2U);
    expect_reuse_entry(refilled.reuse[1], kFocusB, CoordinateSpaceId::kOriented, 7U, 1U);
    expect_reuse_entry(refilled.reuse[2], kFocusA, CoordinateSpaceId::kOriented, 7U, 2U);
    expect_reuse_entry(refilled.reuse[3], kFocusB, CoordinateSpaceId::kOriented, 7U, 1U);
}

TEST(CoverageLedgerTest, TouchingChangeRoiWithZeroIntersectionKeepsTheEntry) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    // The change ROI starts exactly at A's right edge: intersection area 0.
    apply_change(&snapshot, ChangeClassification::kPartial, {RectI{24, 0, 4, 4}});
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.invalidations.empty());
    EXPECT_TRUE(plan.work.empty());
    EXPECT_EQ(plan.coverage, CoverageState::kComplete);
}

TEST(CoverageLedgerTest, DisjointChangeRoiKeepsTheLedgerIntact) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    apply_change(&snapshot, ChangeClassification::kPartial, {RectI{40, 50, 4, 4}});
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.invalidations.empty());
    EXPECT_TRUE(plan.work.empty());
    EXPECT_EQ(plan.coverage, CoverageState::kComplete);
    EXPECT_EQ(plan.attention.reconsiderations.size(), 1U);  // peripheral, M9-03 view
}

TEST(CoverageLedgerTest, GlobalChangeClearsTheLedgerWithOneRecordAndOneResync) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    apply_change(&snapshot, ChangeClassification::kGlobal, {RectI{0, 0, 2, 2}});
    const WorkPlan global = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    ASSERT_EQ(global.invalidations.size(), 1U);
    expect_invalidation(global.invalidations.front(), RectF{0.0F, 0.0F, 48.0F, 64.0F}, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kLayoutChange, true);
    EXPECT_TRUE(global.resync_required);
    EXPECT_TRUE(global.reuse.empty());
    ASSERT_EQ(global.work.size(), 1U);  // the focus is re-issued after the reset
    EXPECT_EQ(global.coverage, CoverageState::kIncomplete);

    // The resync is reported exactly once; an already-empty ledger records
    // nothing further.
    const WorkPlan again = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U)));
    EXPECT_FALSE(again.resync_required);
    EXPECT_TRUE(again.invalidations.empty());
    ASSERT_EQ(again.work.size(), 1U);
}

TEST(CoverageLedgerTest, GlobalChangeWithEmptyLedgerRecordsNothing) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    apply_change(&snapshot, ChangeClassification::kGlobal, {RectI{0, 0, 2, 2}});

    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U)));
    EXPECT_TRUE(plan.invalidations.empty());
    EXPECT_FALSE(plan.resync_required);
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
}

TEST(CoverageLedgerTest, GenerationAdvanceInvalidatesEveryEntryAsIdentityChanged) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(policy_of(
        {roi_condition(kFocusA, CoordinateSpaceId::kOriented), roi_condition(kFocusB, CoordinateSpaceId::kOriented)})));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    rig_paint(&rig, kFocusB, 60U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA), make_region(901U, kFocusB)});
    const Frame fill_frame = rig_frame(rig, 1U);
    const WorkPlan fill = expect_cycle_planned(coordinator.plan_cycle(snapshot, fill_frame));
    expect_batch_accepted(coordinator.accept_work(tokens_of(fill), fill_frame));

    const SemanticSnapshot advanced = make_snapshot(snapshot.regions, 8);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(advanced, rig_frame(rig, 2U)));
    ASSERT_EQ(plan.invalidations.size(), 2U);
    expect_invalidation(plan.invalidations[0], kFocusA, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kIdentityChanged, false);
    expect_invalidation(plan.invalidations[1], kFocusB, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kIdentityChanged, false);
    ASSERT_EQ(plan.work.size(), 2U);
    EXPECT_EQ(plan.work.front().token.generation, 8U);
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
}

TEST(CoverageLedgerTest, RequestDigestChangeInvalidatesAsIdentityChanged) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame fill_frame = rig_frame(rig, 1U);
    const WorkPlan fill = expect_cycle_planned(coordinator.plan_cycle(snapshot, fill_frame, {}, 0U));
    expect_batch_accepted(coordinator.accept_work(tokens_of(fill), fill_frame));

    const WorkPlan switched = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U), {}, 99U));
    ASSERT_EQ(switched.invalidations.size(), 1U);
    expect_invalidation(switched.invalidations.front(), kFocusA, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kIdentityChanged, false);
    ASSERT_EQ(switched.work.size(), 1U);
    EXPECT_EQ(switched.work.front().token.request_digest, 99U);
    EXPECT_EQ(switched.coverage, CoverageState::kIncomplete);
}

TEST(CoverageLedgerTest, PolicyRevisionChangeInvalidatesAsIdentityChanged) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    // Same conditions, different cap: digest change -> revision 2.
    AttentionPolicy re_capped;
    re_capped.conditions.push_back(roi_condition(kFocusA, CoordinateSpaceId::kOriented));
    re_capped.max_condition_bytes = 4096;
    expect_engaged(coordinator.set_attention_policy(re_capped));
    ASSERT_EQ(coordinator.policy_revision(), 2U);

    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    ASSERT_EQ(plan.invalidations.size(), 1U);
    expect_invalidation(plan.invalidations.front(), kFocusA, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kIdentityChanged, false);
    ASSERT_EQ(plan.work.size(), 1U);
    EXPECT_EQ(plan.work.front().token.policy_revision, 2U);
}

TEST(CoverageLedgerTest, SnapshotSpaceChangeInvalidatesAsIdentityChanged) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{4.0F, 4.0F, 20.0F, 20.0F}, CoordinateSpaceId::kFrame)})));
    CycleRig rig = make_rig();
    rig_paint(&rig, RectF{4.0F, 4.0F, 20.0F, 20.0F}, 40U);
    const SemanticSnapshot frame_space =
        make_snapshot({make_region(900U, RectF{4.0F, 4.0F, 20.0F, 20.0F})}, 7, CoordinateSpaceId::kFrame);
    fill_and_accept(&coordinator, frame_space, &rig, 1U);

    // Rotation k0 keeps the coordinates identical, so only the space component
    // of the entry identity differs.
    const SemanticSnapshot oriented =
        make_snapshot({make_region(900U, RectF{4.0F, 4.0F, 20.0F, 20.0F})}, 7, CoordinateSpaceId::kOriented);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(oriented, rig_frame(rig, 2U)));
    ASSERT_EQ(plan.invalidations.size(), 1U);
    expect_invalidation(plan.invalidations.front(), RectF{4.0F, 4.0F, 20.0F, 20.0F}, CoordinateSpaceId::kFrame,
                        WorkInvalidationReason::kIdentityChanged, false);
}

TEST(CoverageLedgerTest, UncoveredSnapshotRegionKeepsCoverageIncomplete) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot filled = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, filled, &rig, 1U);

    // A new region appears outside every coverage entry.
    const SemanticSnapshot with_extra =
        make_snapshot({make_region(900U, kFocusA), make_region(902U, RectF{30.0F, 40.0F, 8.0F, 8.0F})});
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(with_extra, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.work.empty());  // no condition names the extra region
    ASSERT_EQ(plan.reuse.size(), 2U);
    for (const WorkReuse& entry : plan.reuse) {
        EXPECT_EQ(entry.roi, kFocusA);
    }
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
}

TEST(CoverageLedgerTest, EmptyRegionSetOverAFilledLedgerStaysComplete) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot filled = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, filled, &rig, 1U);

    // Every entity left the frame: the frozen formula (kComplete iff every
    // snapshot region lies inside a valid entry) is vacuously satisfied.
    const SemanticSnapshot empty = make_snapshot({}, 7);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(empty, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.work.empty());
    ASSERT_EQ(plan.reuse.size(), 1U);  // the suppressed focus candidate only
    EXPECT_EQ(plan.coverage, CoverageState::kComplete);
}

// --- B. Token binding -------------------------------------------------------------

TEST(TokenBindingTest, IssuedTokenEchoesEveryBindingField) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});

    const WorkPlan plan =
        expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 5U), importance_at(1.5F), 77U));
    ASSERT_EQ(plan.work.size(), 1U);
    const PlannedWorkBinding& binding = plan.work.front();
    EXPECT_EQ(binding.work.kind, PlannedWorkKind::kFocus);
    EXPECT_EQ(binding.work.priority, 1.5F);
    expect_token_binding(binding.token, "cam", coordinator.source_epoch(), 5U, 7U, coordinator.policy_revision(), 77U,
                         kFocusA, CoordinateSpaceId::kOriented);
    // The issued work mirrors the M9-03 attention view, in order.
    EXPECT_EQ(plan.attention.policy_revision, coordinator.policy_revision());
    ASSERT_EQ(plan.attention.work.size(), 1U);
    EXPECT_EQ(binding.work, plan.attention.work.front());
}

TEST(TokenBindingTest, ContentDigestBindsTheExactPlannedPixels) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig planned_rig = make_rig();
    rig_paint(&planned_rig, kFocusA, 40U);
    CycleRig tampered_rig = make_rig();
    rig_paint(&tampered_rig, kFocusA, 41U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});

    const WorkToken token = plan_only(&coordinator, snapshot, rig_frame(planned_rig, 1U));
    const std::vector<WorkToken> batch{token};
    // Executing against different pixels than planned can never adopt.
    expect_batch_rejected(coordinator.accept_work(batch, rig_frame(tampered_rig, 1U)),
                          {WorkRejectionReason::kContentMismatch});
    expect_rejection_count(coordinator, WorkRejectionReason::kContentMismatch, 1U);
    // The exact planned frame adopts (behavioral pin of the digest value).
    expect_batch_accepted(coordinator.accept_work(batch, rig_frame(planned_rig, 1U)));
}

TEST(TokenBindingTest, CoveredFocusCandidateIsSuppressedIntoReuse) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.work.empty());
    // The attention view still carries the candidate; the cycle issued none.
    ASSERT_EQ(plan.attention.work.size(), 1U);
    ASSERT_EQ(plan.reuse.size(), 2U);
    for (const WorkReuse& entry : plan.reuse) {
        expect_reuse_entry(entry, kFocusA, CoordinateSpaceId::kOriented, 7U, 1U);
    }
}

TEST(TokenBindingTest, BootstrapWorkIsNeverSuppressed) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(policy_of({text_condition("Ghost")})));
    const CycleRig rig = make_rig();
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "OK")});

    const Frame fill_frame = rig_frame(rig, 1U);
    const WorkPlan fill = expect_cycle_planned(coordinator.plan_cycle(snapshot, fill_frame, importance_at(2.0F, true)));
    ASSERT_EQ(fill.work.size(), 1U);
    EXPECT_EQ(fill.work.front().work.kind, PlannedWorkKind::kBootstrap);
    expect_token_binding(fill.work.front().token, "cam", 1U, 1U, 7U, 1U, 0U, RectF{0.0F, 0.0F, 48.0F, 64.0F},
                         CoordinateSpaceId::kOriented);
    expect_batch_accepted(coordinator.accept_work(tokens_of(fill), fill_frame));

    // The accepted full-frame entry covers the snapshot region but never
    // suppresses a bootstrap candidate.
    const WorkPlan again =
        expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U), importance_at(2.0F, true)));
    ASSERT_EQ(again.work.size(), 1U);
    EXPECT_EQ(again.work.front().work.kind, PlannedWorkKind::kBootstrap);
    EXPECT_EQ(again.work.front().token.frame_sequence, 2U);
    ASSERT_EQ(again.reuse.size(), 1U);
    expect_reuse_entry(again.reuse.front(), kFocusA, CoordinateSpaceId::kOriented, 7U, 1U);
    EXPECT_EQ(again.coverage, CoverageState::kComplete);
}

TEST(TokenBindingTest, TwinCoordinatorsIssueEqualPlansAndTokens) {
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    ContinuousPerception first = make_coordinator(options_with_source("cam"));
    ContinuousPerception second = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&first, kFocusA);
    engage_focus_policy(&second, kFocusA);
    CycleRig rig_a = make_rig();
    CycleRig rig_b = make_rig();
    rig_paint(&rig_a, kFocusA, 40U);
    rig_paint(&rig_b, kFocusA, 40U);

    const WorkPlan plan_a = expect_cycle_planned(first.plan_cycle(snapshot, rig_frame(rig_a, 1U)));
    const WorkPlan plan_b = expect_cycle_planned(second.plan_cycle(snapshot, rig_frame(rig_b, 1U)));
    EXPECT_EQ(plan_a, plan_b);  // includes the content digests inside the tokens
    expect_batch_accepted(first.accept_work(tokens_of(plan_a), rig_frame(rig_a, 1U)));
    expect_batch_accepted(second.accept_work(tokens_of(plan_b), rig_frame(rig_b, 1U)));
    EXPECT_EQ(first.coordination_owned_bytes(), second.coordination_owned_bytes());
}

// --- C. Accept matrix (AC9-06) ------------------------------------------------------

TEST(WorkAcceptMatrixTest, ForeignSourceIsRejectedCountedAndWinsTheFrozenOrder) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const WorkToken token = plan_only(&coordinator, snapshot, rig_frame(rig, 1U));

    const std::vector<WorkToken> foreign_batch{token_with_source(token, "other")};
    expect_batch_rejected(coordinator.accept_work(foreign_batch, rig_frame(rig, 1U)),
                          {WorkRejectionReason::kForeignSource});
    expect_rejection_count(coordinator, WorkRejectionReason::kForeignSource, 1U);

    // A foreign source wins the frozen order over every other staleness.
    const std::vector<WorkToken> foreign_all{token_with_epoch(token_with_source(token, "other"), 9U)};
    expect_batch_rejected(coordinator.accept_work(foreign_all, rig_frame(rig, 1U)),
                          {WorkRejectionReason::kForeignSource});
    expect_rejection_count(coordinator, WorkRejectionReason::kForeignSource, 2U);
}

TEST(WorkAcceptMatrixTest, StaleSourceEpochIsRejectedAndCounted) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const WorkToken token = plan_only(&coordinator, snapshot, rig_frame(rig, 1U));

    static_cast<void>(coordinator.notify_source_reset());
    const std::vector<WorkToken> old_epoch{token};
    expect_batch_rejected(coordinator.accept_work(old_epoch, rig_frame(rig, 1U)),
                          {WorkRejectionReason::kStaleSourceEpoch});
    expect_rejection_count(coordinator, WorkRejectionReason::kStaleSourceEpoch, 1U);
}

TEST(WorkAcceptMatrixTest, StalePolicyRevisionIsRejectedAndCounted) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const WorkToken token = plan_only(&coordinator, snapshot, rig_frame(rig, 1U));

    expect_engaged(coordinator.set_attention_policy(policy_of({text_condition("Other")})));
    ASSERT_EQ(coordinator.policy_revision(), 2U);
    const std::vector<WorkToken> old_revision{token};
    expect_batch_rejected(coordinator.accept_work(old_revision, rig_frame(rig, 1U)),
                          {WorkRejectionReason::kStalePolicyRevision});
    expect_rejection_count(coordinator, WorkRejectionReason::kStalePolicyRevision, 1U);
}

TEST(WorkAcceptMatrixTest, StaleGenerationIsRejectedAndCounted) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot old = make_snapshot({make_region(900U, kFocusA)}, 7);
    const WorkToken token = plan_only(&coordinator, old, rig_frame(rig, 1U));

    // A newer plan rebinds the generation; the old token cannot follow.
    const SemanticSnapshot advanced = make_snapshot({make_region(900U, kFocusA)}, 8);
    static_cast<void>(plan_only(&coordinator, advanced, rig_frame(rig, 1U)));
    const std::vector<WorkToken> old_generation{token};
    expect_batch_rejected(coordinator.accept_work(old_generation, rig_frame(rig, 1U)),
                          {WorkRejectionReason::kStaleGeneration});
    expect_rejection_count(coordinator, WorkRejectionReason::kStaleGeneration, 1U);
}

TEST(WorkAcceptMatrixTest, StaleFrameSequenceIsRejectedAndCounted) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const WorkToken token = plan_only(&coordinator, snapshot, rig_frame(rig, 1U));
    static_cast<void>(plan_only(&coordinator, snapshot, rig_frame(rig, 2U)));

    const std::vector<WorkToken> old_frame{token};
    expect_batch_rejected(coordinator.accept_work(old_frame, rig_frame(rig, 2U)),
                          {WorkRejectionReason::kStaleFrameSequence});
    expect_rejection_count(coordinator, WorkRejectionReason::kStaleFrameSequence, 1U);
}

TEST(WorkAcceptMatrixTest, UnplannedTokenWithCurrentIdentityReportsStaleFrameSequence) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    static_cast<void>(plan_only(&coordinator, snapshot, rig_frame(rig, 2U)));

    // Never issued, yet every identity field matches the binding context: the
    // not-in-flight path reports the frozen kStaleFrameSequence case.
    WorkToken ghost;
    ghost.source_id = "cam";
    ghost.source_epoch = 1U;
    ghost.frame_sequence = 2U;
    ghost.generation = 7U;
    ghost.policy_revision = 1U;
    ghost.roi = RectF{30.0F, 50.0F, 4.0F, 4.0F};
    ghost.roi_space = CoordinateSpaceId::kOriented;
    ghost.content_digest = 12345U;
    const std::vector<WorkToken> ghost_batch{ghost};
    expect_batch_rejected(coordinator.accept_work(ghost_batch, rig_frame(rig, 2U)),
                          {WorkRejectionReason::kStaleFrameSequence});

    // A token naming a newer frame than the planned one is stale by identity.
    const std::vector<WorkToken> newer_batch{token_with_frame(ghost, 3U)};
    expect_batch_rejected(coordinator.accept_work(newer_batch, rig_frame(rig, 2U)),
                          {WorkRejectionReason::kStaleFrameSequence});
    expect_rejection_count(coordinator, WorkRejectionReason::kStaleFrameSequence, 2U);
}

TEST(WorkAcceptMatrixTest, AdoptedTokenResubmissionIsDuplicateAndCounted) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    const std::vector<WorkToken> tokens = tokens_of(plan);
    expect_batch_accepted(coordinator.accept_work(tokens, frame));

    expect_batch_rejected(coordinator.accept_work(tokens, frame), {WorkRejectionReason::kDuplicateSubmission});
    expect_rejection_count(coordinator, WorkRejectionReason::kDuplicateSubmission, 1U);
}

TEST(WorkAcceptMatrixTest, TamperedFrameFailsThenExactFrameCommits) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    const std::vector<WorkToken> tokens = tokens_of(plan);

    // One byte inside the bound ROI moved since planning.
    CycleRig tampered = rig;
    rig_paint_pixel(&tampered, 12, 12, 41U);
    expect_batch_rejected(coordinator.accept_work(tokens, rig_frame(tampered, 1U)),
                          {WorkRejectionReason::kContentMismatch});
    expect_rejection_count(coordinator, WorkRejectionReason::kContentMismatch, 1U);

    // The failed batch adopted nothing: the exact frame still commits.
    expect_batch_accepted(coordinator.accept_work(tokens, frame));
}

TEST(WorkAcceptMatrixTest, MixedBatchRejectsAtomicallyWithPerItemReasons) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    const WorkToken valid = plan.work.front().token;
    const std::vector<WorkToken> mixed{valid, token_with_source(valid, "other")};

    expect_batch_rejected(coordinator.accept_work(mixed, frame), {std::nullopt, WorkRejectionReason::kForeignSource});
    expect_rejection_count(coordinator, WorkRejectionReason::kForeignSource, 1U);

    // Atomicity: the valid item was not adopted by the rejected batch.
    const std::vector<WorkToken> alone{valid};
    expect_batch_accepted(coordinator.accept_work(alone, frame));
}

TEST(WorkAcceptMatrixTest, EmptyBatchCommitsTriviallyWithoutPolling) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    const CycleRig rig = make_rig();
    const Frame frame = rig_frame(rig, 1U);

    const std::vector<WorkToken> none;
    const WorkAcceptReport report = expect_batch_accepted(coordinator.accept_work(none, frame));
    EXPECT_EQ(report.adopted_count, 0U);
    EXPECT_TRUE(report.item_rejections.empty());

    // No item, no poll point: even a cancelled context commits trivially.
    const WorkAcceptReport cancelled = expect_batch_accepted(coordinator.accept_work(none, frame, cancelled_context()));
    EXPECT_EQ(cancelled.adopted_count, 0U);
}

TEST(WorkAcceptMatrixTest, StructuralAbortsRejectTheWholeCallWithoutCounting) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    const WorkToken valid = plan.work.front().token;

    WorkToken empty_source = valid;
    empty_source.source_id.clear();
    const std::vector<WorkToken> empty_source_batch{empty_source};
    expect_accept_aborted(coordinator.accept_work(empty_source_batch, frame), ErrorCode::kInvalidArgument);

    WorkToken out_of_frame = valid;
    out_of_frame.roi = RectF{40.0F, 40.0F, 20.0F, 20.0F};  // 60 > 48
    const std::vector<WorkToken> out_of_frame_batch{out_of_frame};
    expect_accept_aborted(coordinator.accept_work(out_of_frame_batch, frame), ErrorCode::kInvalidArgument);

    WorkToken zero_roi = valid;
    zero_roi.roi = RectF{0.0F, 0.0F, 0.0F, 10.0F};
    const std::vector<WorkToken> zero_roi_batch{zero_roi};
    expect_accept_aborted(coordinator.accept_work(zero_roi_batch, frame), ErrorCode::kInvalidArgument);

    // Pass 0 precedes pass 1: a valid-looking foreign item before an aborting
    // one must not even be classified (and never counted).
    const std::vector<WorkToken> aborting_mix{valid, empty_source};
    expect_accept_aborted(coordinator.accept_work(aborting_mix, frame), ErrorCode::kInvalidArgument);
    expect_rejection_count(coordinator, WorkRejectionReason::kForeignSource, 0U);
    EXPECT_EQ(total_rejections(coordinator), 0U);

    // Nothing was adopted or destroyed by any aborted call.
    const std::vector<WorkToken> valid_batch{valid};
    expect_batch_accepted(coordinator.accept_work(valid_batch, frame));
}

TEST(WorkAcceptMatrixTest, InvalidOrMultiPlaneFrameAbortsAccept) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    const std::vector<WorkToken> tokens = tokens_of(plan);

    expect_accept_aborted(coordinator.accept_work(tokens, make_null_pixel_frame()), ErrorCode::kInvalidArgument);
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;
    expect_accept_aborted(coordinator.accept_work(tokens, make_nv12_frame(&luma, &chroma)),
                          ErrorCode::kInvalidArgument);
    EXPECT_EQ(total_rejections(coordinator), 0U);

    // The aborted calls left the in-flight set untouched.
    expect_batch_accepted(coordinator.accept_work(tokens, frame));
}

TEST(WorkAcceptMatrixTest, RejectionCountersAccumulateByReasonMonotonically) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    const std::vector<WorkToken> tokens = tokens_of(plan);
    expect_batch_accepted(coordinator.accept_work(tokens, frame));

    expect_batch_rejected(coordinator.accept_work(tokens, frame), {WorkRejectionReason::kDuplicateSubmission});
    expect_batch_rejected(coordinator.accept_work(tokens, frame), {WorkRejectionReason::kDuplicateSubmission});
    const std::vector<WorkToken> foreign{token_with_source(tokens.front(), "other")};
    expect_batch_rejected(coordinator.accept_work(foreign, frame), {WorkRejectionReason::kForeignSource});

    expect_rejection_count(coordinator, WorkRejectionReason::kDuplicateSubmission, 2U);
    expect_rejection_count(coordinator, WorkRejectionReason::kForeignSource, 1U);
    expect_rejection_count(coordinator, WorkRejectionReason::kStaleFrameSequence, 0U);
    expect_rejection_count(coordinator, WorkRejectionReason::kContentMismatch, 0U);
    EXPECT_EQ(total_rejections(coordinator), 3U);
}

TEST(WorkAcceptMatrixTest, SameRegionReacceptRefreshesTheLedgerEntry) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig fill_rig = make_rig();
    rig_paint(&fill_rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &fill_rig, 1U);

    CycleRig drift_rig = make_rig();
    rig_paint(&drift_rig, kFocusA, 41U);
    const Frame drift_frame = rig_frame(drift_rig, 3U);
    const WorkPlan drift = expect_cycle_planned(coordinator.plan_cycle(snapshot, drift_frame));
    expect_batch_accepted(coordinator.accept_work(tokens_of(drift), drift_frame));

    // The refresh replaced the stale entry instead of appending: the static
    // re-verification validates against the new frame, not the old digest.
    const WorkPlan revived = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(drift_rig, 4U)));
    EXPECT_TRUE(revived.work.empty());
    EXPECT_EQ(revived.coverage, CoverageState::kComplete);
    ASSERT_EQ(revived.reuse.size(), 2U);
    expect_reuse_entry(revived.reuse.front(), kFocusA, CoordinateSpaceId::kOriented, 7U, 3U);
}

// --- D. Latest-pending replacement (AC9-07) -----------------------------------------

TEST(ReplacedInflightTest, UnadoptedWorkIsCountedAsReplaced) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});

    static_cast<void>(plan_only(&coordinator, snapshot, rig_frame(rig, 1U)));
    const WorkPlan second = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_EQ(second.replaced_inflight, 1U);
    const WorkPlan third = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U)));
    EXPECT_EQ(third.replaced_inflight, 1U);
}

TEST(ReplacedInflightTest, AdoptedWorkIsNotCountedAsReplaced) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_EQ(plan.replaced_inflight, 0U);
}

TEST(ReplacedInflightTest, TheWholePreviousSetIsReplacedThenDropsToZero) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    expect_engaged(coordinator.set_attention_policy(policy_of(
        {roi_condition(kFocusA, CoordinateSpaceId::kOriented), roi_condition(kFocusB, CoordinateSpaceId::kOriented)})));
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    rig_paint(&rig, kFocusB, 60U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA), make_region(901U, kFocusB)});

    const Frame first_frame = rig_frame(rig, 1U);
    const WorkPlan first = expect_cycle_planned(coordinator.plan_cycle(snapshot, first_frame));
    ASSERT_EQ(first.work.size(), 2U);
    const Frame second_frame = rig_frame(rig, 2U);
    const WorkPlan second = expect_cycle_planned(coordinator.plan_cycle(snapshot, second_frame));
    ASSERT_EQ(second.work.size(), 2U);
    EXPECT_EQ(second.replaced_inflight, 2U);

    expect_batch_accepted(coordinator.accept_work(tokens_of(second), second_frame));
    const WorkPlan third = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U)));
    EXPECT_EQ(third.replaced_inflight, 0U);
}

// --- E. Source reset ------------------------------------------------------------------

TEST(SourceResetCycleTest, ResetBumpsEpochClearsTheLedgerAndResyncsOnce) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    const SourceResetReport report = coordinator.notify_source_reset();
    EXPECT_EQ(report.previous_epoch, 1U);
    EXPECT_EQ(report.new_epoch, 2U);
    EXPECT_EQ(coordinator.source_epoch(), 2U);

    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.resync_required);
    EXPECT_TRUE(plan.reuse.empty());
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
    ASSERT_EQ(plan.work.size(), 1U);
    EXPECT_EQ(plan.work.front().token.source_epoch, 2U);

    const WorkPlan again = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U)));
    EXPECT_FALSE(again.resync_required);
}

TEST(SourceResetCycleTest, OldEpochTokenIsRejectedAsStaleSourceEpoch) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const WorkToken token = plan_only(&coordinator, snapshot, rig_frame(rig, 1U));

    static_cast<void>(coordinator.notify_source_reset());
    expect_batch_rejected(coordinator.accept_work({&token, 1}, rig_frame(rig, 1U)),
                          {WorkRejectionReason::kStaleSourceEpoch});
    expect_rejection_count(coordinator, WorkRejectionReason::kStaleSourceEpoch, 1U);
}

TEST(SourceResetCycleTest, ResetDropsPendingInflightWithoutCountingItReplaced) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    static_cast<void>(plan_only(&coordinator, snapshot, rig_frame(rig, 1U)));

    static_cast<void>(coordinator.notify_source_reset());
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_EQ(plan.replaced_inflight, 0U);
    EXPECT_TRUE(plan.resync_required);
}

// --- F. Budget gates and failure atomicity ---------------------------------------------

TEST(CycleBudgetGateTest, TightBudgetPlanIsRejectedWithStateUntouched) {
    const BudgetProbe probe = probe_fixture_bytes();
    ContinuousPerceptionOptions options = options_with_source("cam");
    options.max_coordination_bytes = probe.engaged + 32;
    ContinuousPerception coordinator = make_coordinator(options);
    engage_focus_policy(&coordinator, kFocusA);
    ASSERT_EQ(coordinator.coordination_owned_bytes(), probe.engaged);

    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    expect_cycle_rejected(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U)), ErrorCode::kBudgetExceeded);

    // Nothing was stored: the account is unchanged, no rejections were counted,
    // and a later zero-work cycle finds neither in-flight descriptors nor
    // ledger entries from the failed plan.
    EXPECT_EQ(coordinator.coordination_owned_bytes(), probe.engaged);
    EXPECT_EQ(total_rejections(coordinator), 0U);
    expect_engaged(coordinator.set_attention_policy(AttentionPolicy{}));
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.work.empty());
    EXPECT_EQ(plan.replaced_inflight, 0U);
    EXPECT_TRUE(plan.reuse.empty());
    EXPECT_TRUE(plan.invalidations.empty());
    EXPECT_EQ(plan.coverage, CoverageState::kIncomplete);
}

TEST(CycleBudgetGateTest, InvalidPlanInputLeavesCycleStateUntouched) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    static_cast<void>(plan_only(&coordinator, snapshot, rig_frame(rig, 1U)));

    const int64_t bytes_before = coordinator.coordination_owned_bytes();
    expect_cycle_rejected(coordinator.plan_cycle(make_snapshot({}, 7, CoordinateSpaceId::kDisplay), rig_frame(rig, 1U)),
                          ErrorCode::kInvalidArgument);
    expect_cycle_rejected(coordinator.plan_cycle(snapshot, make_null_pixel_frame()), ErrorCode::kInvalidArgument);
    expect_cycle_rejected(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U), importance_at(kNaN)),
                          ErrorCode::kInvalidArgument);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes_before);
    EXPECT_EQ(total_rejections(coordinator), 0U);

    // The pending in-flight set survived every rejected call.
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_EQ(plan.replaced_inflight, 1U);

    // A pending resync report also survives rejected calls.
    static_cast<void>(coordinator.notify_source_reset());
    expect_cycle_rejected(coordinator.plan_cycle(snapshot, make_null_pixel_frame()), ErrorCode::kInvalidArgument);
    const WorkPlan after_reset = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U)));
    EXPECT_TRUE(after_reset.resync_required);
}

TEST(CycleBudgetGateTest, EmptySourceIdCoordinatorRejectsCyclesWithStateUntouched) {
    // A coordinator created without a source id (the M9-01 create zero point)
    // could only plan work its own accept would always reject: plan_cycle
    // refuses up front instead of issuing such tokens.
    ContinuousPerception coordinator = make_coordinator();
    expect_engaged(coordinator.set_attention_policy(policy_of({roi_condition(kFocusA, CoordinateSpaceId::kOriented)})));
    const CycleRig rig = make_rig();
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const int64_t bytes_before = coordinator.coordination_owned_bytes();

    expect_cycle_rejected(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U)), ErrorCode::kInvalidArgument);
    // Validation precedes the first context poll: even with a condition that
    // would poll, cancellation cannot mask the source-id rejection.
    expect_cycle_rejected(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U), {}, 0U, cancelled_context()),
                          ErrorCode::kInvalidArgument);

    // Zero side effects: the account holds the engaged policy only (no
    // in-flight descriptor or ledger entry was stored), nothing was counted,
    // and neither the epoch nor the revision moved.
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes_before);
    EXPECT_EQ(total_rejections(coordinator), 0U);
    EXPECT_EQ(coordinator.source_epoch(), 1U);
    EXPECT_EQ(coordinator.policy_revision(), 1U);
}

TEST(CycleBudgetGateTest, TightBudgetAcceptIsRejectedWithInflightPreserved) {
    const BudgetProbe probe = probe_fixture_bytes();
    ContinuousPerceptionOptions options = options_with_source("cam");
    options.max_coordination_bytes = probe.planned + 32;
    ContinuousPerception coordinator = make_coordinator(options);
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});

    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    ASSERT_EQ(coordinator.coordination_owned_bytes(), probe.planned);

    // One more coverage entry would overflow the tightened budget.
    expect_accept_aborted(coordinator.accept_work(tokens_of(plan), frame), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), probe.planned);
    EXPECT_EQ(total_rejections(coordinator), 0U);

    // The issued descriptors are still in flight and the ledger is still empty.
    const WorkPlan next = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_EQ(next.replaced_inflight, 1U);
    EXPECT_TRUE(next.reuse.empty());
    EXPECT_EQ(next.coverage, CoverageState::kIncomplete);
}

// --- G. Byte accounting, determinism and context conversion -----------------------------

TEST(CycleByteAccountingTest, FreshCoordinatorOwnsZeroBytes) {
    const ContinuousPerception plain = make_coordinator();
    EXPECT_EQ(plain.coordination_owned_bytes(), 0);

    const ContinuousPerceptionOptions options = options_with_source("a-longer-source-identity-string");
    const ContinuousPerception configured = make_coordinator(options);
    EXPECT_EQ(configured.coordination_owned_bytes(), 0);  // configuration is caller-owned
}

TEST(CycleByteAccountingTest, CycleStateGrowsTheAccountDeterministically) {
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    ContinuousPerception first = make_coordinator(options_with_source("cam"));
    ContinuousPerception second = make_coordinator(options_with_source("cam"));
    CycleRig rig_a = make_rig();
    CycleRig rig_b = make_rig();
    rig_paint(&rig_a, kFocusA, 40U);
    rig_paint(&rig_b, kFocusA, 40U);
    engage_focus_policy(&first, kFocusA);
    engage_focus_policy(&second, kFocusA);

    ASSERT_GT(first.coordination_owned_bytes(), 0);
    EXPECT_EQ(first.coordination_owned_bytes(), second.coordination_owned_bytes());
    const int64_t engaged = first.coordination_owned_bytes();

    const WorkPlan plan_a = expect_cycle_planned(first.plan_cycle(snapshot, rig_frame(rig_a, 1U)));
    const WorkPlan plan_b = expect_cycle_planned(second.plan_cycle(snapshot, rig_frame(rig_b, 1U)));
    ASSERT_GT(first.coordination_owned_bytes(), engaged);
    EXPECT_EQ(first.coordination_owned_bytes(), second.coordination_owned_bytes());

    const WorkAcceptReport report_a = expect_batch_accepted(first.accept_work(tokens_of(plan_a), rig_frame(rig_a, 1U)));
    const WorkAcceptReport report_b =
        expect_batch_accepted(second.accept_work(tokens_of(plan_b), rig_frame(rig_b, 1U)));
    EXPECT_GT(first.coordination_owned_bytes(), engaged);
    EXPECT_EQ(first.coordination_owned_bytes(), second.coordination_owned_bytes());
    EXPECT_EQ(report_a, report_b);
}

TEST(CycleByteAccountingTest, RepeatedStaticCyclesAreDeterministic) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    // The fill adopt counted as the first valid observation (M9-05); the first
    // static cycle is the second one and queues the single confirmation event.
    // From there the temporal face is settled: the account and the plans are
    // reproducible and the queue stops growing.
    static_cast<void>(expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U))));
    const WorkPlan settled = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U)));
    const int64_t bytes = coordinator.coordination_owned_bytes();
    const WorkPlan first = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 4U)));
    const WorkPlan second = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 5U)));
    EXPECT_EQ(first, second);
    EXPECT_EQ(first, settled);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes);
    EXPECT_EQ(coordinator.query_temporal_state().pending_events, 1U);
}

TEST(CycleContextTest, CancelledCyclePlanConvertsToKCancelled) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});

    expect_cycle_rejected(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U), {}, 0U, cancelled_context()),
                          ErrorCode::kCancelled);
    EXPECT_EQ(total_rejections(coordinator), 0U);
}

TEST(CycleContextTest, ExpiredDeadlineConvertsToKTimeout) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});

    expect_cycle_rejected(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U), {}, 0U, expired_context()),
                          ErrorCode::kTimeout);
}

TEST(CycleContextTest, IdleCycleNeverPollsButEveryWorkSourceDoes) {
    CycleRig rig = make_rig();
    const SemanticSnapshot empty = make_snapshot({}, 7);

    // No policy, no regions, no ledger: not even a cancelled context is polled.
    ContinuousPerception idle = make_coordinator(options_with_source("cam"));
    const WorkPlan plan = expect_cycle_planned(idle.plan_cycle(empty, rig_frame(rig, 1U), {}, 0U, cancelled_context()));
    EXPECT_TRUE(plan.work.empty());
    EXPECT_TRUE(plan.reuse.empty());

    // One uncovered snapshot region: the reuse scan polls.
    ContinuousPerception with_region = make_coordinator(options_with_source("cam"));
    const SemanticSnapshot one_region = make_snapshot({make_region(900U, kFocusA)});
    expect_cycle_rejected(with_region.plan_cycle(one_region, rig_frame(rig, 1U), {}, 0U, cancelled_context()),
                          ErrorCode::kCancelled);

    // One coverage entry: the ledger refresh polls (empty policy keeps the
    // entry loop as the only reachable poll point before cancellation).
    ContinuousPerception with_ledger = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&with_ledger, kFocusA);
    rig_paint(&rig, kFocusA, 40U);
    fill_and_accept(&with_ledger, one_region, &rig, 1U);
    expect_engaged(with_ledger.set_attention_policy(AttentionPolicy{}));
    expect_cycle_rejected(with_ledger.plan_cycle(one_region, rig_frame(rig, 2U), {}, 0U, cancelled_context()),
                          ErrorCode::kCancelled);
}

TEST(CycleContextTest, InputValidationPrecedesTheFirstContextPoll) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    const CycleRig rig = make_rig();
    const SemanticSnapshot display = make_snapshot({}, 7, CoordinateSpaceId::kDisplay);

    expect_cycle_rejected(coordinator.plan_cycle(display, rig_frame(rig, 1U), {}, 0U, cancelled_context()),
                          ErrorCode::kInvalidArgument);
    expect_cycle_rejected(coordinator.plan_cycle(display, make_null_pixel_frame(), {}, 0U, cancelled_context()),
                          ErrorCode::kInvalidArgument);
}

TEST(CycleContextTest, AcceptPollsPerItemBeforeClassifying) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    const Frame frame = rig_frame(rig, 1U);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, frame));
    const std::vector<WorkToken> tokens = tokens_of(plan);

    expect_accept_aborted(coordinator.accept_work(tokens, frame, cancelled_context()), ErrorCode::kCancelled);
    expect_accept_aborted(coordinator.accept_work(tokens, frame, expired_context()), ErrorCode::kTimeout);
    // The conversion is not a rejection: nothing was counted or adopted.
    EXPECT_EQ(total_rejections(coordinator), 0U);
    expect_batch_accepted(coordinator.accept_work(tokens, frame));
}

// --- Coordinate matrix ------------------------------------------------------------------

/// Straight-line oracle sanity checks for the tamper pixels and points.
void expect_pixel_inside_rect(int32_t x, int32_t y, const RectF& rect) {
    const auto px = static_cast<float>(x);
    const auto py = static_cast<float>(y);
    ASSERT_GE(px, rect.x);
    ASSERT_LT(px, rect.x + rect.width);
    ASSERT_GE(py, rect.y);
    ASSERT_LT(py, rect.y + rect.height);
}

void expect_pixel_outside_rect(int32_t x, int32_t y, const RectF& rect) {
    const auto px = static_cast<float>(x);
    const auto py = static_cast<float>(y);
    const bool inside = px >= rect.x && px < rect.x + rect.width && py >= rect.y && py < rect.y + rect.height;
    ASSERT_FALSE(inside);
}

void expect_point_inside_rect(const PointF& point, const RectF& rect) {
    ASSERT_GE(point.x, rect.x);
    ASSERT_LT(point.x, rect.x + rect.width);
    ASSERT_GE(point.y, rect.y);
    ASSERT_LT(point.y, rect.y + rect.height);
}

void engage_focus_policy_in_space(ContinuousPerception* coordinator, const RectF& roi, CoordinateSpaceId space) {
    expect_engaged(coordinator->set_attention_policy(policy_of({roi_condition(roi, space)})));
}

/// The twin of a digest round trip whose only difference is a pixel tamper
/// outside the focus footprint: fill/accept succeeds and the static cycle
/// stays fully covered (the digest covers nothing beyond the focus).
void expect_outside_tamper_twin(const SemanticSnapshot& snapshot, const RectF& focus, Rotation rotation,
                                int32_t outside_x, int32_t outside_y) {
    CycleRig twin_rig = make_rig(rotation);
    rig_paint_pixel(&twin_rig, outside_x, outside_y, 99U);
    ContinuousPerception twin = make_coordinator(options_with_source("cam"));
    engage_focus_policy_in_space(&twin, focus, snapshot.coordinate_space);
    const Frame fill = rig_frame(twin_rig, 1U);
    const WorkPlan plan = expect_cycle_planned(twin.plan_cycle(snapshot, fill));
    ASSERT_EQ(plan.work.size(), 1U);
    expect_batch_accepted(twin.accept_work(tokens_of(plan), fill));
    const WorkPlan same = expect_cycle_planned(twin.plan_cycle(snapshot, rig_frame(twin_rig, 2U)));
    EXPECT_TRUE(same.invalidations.empty());
    EXPECT_EQ(same.coverage, CoverageState::kComplete);
}

/// Drives the digest round trip for one focus/space/rotation combination:
/// fill/accept with the token echo, a static cycle that must stay complete,
/// and the outside-tamper twin.
void expect_digest_round_trip(const SemanticSnapshot& snapshot, const RectF& focus, Rotation rotation,
                              CoordinateSpaceId expected_roi_space, int32_t inside_x, int32_t inside_y,
                              int32_t outside_x, int32_t outside_y) {
    CycleRig rig = make_rig(rotation);
    rig_paint_pixel(&rig, inside_x, inside_y, 99U);
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy_in_space(&coordinator, focus, snapshot.coordinate_space);

    const Frame fill_frame = rig_frame(rig, 1U);
    const WorkPlan fill = expect_cycle_planned(coordinator.plan_cycle(snapshot, fill_frame));
    ASSERT_EQ(fill.work.size(), 1U);
    EXPECT_EQ(fill.work.front().token.roi, focus);
    EXPECT_EQ(fill.work.front().token.roi_space, expected_roi_space);
    expect_batch_accepted(coordinator.accept_work(tokens_of(fill), fill_frame));

    // Static frame: the rotated digest re-verification matches itself.
    const WorkPlan same = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(same.work.empty());
    EXPECT_EQ(same.coverage, CoverageState::kComplete);
    expect_outside_tamper_twin(snapshot, focus, rotation, outside_x, outside_y);
}

/// Drives the digest round trip for one rotation: the oracle-checked tamper
/// points, fill/accept, static reuse, and the outside-tamper twin.
void expect_rotation_round_trip(Rotation rotation, int32_t inside_x, int32_t inside_y, int32_t outside_x,
                                int32_t outside_y) {
    const RectF focus{0.0F, 0.0F, 20.0F, 20.0F};
    const RectF footprint = oriented_footprint(focus, rotation, 48, 64);
    expect_pixel_inside_rect(inside_x, inside_y, footprint);
    expect_pixel_outside_rect(outside_x, outside_y, footprint);
    // The tampered raw pixel is presented inside the focused region.
    expect_point_inside_rect(presented_point(inside_x, inside_y, rotation, 48, 64), focus);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, focus)});
    expect_digest_round_trip(snapshot, focus, rotation, CoordinateSpaceId::kOriented, inside_x, inside_y, outside_x,
                             outside_y);
}

TEST(CycleCoordinateMatrixTest, DigestRoundTripCatchesOnlyFootprintTampersAcrossRotations) {
    // Raw-pixel tamper points per rotation: inside the oriented focus's
    // footprint, and outside it.
    const std::array<std::tuple<Rotation, int32_t, int32_t, int32_t, int32_t>, 4> cases{{
        {Rotation::k0, 5, 5, 40, 50},
        {Rotation::k90, 5, 49, 40, 10},
        {Rotation::k180, 33, 49, 5, 5},
        {Rotation::k270, 33, 5, 5, 50},
    }};
    for (const auto& [rotation, inside_x, inside_y, outside_x, outside_y] : cases) {
        expect_rotation_round_trip(rotation, inside_x, inside_y, outside_x, outside_y);
    }
}

TEST(CycleCoordinateMatrixTest, PaddedStrideNeverFeedsTheDigest) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig(Rotation::k0, 200);  // 8 padding bytes per row
    rig_paint_padding(&rig, 0xAAU);
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    // Same region content, different padding: the re-digest must still match.
    rig_paint_padding(&rig, 0xBBU);
    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 2U)));
    EXPECT_TRUE(plan.invalidations.empty());
    EXPECT_TRUE(plan.work.empty());
    EXPECT_EQ(plan.coverage, CoverageState::kComplete);

    // An in-region tamper under the padded stride is still caught.
    rig_paint_pixel(&rig, 3, 3, 41U);
    const WorkPlan drifted = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 3U)));
    ASSERT_EQ(drifted.invalidations.size(), 1U);
    expect_invalidation(drifted.invalidations.front(), kFocusA, CoordinateSpaceId::kOriented,
                        WorkInvalidationReason::kContentMismatch, false);
}

TEST(CycleCoordinateMatrixTest, OrientedRoiBeyondThePresentedFrameIsRejected) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    // The presented view of a k90 48x64 frame is 64x48; this ROI leaves it.
    expect_engaged(coordinator.set_attention_policy(
        policy_of({roi_condition(RectF{60.0F, 0.0F, 10.0F, 10.0F}, CoordinateSpaceId::kOriented)})));
    const CycleRig rig = make_rig(Rotation::k90);
    const int64_t bytes_before = coordinator.coordination_owned_bytes();
    expect_cycle_rejected(coordinator.plan_cycle(make_snapshot({}, 7), rig_frame(rig, 1U)),
                          ErrorCode::kInvalidArgument);
    EXPECT_EQ(coordinator.coordination_owned_bytes(), bytes_before);
}

TEST(CycleCoordinateMatrixTest, FrameSpaceSnapshotBindsTheRawRectangle) {
    // k90 raw 48x64: the oriented focus {0,0,20,20} lives at raw {0,44,20,20};
    // the tamper (5,49) is inside that footprint, (5,5) outside it.
    const RectF raw_focus{0.0F, 44.0F, 20.0F, 20.0F};
    expect_pixel_inside_rect(5, 49, raw_focus);
    expect_pixel_outside_rect(5, 5, raw_focus);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, raw_focus)}, 7, CoordinateSpaceId::kFrame);
    expect_digest_round_trip(snapshot, raw_focus, Rotation::k90, CoordinateSpaceId::kFrame, 5, 49, 5, 5);
}

// --- H. AC9-07 backlog simulation ---------------------------------------------------------

/// Per-cycle accounting of the backlog simulation.
struct BacklogAccounting {
    uint64_t pending = 0;  // un-adopted descriptors left by the previous plan
    uint64_t issued_total = 0;
    uint64_t adopted_total = 0;
    uint64_t replaced_total = 0;
    int64_t filled_bytes = -1;    // account with one in-flight descriptor (first adoption)
    int64_t drained_bytes = -1;   // account after a static cycle emptied the set
    uint64_t drained_events = 0;  // events pulled by the periodic drains
};

/// Asserts the coordination account stays inside the fixed filled/drained
/// pair established by the first adoption and the first static cycle. The
/// measurement happens right after a drain, so the settled pair carries the
/// cycle state itself — the M9-05 event queue never accumulates across it.
void expect_settled_bytes(BacklogAccounting* accounting, int64_t now) {
    if (accounting->filled_bytes < 0) {
        accounting->filled_bytes = now;
        return;
    }
    if (accounting->drained_bytes < 0 && now < accounting->filled_bytes) {
        accounting->drained_bytes = now;
        return;
    }
    EXPECT_TRUE(now == accounting->filled_bytes || now == accounting->drained_bytes);
    EXPECT_LE(now, accounting->filled_bytes);
}

/// The caller's pressure valve (AC9-09): every 8th cycle drains the M9-05
/// event queue — keeping it bounded no matter how long the simulation runs —
/// and measures the account right after the drain, so the settled pair stays
/// the filled/drained shape of the cycle state itself.
void drain_and_measure(ContinuousPerception* coordinator, BacklogAccounting* accounting, int cycle) {
    if (cycle % 8 != 7 || accounting->adopted_total == 0U) {
        return;
    }
    const Result<EventDrain> drained = coordinator->drain_events();
    EXPECT_TRUE(drained.ok()) << (drained.ok() ? "" : drained.status().message());
    if (drained.ok()) {
        accounting->drained_events += drained.value().events.size();
    }
    expect_settled_bytes(accounting, coordinator->coordination_owned_bytes());
}

/// Runs one processed cycle of the latest-pending policy: plan the newest
/// frame (replacing any un-adopted in-flight descriptors) and accept on every
/// even cycle, leaving the newest work pending otherwise.
void run_backlog_cycle(ContinuousPerception* coordinator, const SemanticSnapshot& snapshot, CycleRig* rig, int cycle,
                       uint64_t frame_sequence, BacklogAccounting* accounting) {
    // Periodic content drift the (fingerprint-stable) change report hides.
    if (cycle % 5 == 2) {
        rig_paint(rig, kFocusA, static_cast<std::uint8_t>(40 + (cycle / 5) % 200));
    }
    const Frame frame = rig_frame(*rig, frame_sequence);
    const WorkPlan plan = expect_cycle_planned(coordinator->plan_cycle(snapshot, frame));
    EXPECT_EQ(plan.replaced_inflight, accounting->pending);
    ASSERT_LE(plan.work.size(), 1U);
    accounting->replaced_total += plan.replaced_inflight;
    accounting->issued_total += plan.work.size();

    if (!plan.work.empty()) {
        const std::vector<WorkToken> tokens = tokens_of(plan);
        if (cycle % 2 == 0) {
            const WorkAcceptReport report = expect_batch_accepted(coordinator->accept_work(tokens, frame));
            accounting->adopted_total += report.adopted_count;
            accounting->pending = 0U;
        } else {
            accounting->pending = tokens.size();  // the newest frame stays pending
        }
    } else {
        accounting->pending = 0U;
    }
    drain_and_measure(coordinator, accounting, cycle);
}

/// Final invariants: every issued descriptor was adopted or replaced exactly
/// once, both settled account values are positive (nothing grew), and the
/// temporal face produced exactly its one-time confirmation event — the queue
/// never grew with duration.
void expect_backlog_totals(const BacklogAccounting& accounting) {
    EXPECT_EQ(accounting.issued_total, accounting.adopted_total + accounting.replaced_total);
    EXPECT_GT(accounting.adopted_total, 0U);
    EXPECT_GT(accounting.replaced_total, 0U);
    EXPECT_GT(accounting.filled_bytes, 0);
    EXPECT_GT(accounting.drained_bytes, 0);
    EXPECT_EQ(accounting.drained_events, 1U);
}

TEST(BacklogSimulationTest, LatestPendingAtFiveFpsOverTenMinutesStaysBounded) {
    constexpr int kCycles = 600;           // 10 minutes at 5 fps processing
    constexpr uint64_t kSkipFrames = 12U;  // 60 fps capture between cycles

    ContinuousPerception coordinator = make_coordinator(options_with_source("sim"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});  // kNone change report

    BacklogAccounting accounting;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        run_backlog_cycle(&coordinator, snapshot, &rig, cycle, (cycle + 1) * kSkipFrames, &accounting);
    }
    expect_backlog_totals(accounting);
    EXPECT_EQ(total_rejections(coordinator), 0U);
}

// --- I. Old-surface isolation ---------------------------------------------------------------

TEST(CycleSurfaceIsolationTest, AttentionPlanStaysUnchangedBesideCycleState) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    // The M9-03 view neither sees the ledger nor claims coverage.
    const Result<AttentionPlan> attention = coordinator.plan_attention(snapshot, rig_frame(rig, 2U));
    ASSERT_TRUE(attention.ok());
    EXPECT_EQ(attention.value().coverage, CoverageState::kIncomplete);
    ASSERT_EQ(attention.value().work.size(), 1U);  // the candidate is not suppressed there
}

TEST(CycleSurfaceIsolationTest, RelationViewBuilderStaysPureBesideCycleState) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    const SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA, "OK", "btn")});
    fill_and_accept(&coordinator, snapshot, &rig, 1U);

    const Result<mirador::RelationView> view = build_relation_view(snapshot);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value().space, CoordinateSpaceId::kOriented);
    ASSERT_EQ(view.value().entities.size(), 1U);
    EXPECT_EQ(view.value().coverage, CoverageState::kIncomplete);
}

TEST(CycleSurfaceIsolationTest, PlanCycleDoesNotMutateSnapshotOrFrameBytes) {
    ContinuousPerception coordinator = make_coordinator(options_with_source("cam"));
    engage_focus_policy(&coordinator, kFocusA);
    CycleRig rig = make_rig();
    rig_paint(&rig, kFocusA, 40U);
    SemanticSnapshot snapshot = make_snapshot({make_region(900U, kFocusA)});
    apply_change(&snapshot, ChangeClassification::kPartial, {RectI{30, 30, 4, 4}});
    const SemanticSnapshot snapshot_copy = snapshot;  // NOLINT(performance-unnecessary-copy-initialization)
    const std::vector<std::uint8_t> pixels_copy = rig.pixels;

    const WorkPlan plan = expect_cycle_planned(coordinator.plan_cycle(snapshot, rig_frame(rig, 1U)));
    EXPECT_EQ(plan.work.size(), 1U);
    EXPECT_EQ(snapshot, snapshot_copy);
    EXPECT_EQ(rig.pixels, pixels_copy);
}

}  // namespace
