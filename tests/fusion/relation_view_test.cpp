// M9-02 relation-view builder tests (DEC-026 section 4, Experimental): the
// fixed fusion fixtures (duplicate / part-of / plain-overlap / conflict /
// disjoint), star-shaped duplicate grouping without transitive chains and
// without stable-id merging, anchor-field entity emission with evidence and
// merge-trace preservation (100% evidence traceability), the smallest-non-zero
// stable-id aggregation, the reliability gate with its inclusive boundaries,
// the whole-batch rejection matrix (illegal geometry, stale generation guard,
// non-positive budgets, tighten-only caps, the min_merge_confidence domain and
// the frozen validation order), the per-cap budget-exceeded paths (entities,
// relations on both emission paths, exact view bytes), determinism plus the
// structural invariants of every successful fixture, the empty / single /
// zero-size boundary shapes, cancellation/deadline conversion at the scan
// stripes, and the old-fusion isolation negative (the snapshot is untouched).
//
// Assertion helpers keep gtest macros out of hand-written loops wherever a
// function would otherwise nest them: macro-expanded control flow inside
// hand-written control flow is what readability-function-cognitive-complexity
// charges for, so per-record checks live in straight-line helpers and pure
// scans return bool for the caller to assert on.

#include <mirador/continuous_perception.hpp>

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::build_relation_view;
using mirador::CoordinateSpaceId;
using mirador::CoverageState;
using mirador::EntityRecord;
using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::find_region;
using mirador::Freshness;
using mirador::is_generation_current;
using mirador::kContinuousMaxCoordinationBytes;
using mirador::kContinuousMaxEntities;
using mirador::kContinuousMaxRelations;
using mirador::RectF;
using mirador::RegionSource;
using mirador::relation_view_consistent;
using mirador::relation_view_owned_bytes;
using mirador::RelationKind;
using mirador::RelationRecord;
using mirador::RelationView;
using mirador::RelationViewOptions;
using mirador::Result;
using mirador::SemanticSnapshot;
using mirador::VisualRegion;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// --- Shared makers and assertion helpers ------------------------------------

[[nodiscard]] double area_of(const RectF& rect) noexcept {
    return static_cast<double>(rect.width) * static_cast<double>(rect.height);
}

VisualRegion make_region(uint64_t stable_id, RectF bounds, std::string text, std::string label = "",
                         float confidence = 0.9F, uint32_t source_mask = static_cast<uint32_t>(RegionSource::kOcr),
                         std::vector<uint64_t> evidence_ids = {}) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.text = std::move(text);
    region.label = std::move(label);
    region.confidence = confidence;
    region.source_mask = source_mask;
    region.evidence_ids = std::move(evidence_ids);
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

/// The fixed duplicate fixture: two "OK" text regions with IoU 90/110, both
/// confident, carrying disjoint evidence and different stable ids.
SemanticSnapshot make_duplicate_pair() {
    return make_snapshot({
        make_region(7, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {5}),
        make_region(3, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {6}),
    });
}

void expect_rejected(const Result<RelationView>& result, ErrorCode code) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), code);
    }
}

/// The frozen emission order: every kDuplicateEvidence record precedes every
/// pairwise record, when scanned in record order.
[[nodiscard]] bool duplicate_records_precede_pairwise(const RelationView& view) noexcept {
    bool seen_pairwise = false;
    for (const RelationRecord& record : view.relations) {
        if (record.kind == RelationKind::kDuplicateEvidence) {
            if (seen_pairwise) {
                return false;
            }
        } else {
            seen_pairwise = true;
        }
    }
    return true;
}

/// Frozen shape of one kDuplicateEvidence record: it names the kept entity on
/// both sides, lists absorbed inputs and carries no decision metrics.
void expect_duplicate_record_shape(const RelationRecord& record) {
    EXPECT_EQ(record.first_entity, record.second_entity);
    EXPECT_FALSE(record.merged_from.empty());
    EXPECT_DOUBLE_EQ(record.iou, 0.0);
    EXPECT_DOUBLE_EQ(record.containment, 0.0);
}

/// Shape checks for one pairwise (non-duplicate) record: no merge trace, and
/// part-of edges running from the smaller-area child to the larger parent.
void expect_pairwise_record_shape(const RelationView& view, const RelationRecord& record) {
    EXPECT_TRUE(record.merged_from.empty());
    if (record.kind == RelationKind::kPartOf) {
        const RectF& child = view.entities[record.first_entity].bounds;
        const RectF& parent = view.entities[record.second_entity].bounds;
        EXPECT_LE(area_of(child), area_of(parent));
    }
}

