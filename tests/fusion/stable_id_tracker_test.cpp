// M4 stable-id tracker tests (M4-04, DEC-010): gated greedy one-to-one
// matching, inclusive gate boundaries, text signal, split/merge events,
// generation bump policy, deterministic tie-breaks, failure atomicity, reset
// and budgets.

#include <mirador/stable_id_tracker.hpp>

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::IdEvent;
using mirador::PointF;
using mirador::RectF;
using mirador::StableIdOptions;
using mirador::StableIdReport;
using mirador::StableIdTracker;
using mirador::VisualRegion;

VisualRegion make_region(float x, float y, float width, float height, std::string text = {}) {
    VisualRegion region;
    region.bounds = RectF{x, y, width, height};
    region.anchor = PointF{x + width / 2.0F, y + height / 2.0F};
    region.text = std::move(text);
    region.stable_id = 12345U;  // tracker output must not depend on input ids
    return region;
}

mirador::Result<StableIdReport> advance(StableIdTracker& tracker, const std::vector<VisualRegion>& regions,
                                        const StableIdOptions& options = {}, const ExecutionContext& context = {}) {
    return tracker.advance(std::span<const VisualRegion>{regions.data(), regions.size()}, options, context);
}

// --- Retention and fresh ids ---------------------------------------------------------

TEST(StableIdTrackerTest, FirstAdvanceAllocatesIdsFromOne) {
    StableIdTracker tracker;
    const auto report =
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(50.0F, 0.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().assignments.size(), 2U);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kNew);
    EXPECT_EQ(report.value().assignments[1].stable_id, 2U);
    EXPECT_EQ(report.value().assignments[1].event, IdEvent::kNew);
    EXPECT_EQ(report.value().retained_count, 0U);
    EXPECT_FALSE(report.value().generation_bump);  // no previous state: nothing to bump
    EXPECT_EQ(tracker.tracked_count(), 2U);
    EXPECT_EQ(tracker.last_id(), 2U);
}

TEST(StableIdTrackerTest, SmallDisplacementRetainsIdentity) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const auto report = advance(tracker, {make_region(1.0F, 1.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().retained_count, 1U);
    EXPECT_FALSE(report.value().generation_bump);
}

TEST(StableIdTrackerTest, LargeDisplacementAllocatesFreshId) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const auto report = advance(tracker, {make_region(200.0F, 200.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].stable_id, 2U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kNew);
    EXPECT_TRUE(report.value().generation_bump);  // 0 retained of 1 < 0.5 ratio
}

// --- Gate boundaries ------------------------------------------------------------------

TEST(StableIdTrackerTest, CenterGateAdmitsDisplacementExactlyAtRadius) {
    // Previous 30x40 region: diagonal exactly 50, center gate radius 0.5*50=25.
    // The candidate at {25,0,30,40} has center displacement exactly 25 (centers
    // (15,20) -> (40,20)) and IoU 200/2200 < 0.3, so the center gate alone must
    // admit it (inclusive).
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 30.0F, 40.0F)}).ok());
    const auto report = advance(tracker, {make_region(25.0F, 0.0F, 30.0F, 40.0F)});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
}

TEST(StableIdTrackerTest, CenterGateRejectsDisplacementJustBeyondRadius) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 30.0F, 40.0F)}).ok());
    // Displacement 25.5 > radius 25 (centers (15,20) -> (40.5,20)); IoU still
    // below the match threshold.
    const auto report = advance(tracker, {make_region(25.5F, 0.0F, 30.0F, 40.0F)});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kNew);
    EXPECT_EQ(report.value().assignments[0].stable_id, 2U);
    EXPECT_TRUE(report.value().generation_bump);
}

// --- Text signal ------------------------------------------------------------------------

TEST(StableIdTrackerTest, MatchingTextWinsASymmetricGeometricTie) {
    StableIdTracker tracker;
    // Previous 100x100 region, text normalized (trim + collapse) before the
    // similarity cost.
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 100.0F, 100.0F, "  hello   world ")}).ok());

    // Two candidates with identical IoU and displacement; only the text signal
    // differs. radius = 0.5*hypot(100,100) ~ 70.7 admits both (displacement 30).
    const std::vector<VisualRegion> current{make_region(30.0F, 0.0F, 100.0F, 100.0F, "hello world"),
                                            make_region(-30.0F, 0.0F, 100.0F, 100.0F, "goodbye")};
    const auto report = advance(tracker, current);
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 2U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
    EXPECT_EQ(report.value().assignments[1].event, IdEvent::kNew);
    EXPECT_EQ(report.value().assignments[1].stable_id, 2U);
}