/// Per-record invariants at one relation index of the view.
void expect_record_invariants(const RelationView& view, size_t index) {
    const RelationRecord& relation = view.relations[index];
    EXPECT_EQ(relation.relation_id, index + 1U);
    if (relation.kind == RelationKind::kDuplicateEvidence) {
        expect_duplicate_record_shape(relation);
    } else {
        expect_pairwise_record_shape(view, relation);
    }
}

/// Structural invariants every successful build must satisfy: consistent by
/// the frozen checker, no view-level coverage claim, the duplicate-record
/// emission order, sequential 1-based relation ids and per-record shapes.
void expect_view_invariants(const RelationView& view) {
    EXPECT_TRUE(relation_view_consistent(view));
    EXPECT_EQ(view.coverage, CoverageState::kIncomplete);
    EXPECT_TRUE(duplicate_records_precede_pairwise(view));
    for (size_t index = 0; index < view.relations.size(); ++index) {
        expect_record_invariants(view, index);
    }
}

/// Builds with the given options and, on success, checks the structural
/// invariants of the view before returning it.
[[nodiscard]] RelationView expect_built(const SemanticSnapshot& snapshot, const RelationViewOptions& options = {},
                                        const ExecutionContext& context = {}) {
    const Result<RelationView> result = build_relation_view(snapshot, options, context);
    EXPECT_TRUE(result.ok());
    if (!result.ok()) {
        return RelationView{};
    }
    expect_view_invariants(result.value());
    return result.value();
}

/// No record of the view may claim `region_index` as a merged input.
void expect_region_never_merged(const RelationView& view, size_t region_index) {
    for (const RelationRecord& record : view.relations) {
        for (const size_t source : record.merged_from) {
            EXPECT_NE(source, region_index);
        }
    }
}

/// Every entity evidence list of the view is ascending and duplicate-free.
void expect_entity_evidence_ascending(const RelationView& view) {
    for (const EntityRecord& entity : view.entities) {
        EXPECT_TRUE(std::is_sorted(entity.evidence_ids.begin(), entity.evidence_ids.end()));
        EXPECT_EQ(std::adjacent_find(entity.evidence_ids.begin(), entity.evidence_ids.end()),
                  entity.evidence_ids.end());
    }
}

/// Every merge trace references input region indexes (in range, ascending).
void expect_merge_traces_valid(const RelationView& view, size_t region_count) {
    for (const RelationRecord& record : view.relations) {
        EXPECT_TRUE(std::is_sorted(record.merged_from.begin(), record.merged_from.end()));
        for (const size_t source : record.merged_from) {
            EXPECT_LT(source, region_count);
        }
    }
}

[[nodiscard]] std::vector<uint64_t> collect_input_evidence(const SemanticSnapshot& snapshot) {
    std::vector<uint64_t> evidence;
    for (const VisualRegion& region : snapshot.regions) {
        evidence.insert(evidence.end(), region.evidence_ids.begin(), region.evidence_ids.end());
    }
    return evidence;
}

[[nodiscard]] std::vector<uint64_t> collect_view_evidence(const RelationView& view) {
    std::vector<uint64_t> evidence;
    for (const EntityRecord& entity : view.entities) {
        evidence.insert(evidence.end(), entity.evidence_ids.begin(), entity.evidence_ids.end());
    }
    return evidence;
}

// --- A. Fixed fusion fixtures -------------------------------------------------

TEST(RelationViewFixtureTest, DuplicatePairMergesIntoOneAnchorEntityWithFullTrace) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 1U);
    EXPECT_EQ(view.space, snapshot.coordinate_space);

    // The kept entity is the anchor: anchor bounds (not the union), anchor
    // confidence, snapshot generation/space, kUnvalidated freshness, and the
    // aggregates over both members.
    EntityRecord expected;
    expected.stable_id = 3;  // smallest non-zero member id, not the anchor's 7
    expected.generation = 7;
    expected.bounds = RectF{0.0F, 0.0F, 10.0F, 10.0F};
    expected.space = CoordinateSpaceId::kOriented;
    expected.source_mask = RegionSource::kOcr | RegionSource::kDetector;
    expected.confidence = 0.9F;
    expected.freshness = Freshness::kUnvalidated;
    expected.evidence_ids = {5, 6};
    EXPECT_EQ(view.entities.front(), expected);

    ASSERT_EQ(view.relations.size(), 1U);
    const RelationRecord& record = view.relations.front();
    EXPECT_EQ(record.kind, RelationKind::kDuplicateEvidence);
    EXPECT_EQ(record.relation_id, 1U);
    EXPECT_EQ(record.first_entity, 0U);
    EXPECT_EQ(record.second_entity, 0U);
    EXPECT_EQ(record.merged_from, std::vector<size_t>{1});
}

TEST(RelationViewFixtureTest, PartOfPairEmitsChildToParentEdgeWithDecisionTrace) {
    // Button [0,0,20,10] (area 200) with contained text [2,2,4,2] (area 8):
    // containment 1.0 >= 0.6 and size_ratio 0.04 <= 0.5 form a component.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(0, RectF{0.0F, 0.0F, 20.0F, 10.0F}, "", "button", 0.9F, static_cast<uint32_t>(RegionSource::kOcr),
                    {1}),
        make_region(0, RectF{2.0F, 2.0F, 4.0F, 2.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {2}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    EXPECT_EQ(view.entities[0].bounds, (RectF{0.0F, 0.0F, 20.0F, 10.0F}));
    EXPECT_EQ(view.entities[1].bounds, (RectF{2.0F, 2.0F, 4.0F, 2.0F}));

    ASSERT_EQ(view.relations.size(), 1U);
    const RelationRecord& edge = view.relations.front();
    EXPECT_EQ(edge.kind, RelationKind::kPartOf);
    EXPECT_EQ(edge.first_entity, 1U);   // the text: the smaller-area child
    EXPECT_EQ(edge.second_entity, 0U);  // the button: the parent
    EXPECT_DOUBLE_EQ(edge.iou, 8.0 / 200.0);
    EXPECT_DOUBLE_EQ(edge.containment, 1.0);
    // Both sides stay independently queryable with their own evidence.
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>{1});
    EXPECT_EQ(view.entities[1].evidence_ids, std::vector<uint64_t>{2});
}

TEST(RelationViewFixtureTest, PlainOverlapKeepsBothSidesIndependent) {
    // Compatible semantics but insufficient duplicate geometry (IoU 50/150,
    // containment 0.5): a plain overlap edge, no absorption.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {1}),
        make_region(2, RectF{5.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {2}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    ASSERT_EQ(view.relations.size(), 1U);
    const RelationRecord& edge = view.relations.front();
    EXPECT_EQ(edge.kind, RelationKind::kOverlap);
    EXPECT_EQ(edge.first_entity, 0U);
    EXPECT_EQ(edge.second_entity, 1U);
    EXPECT_DOUBLE_EQ(edge.iou, 50.0 / 150.0);
    EXPECT_DOUBLE_EQ(edge.containment, 0.5);
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>{1});
    EXPECT_EQ(view.entities[1].evidence_ids, std::vector<uint64_t>{2});
}

TEST(RelationViewFixtureTest, ConflictPreservesBothEvidenceSides) {
    // Same-box duplicate geometry with contradicting fully specified labels:
    // recorded as a conflict; the evidence of both sides survives.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(0, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "", "button", 0.9F, static_cast<uint32_t>(RegionSource::kOcr),
                    {1}),
        make_region(0, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "", "icon", 0.9F,
                    static_cast<uint32_t>(RegionSource::kDetector), {2}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>{1});
    EXPECT_EQ(view.entities[1].evidence_ids, std::vector<uint64_t>{2});
    ASSERT_EQ(view.relations.size(), 1U);
    const RelationRecord& edge = view.relations.front();
    EXPECT_EQ(edge.kind, RelationKind::kConflict);
    EXPECT_EQ(edge.first_entity, 0U);
    EXPECT_EQ(edge.second_entity, 1U);
    EXPECT_DOUBLE_EQ(edge.iou, 1.0);
    EXPECT_DOUBLE_EQ(edge.containment, 1.0);
}

TEST(RelationViewFixtureTest, DisjointPairProducesNoEdges) {
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {1}),
        make_region(2, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {2}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    EXPECT_TRUE(view.relations.empty());
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>{1});
    EXPECT_EQ(view.entities[1].evidence_ids, std::vector<uint64_t>{2});
}

// --- B. Star-shaped duplicate grouping ---------------------------------------