// --- Split and merge ----------------------------------------------------------------------

TEST(StableIdTrackerTest, SplitProducesSplitChildrenAndBumpsGeneration) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 100.0F, 100.0F)}).ok());

    // Two non-overlapping 50x50 sub-blocks of the previous region. With the
    // default center gate every interior center is admitted, so the previous
    // region would match one child; shrinking center_gate_ratio to 0.2 makes
    // the previous region have no matching candidates (IoU 0.25 < 0.3,
    // displacement 35.4 > radius 28.3), which is the DEC-010 split case.
    StableIdOptions options;
    options.center_gate_ratio = 0.2;
    const auto report =
        advance(tracker, {make_region(0.0F, 0.0F, 50.0F, 50.0F), make_region(50.0F, 50.0F, 50.0F, 50.0F)}, options);
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 2U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kSplitChild);
    EXPECT_EQ(report.value().assignments[1].event, IdEvent::kSplitChild);
    EXPECT_EQ(report.value().assignments[0].stable_id, 2U);
    EXPECT_EQ(report.value().assignments[1].stable_id, 3U);
    EXPECT_EQ(report.value().split_count, 1U);
    EXPECT_EQ(report.value().retained_count, 0U);
    EXPECT_TRUE(report.value().generation_bump);
}

TEST(StableIdTrackerTest, MergeProducesMergedEventAndBumpsGeneration) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 20.0F, 20.0F), make_region(80.0F, 0.0F, 20.0F, 20.0F)}).ok());

    // One current region whose bounds contain both previous centers; neither
    // previous region passes the gates (IoU 0.125 < 0.3, displacement 44.7 >
    // radius 14.1), so both are swallowed by the fresh region.
    const auto report = advance(tracker, {make_region(0.0F, 0.0F, 100.0F, 60.0F)});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 1U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kMerged);
    EXPECT_EQ(report.value().assignments[0].stable_id, 3U);
    EXPECT_EQ(report.value().merge_count, 1U);
    EXPECT_EQ(report.value().retained_count, 0U);
    EXPECT_TRUE(report.value().generation_bump);
}

// --- Generation retention policy ------------------------------------------------------------

TEST(StableIdTrackerTest, RetentionAtOrAboveRatioDoesNotBump) {
    StableIdTracker tracker;
    ASSERT_TRUE(
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(100.0F, 100.0F, 10.0F, 10.0F)}).ok());

    // One retained + one moved far away: retained/prev = 0.5, not below 0.5.
    const auto report =
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(300.0F, 300.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().retained_count, 1U);
    EXPECT_EQ(report.value().new_count, 1U);
    EXPECT_FALSE(report.value().generation_bump);
}

TEST(StableIdTrackerTest, FewAdditionsDoNotBump) {
    StableIdTracker tracker;
    ASSERT_TRUE(
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(100.0F, 100.0F, 10.0F, 10.0F)}).ok());

    // Both previous regions retained, one brand-new addition: no bump.
    const auto report =
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(100.0F, 100.0F, 10.0F, 10.0F),
                          make_region(300.0F, 300.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 3U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[1].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[2].event, IdEvent::kNew);
    EXPECT_EQ(report.value().assignments[2].stable_id, 3U);
    EXPECT_FALSE(report.value().generation_bump);
}