TEST(RelationViewGroupingTest, StarGroupingNeverChainsThroughAnAbsorbedMember) {
    // r1 duplicates the anchor r0 (IoU 90/110). r2 duplicates r1 (IoU 90/110)
    // but not the anchor (IoU 80/120 = 0.667 < 0.8): absorption is never
    // chained through an absorbed member, so r2 stays an independent entity.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(7, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {1}),
        make_region(3, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {2}),
        make_region(9, RectF{2.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kTemplate),
                    {3}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    EXPECT_EQ(view.entities[0].bounds, (RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    EXPECT_EQ(view.entities[0].stable_id, 3U);
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>({1, 2}));
    EXPECT_EQ(view.entities[1].bounds, (RectF{2.0F, 0.0F, 10.0F, 10.0F}));
    EXPECT_EQ(view.entities[1].stable_id, 9U);
    EXPECT_EQ(view.entities[1].evidence_ids, std::vector<uint64_t>{3});

    ASSERT_EQ(view.relations.size(), 2U);
    EXPECT_EQ(view.relations[0].kind, RelationKind::kDuplicateEvidence);
    EXPECT_EQ(view.relations[0].first_entity, 0U);
    EXPECT_EQ(view.relations[0].second_entity, 0U);
    EXPECT_EQ(view.relations[0].merged_from, std::vector<size_t>{1});
    // The surviving pair (anchor, r2) replays as a plain overlap.
    EXPECT_EQ(view.relations[1].kind, RelationKind::kOverlap);
    EXPECT_EQ(view.relations[1].first_entity, 0U);
    EXPECT_EQ(view.relations[1].second_entity, 1U);
    EXPECT_DOUBLE_EQ(view.relations[1].iou, 80.0 / 120.0);
    EXPECT_DOUBLE_EQ(view.relations[1].containment, 0.8);
    // No record claims input region 2 as merged.
    expect_region_never_merged(view, 2U);
}

TEST(RelationViewGroupingTest, EntitiesFollowAnchorFirstSeenOrder) {
    // Group {0} then group {1, 2}: the kept entity of the second group is the
    // anchor r1 (bounds [0,0,10,10]), never the absorbed r2.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{50.0F, 50.0F, 5.0F, 5.0F}, "A", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {1}),
        make_region(2, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {2}),
        make_region(3, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {3}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    EXPECT_EQ(view.entities[0].bounds, (RectF{50.0F, 50.0F, 5.0F, 5.0F}));
    EXPECT_EQ(view.entities[1].bounds, (RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    ASSERT_EQ(view.relations.size(), 1U);
    EXPECT_EQ(view.relations[0].kind, RelationKind::kDuplicateEvidence);
    EXPECT_EQ(view.relations[0].first_entity, 1U);
    EXPECT_EQ(view.relations[0].second_entity, 1U);
    EXPECT_EQ(view.relations[0].merged_from, std::vector<size_t>{2});
}

TEST(RelationViewGroupingTest, EqualStableIdsNeverMergeWithoutDuplicateGeometry) {
    // Regions never merge on stable ids alone: disjoint geometry with equal
    // ids stays two entities.
    const SemanticSnapshot snapshot = make_snapshot(
        {make_region(9, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"), make_region(9, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "OK")});
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    EXPECT_EQ(view.entities[0].stable_id, 9U);
    EXPECT_EQ(view.entities[1].stable_id, 9U);
    EXPECT_TRUE(view.relations.empty());
}

// --- C. Evidence traceability -------------------------------------------------

TEST(RelationViewTraceabilityTest, EveryInputEvidenceIdSurvivesTheBuild) {
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(0, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr),
                    {4, 2}),
        make_region(0, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {3, 1}),
        make_region(0, RectF{0.0F, 1.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kTemplate),
                    {5}),
        make_region(0, RectF{30.0F, 30.0F, 8.0F, 8.0F}, "Hi", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {7}),
        make_region(0, RectF{60.0F, 60.0F, 8.0F, 8.0F}, "Yo", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {6}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 3U);
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>({1, 2, 3, 4, 5}));
    EXPECT_EQ(view.entities[1].evidence_ids, std::vector<uint64_t>{7});
    EXPECT_EQ(view.entities[2].evidence_ids, std::vector<uint64_t>{6});
    expect_entity_evidence_ascending(view);

    // The union over the view equals the union over the input regions:
    // nothing is dropped.
    std::vector<uint64_t> input_evidence = collect_input_evidence(snapshot);
    std::vector<uint64_t> view_evidence = collect_view_evidence(view);
    std::sort(input_evidence.begin(), input_evidence.end());
    std::sort(view_evidence.begin(), view_evidence.end());
    EXPECT_EQ(input_evidence, view_evidence);

    // Merge traces reference input region indexes, ascending and in range.
    expect_merge_traces_valid(view, snapshot.regions.size());
    ASSERT_EQ(view.relations.size(), 1U);
    EXPECT_EQ(view.relations[0].merged_from, std::vector<size_t>({1, 2}));
}

// --- D. stable_id aggregation --------------------------------------------------

TEST(RelationViewIdentityTest, StableIdAggregatesToTheSmallestNonZeroMemberId) {
    // All members untracked: the kept entity stays untracked.
    const SemanticSnapshot untracked = make_snapshot({
        make_region(0, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(0, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK"),
    });
    const RelationView zero_view = expect_built(untracked);
    ASSERT_EQ(zero_view.entities.size(), 1U);
    EXPECT_EQ(zero_view.entities[0].stable_id, 0U);

    // Mixed tracked/untracked members: zero ids never mask a real one.
    const SemanticSnapshot mixed = make_snapshot({
        make_region(0, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(5, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK"),
    });
    const RelationView mixed_view = expect_built(mixed);
    ASSERT_EQ(mixed_view.entities.size(), 1U);
    EXPECT_EQ(mixed_view.entities[0].stable_id, 5U);
}

// --- E. Reliability gate -------------------------------------------------------

TEST(RelationViewReliabilityTest, BelowTheGateDuplicateGeometryDegradesToOverlap) {
    // Identical boxes with identical text but confidence 0.3 below the 0.5
    // default: duplicate geometry degrades to a plain overlap, both sides keep
    // their evidence, and the overlap edge still carries the geometry trace.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.3F, static_cast<uint32_t>(RegionSource::kOcr), {1}),
        make_region(2, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.3F, static_cast<uint32_t>(RegionSource::kDetector),
                    {2}),
    });
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 2U);
    ASSERT_EQ(view.relations.size(), 1U);
    EXPECT_EQ(view.relations[0].kind, RelationKind::kOverlap);
    EXPECT_DOUBLE_EQ(view.relations[0].iou, 1.0);
    EXPECT_DOUBLE_EQ(view.relations[0].containment, 1.0);
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>{1});
    EXPECT_EQ(view.entities[1].evidence_ids, std::vector<uint64_t>{2});
}

TEST(RelationViewReliabilityTest, LoweringTheGateRestoresTheMerge) {
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.3F, static_cast<uint32_t>(RegionSource::kOcr), {1}),
        make_region(2, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.3F, static_cast<uint32_t>(RegionSource::kDetector),
                    {2}),
    });
    RelationViewOptions options;
    options.min_merge_confidence = 0.2F;
    const RelationView view = expect_built(snapshot, options);
    ASSERT_EQ(view.entities.size(), 1U);
    ASSERT_EQ(view.relations.size(), 1U);
    EXPECT_EQ(view.relations[0].kind, RelationKind::kDuplicateEvidence);
    EXPECT_EQ(view.relations[0].merged_from, std::vector<size_t>{1});
    EXPECT_EQ(view.entities[0].evidence_ids, std::vector<uint64_t>({1, 2}));
}

TEST(RelationViewReliabilityTest, GateBoundariesZeroAndOneAreInclusive) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    RelationViewOptions permissive;
    permissive.min_merge_confidence = 0.0F;
    const RelationView merged = expect_built(snapshot, permissive);
    EXPECT_EQ(merged.entities.size(), 1U);

    RelationViewOptions strict;
    strict.min_merge_confidence = 1.0F;
    const RelationView split = expect_built(snapshot, strict);
    ASSERT_EQ(split.entities.size(), 2U);
    ASSERT_EQ(split.relations.size(), 1U);
    EXPECT_EQ(split.relations[0].kind, RelationKind::kOverlap);
}

// --- F. Whole-batch rejection matrix ------------------------------------------

TEST(RelationViewRejectionTest, AnyIllegalGeometryRejectsTheWholeBatch) {
    const RectF valid{0.0F, 0.0F, 10.0F, 10.0F};
    const std::vector<RectF> invalid{
        RectF{kNaN, 0.0F, 10.0F, 10.0F}, RectF{0.0F, kNaN, 10.0F, 10.0F}, RectF{0.0F, 0.0F, kNaN, 10.0F},
        RectF{0.0F, 0.0F, 10.0F, kNaN},  RectF{kInf, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, -1.0F, 10.0F},
        RectF{0.0F, 0.0F, 10.0F, -1.0F},
    };
    for (const RectF& bounds : invalid) {
        expect_rejected(build_relation_view(make_snapshot({make_region(1, bounds, "OK")}), {}),
                        ErrorCode::kInvalidArgument);
        // A legal region before the illegal one must not produce a partial
        // view either: the batch is rejected as a whole.
        expect_rejected(
            build_relation_view(make_snapshot({make_region(1, valid, "OK"), make_region(2, bounds, "OK")}), {}),
            ErrorCode::kInvalidArgument);
    }
}

TEST(RelationViewRejectionTest, GenerationGuardRejectsForeignBatches) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    EXPECT_EQ(snapshot.generation, 7U);

    RelationViewOptions stale;
    stale.expected_generation = 8U;
    expect_rejected(build_relation_view(snapshot, stale), ErrorCode::kInvalidArgument);

    RelationViewOptions current;
    current.expected_generation = 7U;
    static_cast<void>(expect_built(snapshot, current));

    // Unset guard: every generation is accepted.
    static_cast<void>(expect_built(snapshot, RelationViewOptions{}));
}

TEST(RelationViewRejectionTest, NonPositiveBudgetsAreInvalidArgument) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    for (const int32_t bad : {int32_t{0}, int32_t{-1}}) {
        RelationViewOptions entities;
        entities.max_entities = bad;
        expect_rejected(build_relation_view(snapshot, entities), ErrorCode::kInvalidArgument);

        RelationViewOptions relations;
        relations.max_relations = bad;
        expect_rejected(build_relation_view(snapshot, relations), ErrorCode::kInvalidArgument);

        RelationViewOptions bytes;
        bytes.max_view_bytes = bad;
        expect_rejected(build_relation_view(snapshot, bytes), ErrorCode::kInvalidArgument);
    }
}

TEST(RelationViewRejectionTest, CapsLooserThanReferenceConstantsAreBudgetExceeded) {
    const SemanticSnapshot snapshot = make_duplicate_pair();

    RelationViewOptions entities;
    entities.max_entities = kContinuousMaxEntities + 1;
    expect_rejected(build_relation_view(snapshot, entities), ErrorCode::kBudgetExceeded);

    RelationViewOptions relations;
    relations.max_relations = kContinuousMaxRelations + 1;
    expect_rejected(build_relation_view(snapshot, relations), ErrorCode::kBudgetExceeded);

    RelationViewOptions bytes;
    bytes.max_view_bytes = kContinuousMaxCoordinationBytes + 1;
    expect_rejected(build_relation_view(snapshot, bytes), ErrorCode::kBudgetExceeded);
}

TEST(RelationViewRejectionTest, ExactReferenceCapsAreAccepted) {
    const SemanticSnapshot snapshot = make_duplicate_pair();

    RelationViewOptions options;
    options.max_entities = kContinuousMaxEntities;
    options.max_relations = kContinuousMaxRelations;
    options.max_view_bytes = kContinuousMaxCoordinationBytes;
    static_cast<void>(expect_built(snapshot, options));
}

TEST(RelationViewRejectionTest, MinMergeConfidenceMustBeFiniteWithinUnitRange) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    for (const float bad : {-0.1F, 1.5F, kNaN, kInf}) {
        RelationViewOptions options;
        options.min_merge_confidence = bad;
        expect_rejected(build_relation_view(snapshot, options), ErrorCode::kInvalidArgument);
    }
}