TEST(StableIdTrackerTest, AllPreviousRegionsLostBumpsGeneration) {
    StableIdTracker tracker;
    ASSERT_TRUE(
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(100.0F, 100.0F, 10.0F, 10.0F)}).ok());
    const auto report =
        advance(tracker, {make_region(300.0F, 300.0F, 10.0F, 10.0F), make_region(500.0F, 500.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().retained_count, 0U);
    EXPECT_EQ(report.value().new_count, 2U);
    EXPECT_TRUE(report.value().generation_bump);
}

// --- Determinism ------------------------------------------------------------------------------

TEST(StableIdTrackerTest, EqualCostCandidatesResolveByCurIndex) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(50.0F, 0.0F, 10.0F, 10.0F)}).ok());

    // Both candidates have identical IoU (50/150) and displacement (5); the
    // greedy tie-break must pick the lower cur index.
    const auto report =
        advance(tracker, {make_region(45.0F, 0.0F, 10.0F, 10.0F), make_region(55.0F, 0.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().assignments.size(), 2U);
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
    EXPECT_EQ(report.value().assignments[1].event, IdEvent::kNew);
    EXPECT_EQ(report.value().assignments[1].stable_id, 2U);
}

// --- Failure atomicity, reset, budgets -----------------------------------------------------------

TEST(StableIdTrackerTest, FailedAdvanceLeavesStateUntouched) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    ASSERT_EQ(tracker.tracked_count(), 1U);

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const auto failed = advance(
        tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(50.0F, 50.0F, 10.0F, 10.0F)}, {}, cancelled);
    EXPECT_EQ(failed.status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(tracker.tracked_count(), 1U);
    EXPECT_EQ(tracker.last_id(), 1U);

    // A successful advance still works and matches the untouched state.
    const auto report = advance(tracker, {make_region(0.5F, 0.5F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kRetained);
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
}

TEST(StableIdTrackerTest, ResetClearsStateAndIdSpace) {
    StableIdTracker tracker;
    ASSERT_TRUE(
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(100.0F, 0.0F, 10.0F, 10.0F)}).ok());
    ASSERT_EQ(tracker.tracked_count(), 2U);
    ASSERT_EQ(tracker.last_id(), 2U);

    tracker.reset();
    EXPECT_EQ(tracker.tracked_count(), 0U);
    EXPECT_EQ(tracker.last_id(), 0U);

    const auto report = advance(tracker, {make_region(100.0F, 0.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);  // id space restarted
    EXPECT_EQ(report.value().assignments[0].event, IdEvent::kNew);
}

TEST(StableIdTrackerTest, AdvanceWithNoRegionsBumpsAndClearsTracking) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    const auto report = advance(tracker, {});
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.value().assignments.empty());
    EXPECT_TRUE(report.value().generation_bump);  // 0 retained of 1
    EXPECT_EQ(tracker.tracked_count(), 0U);
}

TEST(StableIdTrackerTest, InvalidOptionsAreRejectedWithoutStateChange) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());

    StableIdOptions zero_weights;
    zero_weights.weight_iou = 0.0;
    zero_weights.weight_center = 0.0;
    zero_weights.weight_text = 0.0;
    EXPECT_EQ(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}, zero_weights).status().code(),
              ErrorCode::kInvalidArgument);

    StableIdOptions bad_gate;
    bad_gate.center_gate_ratio = -1.0;
    EXPECT_EQ(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}, bad_gate).status().code(),
              ErrorCode::kInvalidArgument);

    StableIdOptions bad_ratio;
    bad_ratio.generation_retention_ratio = 1.5;
    EXPECT_EQ(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}, bad_ratio).status().code(),
              ErrorCode::kInvalidArgument);

    StableIdOptions bad_budget;
    bad_budget.max_regions = 0;
    EXPECT_EQ(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}, bad_budget).status().code(),
              ErrorCode::kInvalidArgument);

    EXPECT_EQ(tracker.tracked_count(), 1U);  // nothing committed
}

TEST(StableIdTrackerTest, MoreRegionsThanMaxRegionsFailsAndKeepsState) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());

    StableIdOptions options;
    options.max_regions = 1;
    const auto failed =
        advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F), make_region(100.0F, 0.0F, 10.0F, 10.0F)}, options);
    EXPECT_EQ(failed.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(tracker.tracked_count(), 1U);
}

TEST(StableIdTrackerTest, DeadlineAdvanceReportsTimeout) {
    StableIdTracker tracker;
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const auto report = advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}, {}, expired);
    EXPECT_EQ(report.status().code(), ErrorCode::kTimeout);
    EXPECT_EQ(tracker.tracked_count(), 0U);
}

TEST(StableIdTrackerTest, TrackerIsMoveOnlyAndStateTravelsWithTheMove) {
    StableIdTracker tracker;
    ASSERT_TRUE(advance(tracker, {make_region(0.0F, 0.0F, 10.0F, 10.0F)}).ok());
    StableIdTracker moved = std::move(tracker);
    EXPECT_EQ(moved.tracked_count(), 1U);
    const auto report = advance(moved, {make_region(0.0F, 0.0F, 10.0F, 10.0F)});
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().assignments[0].stable_id, 1U);
}

}  // namespace