TEST(RelationViewRejectionTest, FrozenValidationOrderIsObservableThroughDistinctCodes) {
    // Options positivity precedes cap looseness: a zero entity cap with a
    // too-loose byte cap reports kInvalidArgument, not kBudgetExceeded.
    RelationViewOptions positivity_first;
    positivity_first.max_entities = 0;
    positivity_first.max_view_bytes = kContinuousMaxCoordinationBytes + 1;
    expect_rejected(build_relation_view(make_duplicate_pair(), positivity_first), ErrorCode::kInvalidArgument);

    // Whole-batch geometry precedes the budget scan: an illegal region in a
    // snapshot that would also exceed max_entities reports kInvalidArgument.
    const SemanticSnapshot bad_geometry = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(2, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "OK"),
        make_region(3, RectF{40.0F, 40.0F, 5.0F, -2.0F}, "OK"),
    });
    RelationViewOptions tight;
    tight.max_entities = 1;
    expect_rejected(build_relation_view(bad_geometry, tight), ErrorCode::kInvalidArgument);
}

// --- G. Budget overflow --------------------------------------------------------

TEST(RelationViewBudgetTest, EntitiesAboveTheCapAreBudgetExceeded) {
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(2, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "OK"),
        make_region(3, RectF{40.0F, 40.0F, 5.0F, 5.0F}, "OK"),
    });
    RelationViewOptions tight;
    tight.max_entities = 2;
    expect_rejected(build_relation_view(snapshot, tight), ErrorCode::kBudgetExceeded);

    RelationViewOptions exact;
    exact.max_entities = 3;
    static_cast<void>(expect_built(snapshot, exact));
}

TEST(RelationViewBudgetTest, RelationsAboveTheCapAreBudgetExceededOnBothEmissionPaths) {
    // Pairwise path: three mutually overlapping entities produce three edges.
    const SemanticSnapshot chain = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(2, RectF{4.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(3, RectF{8.0F, 0.0F, 10.0F, 10.0F}, "OK"),
    });
    RelationViewOptions pairwise_tight;
    pairwise_tight.max_relations = 2;
    expect_rejected(build_relation_view(chain, pairwise_tight), ErrorCode::kBudgetExceeded);
    RelationViewOptions pairwise_exact;
    pairwise_exact.max_relations = 3;
    const RelationView chain_view = expect_built(chain, pairwise_exact);
    ASSERT_EQ(chain_view.relations.size(), 3U);

    // Duplicate-record path: two duplicate groups emit two records first.
    const SemanticSnapshot two_groups = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(2, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(3, RectF{40.0F, 40.0F, 10.0F, 10.0F}, "Hi"),
        make_region(4, RectF{41.0F, 40.0F, 10.0F, 10.0F}, "Hi"),
    });
    RelationViewOptions duplicate_tight;
    duplicate_tight.max_relations = 1;
    expect_rejected(build_relation_view(two_groups, duplicate_tight), ErrorCode::kBudgetExceeded);
    RelationViewOptions duplicate_exact;
    duplicate_exact.max_relations = 2;
    const RelationView groups_view = expect_built(two_groups, duplicate_exact);
    ASSERT_EQ(groups_view.relations.size(), 2U);
    EXPECT_EQ(groups_view.relations[0].kind, RelationKind::kDuplicateEvidence);
    EXPECT_EQ(groups_view.relations[1].kind, RelationKind::kDuplicateEvidence);
}

TEST(RelationViewBudgetTest, ViewBytesAboveTheCapAreBudgetExceeded) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    const RelationView reference = expect_built(snapshot);
    const int64_t bytes = relation_view_owned_bytes(reference);
    EXPECT_GT(bytes, 0);

    RelationViewOptions exact;
    exact.max_view_bytes = bytes;
    static_cast<void>(expect_built(snapshot, exact));

    RelationViewOptions tight;
    tight.max_view_bytes = bytes - 1;
    expect_rejected(build_relation_view(snapshot, tight), ErrorCode::kBudgetExceeded);
}

// --- H. Determinism ------------------------------------------------------------

TEST(RelationViewDeterminismTest, RepeatedBuildsWithEqualInputsAreEqual) {
    // A composite batch exercising all four record kinds across two groups,
    // a part-of pair, an overlap and a conflict.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(7, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {1}),
        make_region(3, RectF{1.0F, 0.0F, 10.0F, 10.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {2}),
        make_region(0, RectF{0.0F, 0.0F, 20.0F, 10.0F}, "", "button", 0.9F, static_cast<uint32_t>(RegionSource::kOcr),
                    {3}),
        make_region(0, RectF{2.0F, 2.0F, 4.0F, 2.0F}, "OK", "", 0.9F, static_cast<uint32_t>(RegionSource::kDetector),
                    {4}),
        make_region(0, RectF{50.0F, 0.0F, 10.0F, 10.0F}, "", "button", 0.9F, static_cast<uint32_t>(RegionSource::kOcr),
                    {5}),
        make_region(0, RectF{50.0F, 0.0F, 10.0F, 10.0F}, "", "icon", 0.9F,
                    static_cast<uint32_t>(RegionSource::kDetector), {6}),
        make_region(0, RectF{80.0F, 80.0F, 5.0F, 5.0F}, "Yo", "", 0.9F, static_cast<uint32_t>(RegionSource::kOcr), {7}),
    });
    const RelationView first = expect_built(snapshot);
    const RelationView second = expect_built(snapshot);
    EXPECT_EQ(first, second);
    EXPECT_FALSE(first.relations.empty());
}

// --- I. Boundary shapes --------------------------------------------------------

TEST(RelationViewBoundaryTest, EmptySnapshotBuildsAnEmptyViewWithoutCoverageClaim) {
    const SemanticSnapshot snapshot = make_snapshot({}, 7, CoordinateSpaceId::kDisplay);
    const RelationView view = expect_built(snapshot);
    EXPECT_TRUE(view.entities.empty());
    EXPECT_TRUE(view.relations.empty());
    EXPECT_EQ(view.space, CoordinateSpaceId::kDisplay);
    EXPECT_EQ(view.coverage, CoverageState::kIncomplete);
    EXPECT_TRUE(relation_view_consistent(view));
}

TEST(RelationViewBoundaryTest, SingleRegionBuildsOneEdgelessEntityWithPropagatedFields) {
    const SemanticSnapshot snapshot = make_snapshot({make_region(11, RectF{3.0F, 4.0F, 5.0F, 6.0F}, "Hi", "menu", 0.7F,
                                                                 RegionSource::kExternal | RegionSource::kOcr, {9})},
                                                    7, CoordinateSpaceId::kDisplay);
    const RelationView view = expect_built(snapshot);
    ASSERT_EQ(view.entities.size(), 1U);
    EXPECT_TRUE(view.relations.empty());

    EntityRecord expected;
    expected.stable_id = 11;
    expected.generation = 7;
    expected.bounds = RectF{3.0F, 4.0F, 5.0F, 6.0F};
    expected.space = CoordinateSpaceId::kDisplay;
    expected.source_mask = RegionSource::kExternal | RegionSource::kOcr;
    expected.confidence = 0.7F;
    expected.freshness = Freshness::kUnvalidated;
    expected.evidence_ids = {9};
    EXPECT_EQ(view.entities.front(), expected);
}

TEST(RelationViewBoundaryTest, ZeroSizeRegionsAreLegalIsolatedEntities) {
    const SemanticSnapshot single = make_snapshot({make_region(1, RectF{5.0F, 5.0F, 0.0F, 0.0F}, "OK")});
    const RelationView single_view = expect_built(single);
    ASSERT_EQ(single_view.entities.size(), 1U);
    EXPECT_TRUE(single_view.relations.empty());

    // Coincident zero-size regions carry no intersection: no merge, no edge.
    const SemanticSnapshot pair = make_snapshot({
        make_region(1, RectF{5.0F, 5.0F, 0.0F, 0.0F}, "OK"),
        make_region(2, RectF{5.0F, 5.0F, 0.0F, 0.0F}, "OK"),
    });
    const RelationView pair_view = expect_built(pair);
    ASSERT_EQ(pair_view.entities.size(), 2U);
    EXPECT_TRUE(pair_view.relations.empty());
}

// --- J. Cancellation and deadline ---------------------------------------------

TEST(RelationViewContextTest, CancellationCallbackConvertsToKCancelled) {
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(2, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "OK"),
    });
    ExecutionContext context;
    context.is_cancelled = [] { return true; };
    expect_rejected(build_relation_view(snapshot, {}, context), ErrorCode::kCancelled);
}

TEST(RelationViewContextTest, PastDeadlineConvertsToKTimeout) {
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(2, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "OK"),
    });
    ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    expect_rejected(build_relation_view(snapshot, {}, context), ErrorCode::kTimeout);
}

TEST(RelationViewContextTest, CancellationTakesPrecedenceOverTheDeadline) {
    // The repo-wide stage-boundary convention (fuse_evidence, sessions, the
    // stable-id tracker): a cancellation is reported before a passed deadline.
    const SemanticSnapshot snapshot = make_snapshot({
        make_region(1, RectF{0.0F, 0.0F, 10.0F, 10.0F}, "OK"),
        make_region(2, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "OK"),
    });
    ExecutionContext context;
    context.is_cancelled = [] { return true; };
    context.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    expect_rejected(build_relation_view(snapshot, {}, context), ErrorCode::kCancelled);
}

TEST(RelationViewContextTest, EmptyContextBuildsTheFullView) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    const ExecutionContext context;
    const RelationView view = expect_built(snapshot, {}, context);
    ASSERT_EQ(view.entities.size(), 1U);
    ASSERT_EQ(view.relations.size(), 1U);
}

TEST(RelationViewContextTest, ValidationErrorsPrecedeTheFirstCancellationPoll) {
    // The context is polled between scan stripes only: the frozen validation
    // pipeline (options, generation guard, geometry) runs before it and wins.
    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };

    RelationViewOptions zero_budget;
    zero_budget.max_entities = 0;
    expect_rejected(build_relation_view(make_duplicate_pair(), zero_budget, cancelled), ErrorCode::kInvalidArgument);

    RelationViewOptions stale;
    stale.expected_generation = 8U;
    expect_rejected(build_relation_view(make_duplicate_pair(), stale, cancelled), ErrorCode::kInvalidArgument);
}

// --- K. Old-fusion isolation ---------------------------------------------------

TEST(RelationViewIsolationTest, BuildingTheViewLeavesTheSnapshotUntouched) {
    const SemanticSnapshot snapshot = make_duplicate_pair();
    // The copy is the point: the saved pre-build value is what the snapshot is
    // compared against after both builds.
    const SemanticSnapshot copy = snapshot;  // NOLINT(performance-unnecessary-copy-initialization)

    // Two different builds over the same snapshot (merging and non-merging):
    // the side-view builder changes nothing on the old fusion output.
    static_cast<void>(build_relation_view(snapshot, {}));
    RelationViewOptions strict;
    strict.min_merge_confidence = 1.0F;
    static_cast<void>(build_relation_view(snapshot, strict));

    EXPECT_EQ(snapshot, copy);
    // The old-API surface over the same snapshot keeps working unchanged.
    EXPECT_NE(find_region(snapshot, 7), nullptr);
    EXPECT_NE(find_region(snapshot, 3), nullptr);
    EXPECT_EQ(find_region(snapshot, 99), nullptr);
    EXPECT_TRUE(is_generation_current(snapshot, snapshot.generation));
}

}  // namespace
