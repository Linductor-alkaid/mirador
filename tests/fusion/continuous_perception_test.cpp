// M9-01 continuous-perception contract tests (DEC-026, Experimental): the
// frozen reference limits and byte-sized enums, the per-pair relation decision
// table with inclusive threshold boundaries, decision traces, symmetry and
// invalid-geometry rejection, the class/text compatibility predicate, relation
// view invariants and byte accounting, attention-policy validation, the
// field-sensitive policy digest and focus resolution with the frozen criterion
// priority, work-token validation and the frozen staleness check order,
// semantic-event invariants and byte accounting, and the coordinator identity
// core (create validation, source-epoch lifecycle, zero coordination bytes,
// move-only semantics, twin determinism).

#include <mirador/continuous_perception.hpp>

#include <mirador/geometry.hpp>
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
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using mirador::attention_policy_digest;
using mirador::AttentionCondition;
using mirador::AttentionPolicy;
using mirador::classify_entity_relation;
using mirador::ContinuousPerception;
using mirador::ContinuousPerceptionOptions;
using mirador::CoordinateSpaceId;
using mirador::CoverageState;
using mirador::entity_semantics_compatible;
using mirador::EntityRecord;
using mirador::EntitySemantics;
using mirador::ErrorCode;
using mirador::EventSemantic;
using mirador::FocusUnresolvedReason;
using mirador::Freshness;
using mirador::kContinuousMaxCoordinationBytes;
using mirador::kContinuousMaxEntities;
using mirador::kContinuousMaxEvents;
using mirador::kContinuousMaxFocusConditions;
using mirador::kContinuousMaxRelations;
using mirador::ObservationState;
using mirador::RectF;
using mirador::relation_view_consistent;
using mirador::relation_view_owned_bytes;
using mirador::RelationClassification;
using mirador::RelationClassifyOptions;
using mirador::RelationDecision;
using mirador::RelationKind;
using mirador::RelationRecord;
using mirador::RelationView;
using mirador::resolve_attention_focus;
using mirador::semantic_event_consistent;
using mirador::semantic_event_owned_bytes;
using mirador::SemanticChangeKind;
using mirador::SemanticEvent;
using mirador::SemanticSnapshot;
using mirador::SourceResetReport;
using mirador::validate_attention_policy;
using mirador::validate_work_token;
using mirador::VisualRegion;
using mirador::work_token_stale_reason;
using mirador::WorkBindingContext;
using mirador::WorkRejectionReason;
using mirador::WorkToken;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// --- Shared makers ----------------------------------------------------------

void expect_invalid_argument(const mirador::Result<void>& result) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
    }
}

void expect_budget_exceeded(const mirador::Result<void>& result) {
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), ErrorCode::kBudgetExceeded);
    }
}

void expect_invalid_bounds(const RectF& lhs, const RectF& rhs) {
    const auto result = classify_entity_relation(lhs, rhs, true, true);
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
    }
}

void expect_symmetric_pair(const RectF& rect_one, const RectF& rect_two, bool semantics_compatible,
                           bool reliability_supported) {
    RelationDecision forward;
    RelationDecision backward;
    const auto forward_result =
        classify_entity_relation(rect_one, rect_two, semantics_compatible, reliability_supported, {}, &forward);
    // Deliberately swapped arguments: the rule must be symmetric in both bounds.
    const auto backward_result =
        classify_entity_relation(rect_two, rect_one, semantics_compatible, reliability_supported, {}, &backward);
    ASSERT_TRUE(forward_result.ok());
    ASSERT_TRUE(backward_result.ok());
    EXPECT_EQ(forward_result.value(), backward_result.value());
    EXPECT_EQ(forward.iou, backward.iou);
    EXPECT_EQ(forward.containment, backward.containment);
    EXPECT_EQ(forward.size_ratio, backward.size_ratio);
}

void expect_create_error(ContinuousPerceptionOptions options, ErrorCode expected) {
    const auto result = ContinuousPerception::create(std::move(options));
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
        EXPECT_EQ(result.status().code(), expected);
    }
}

void expect_classification(RelationClassification expected, const RectF& bounds_a, const RectF& bounds_b,
                           bool semantics_compatible, bool reliability_supported,
                           const RelationClassifyOptions& options = {}) {
    const auto result =
        classify_entity_relation(bounds_a, bounds_b, semantics_compatible, reliability_supported, options);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), expected);
}

VisualRegion make_region(uint64_t stable_id, RectF bounds, std::string text, std::string label) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.text = std::move(text);
    region.label = std::move(label);
    return region;
}

/// Snapshot with a duplicate text across two regions (stored-order probe),
/// two label peers and its own space/generation identity.
SemanticSnapshot make_focused_snapshot() {
    SemanticSnapshot snapshot;
    snapshot.frame_sequence = 42;
    snapshot.generation = 7;
    snapshot.coordinate_space = CoordinateSpaceId::kDisplay;
    snapshot.regions.push_back(make_region(100, RectF{1.0F, 1.0F, 10.0F, 10.0F}, "Save", "button"));
    snapshot.regions.push_back(make_region(200, RectF{20.0F, 20.0F, 5.0F, 5.0F}, "Save", "icon"));
    snapshot.regions.push_back(make_region(300, RectF{40.0F, 40.0F, 8.0F, 8.0F}, "Quit", "button"));
    return snapshot;
}

EntityRecord make_entity(RectF bounds, std::vector<uint64_t> evidence_ids = {}) {
    EntityRecord entity;
    entity.stable_id = 11;
    entity.generation = 1;
    entity.bounds = bounds;
    entity.space = CoordinateSpaceId::kOriented;
    entity.source_mask = 1;
    entity.confidence = 0.9F;
    entity.freshness = Freshness::kValidatedReuse;
    entity.evidence_ids = std::move(evidence_ids);
    return entity;
}

RelationRecord make_relation(RelationKind kind, size_t first_entity, size_t second_entity,
                             std::vector<size_t> merged_from = {}) {
    RelationRecord relation;
    relation.relation_id = 1;
    relation.kind = kind;
    relation.first_entity = first_entity;
    relation.second_entity = second_entity;
    relation.merged_from = std::move(merged_from);
    return relation;
}

AttentionCondition roi_condition() {
    AttentionCondition condition;
    condition.roi = RectF{5.0F, 5.0F, 20.0F, 20.0F};
    condition.roi_space = CoordinateSpaceId::kFrame;
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

SemanticEvent make_event(EventSemantic semantic, SemanticChangeKind change_kind) {
    SemanticEvent event;
    event.event_sequence = 1;
    event.semantic = semantic;
    event.change_kind = change_kind;
    event.stable_id = 7;
    event.window.source_id = "cam";
    event.window.source_epoch = 1;
    event.window.observed_frame = 3;
    event.window.confirmed_frame = 4;
    event.bounds = RectF{0.0F, 0.0F, 10.0F, 10.0F};
    return event;
}

WorkToken make_token() {
    WorkToken token;
    token.source_id = "cam";
    token.source_epoch = 2;
    token.frame_sequence = 10;
    token.generation = 5;
    token.policy_revision = 3;
    token.request_digest = 0xABCD;
    token.roi = RectF{1.0F, 2.0F, 30.0F, 20.0F};
    token.roi_space = CoordinateSpaceId::kOriented;
    token.content_digest = 0x1234;
    return token;
}

WorkBindingContext make_context() {
    WorkBindingContext context;
    context.source_id = "cam";
    context.source_epoch = 2;
    context.frame_sequence = 10;
    context.generation = 5;
    context.policy_revision = 3;
    return context;
}

ContinuousPerceptionOptions make_coordinator_options(std::string source_id) {
    ContinuousPerceptionOptions options;
    options.source_id = std::move(source_id);
    return options;
}

// --- Reference limits and frozen enums --------------------------------------

TEST(ContinuousPerceptionConstants, ReferenceLimitsMatchDec026) {
    static_assert(kContinuousMaxCoordinationBytes == int64_t{8} * 1024 * 1024);
    static_assert(kContinuousMaxEntities == 1024);
    static_assert(kContinuousMaxRelations == 4096);
    static_assert(kContinuousMaxEvents == 256);
    static_assert(kContinuousMaxFocusConditions == 64);
    SUCCEED();
}

TEST(ContinuousPerceptionConstants, EnumsAreByteSizedWithFrozenEnumeratorSets) {
    static_assert(std::is_same_v<std::underlying_type_t<Freshness>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<ObservationState>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<EventSemantic>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<SemanticChangeKind>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<RelationKind>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<RelationClassification>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<WorkRejectionReason>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<FocusUnresolvedReason>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<CoverageState>, uint8_t>);

    const std::vector<Freshness> freshness{Freshness::kFresh, Freshness::kValidatedReuse, Freshness::kUnvalidated,
                                           Freshness::kInvalidated};
    const std::vector<ObservationState> observation{ObservationState::kObserved, ObservationState::kOccluded,
                                                    ObservationState::kTrackingLost, ObservationState::kUnanalyzed};
    const std::vector<EventSemantic> semantic{EventSemantic::kBegin, EventSemantic::kUpdate, EventSemantic::kEnd};
    const std::vector<SemanticChangeKind> change{
        SemanticChangeKind::kAppeared,        SemanticChangeKind::kTextChanged, SemanticChangeKind::kAppearanceChanged,
        SemanticChangeKind::kRelationChanged, SemanticChangeKind::kDisappeared, SemanticChangeKind::kSourceReset};
    const std::vector<RelationKind> relation_kind{RelationKind::kDuplicateEvidence, RelationKind::kPartOf,
                                                  RelationKind::kOverlap, RelationKind::kConflict};
    const std::vector<RelationClassification> classification{
        RelationClassification::kNone, RelationClassification::kDuplicate, RelationClassification::kPartOf,
        RelationClassification::kOverlap, RelationClassification::kConflict};
    const std::vector<WorkRejectionReason> rejection{
        WorkRejectionReason::kForeignSource,       WorkRejectionReason::kStaleSourceEpoch,
        WorkRejectionReason::kStalePolicyRevision, WorkRejectionReason::kStaleGeneration,
        WorkRejectionReason::kStaleFrameSequence,  WorkRejectionReason::kDuplicateSubmission,
        WorkRejectionReason::kContentMismatch};
    const std::vector<FocusUnresolvedReason> focus{
        FocusUnresolvedReason::kUnknownStableId, FocusUnresolvedReason::kStaleGeneration,
        FocusUnresolvedReason::kMissingLocalization, FocusUnresolvedReason::kNotObserved};
    const std::vector<CoverageState> coverage{CoverageState::kComplete, CoverageState::kIncomplete};
    EXPECT_EQ(freshness.size(), 4U);
    EXPECT_EQ(observation.size(), 4U);
    EXPECT_EQ(semantic.size(), 3U);
    EXPECT_EQ(change.size(), 6U);
    EXPECT_EQ(relation_kind.size(), 4U);
    EXPECT_EQ(classification.size(), 5U);
    EXPECT_EQ(rejection.size(), 7U);
    EXPECT_EQ(focus.size(), 4U);
    EXPECT_EQ(coverage.size(), 2U);
}

// --- classify_entity_relation: frozen decision table ------------------------

TEST(ClassifyEntityRelationTest, DisjointPairClassifiesNoneAndStillWritesTrace) {
    RelationDecision trace;
    trace.iou = -1.0;
    const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{20.0F, 20.0F, 5.0F, 5.0F}, true,
                                                 true, {}, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kNone);
    EXPECT_DOUBLE_EQ(trace.iou, 0.0);
    EXPECT_DOUBLE_EQ(trace.containment, 0.0);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 25.0 / 100.0);
}

TEST(ClassifyEntityRelationTest, EdgeTouchingPairHasNoIntersection) {
    expect_classification(RelationClassification::kNone, RectF{0.0F, 0.0F, 10.0F, 10.0F},
                          RectF{10.0F, 0.0F, 5.0F, 5.0F}, true, true);
}

TEST(ClassifyEntityRelationTest, IdenticalRectsAreDuplicateWithExactMetrics) {
    RelationDecision trace;
    const auto result = classify_entity_relation(RectF{2.0F, 3.0F, 10.0F, 10.0F}, RectF{2.0F, 3.0F, 10.0F, 10.0F}, true,
                                                 true, {}, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kDuplicate);
    EXPECT_DOUBLE_EQ(trace.iou, 1.0);
    EXPECT_DOUBLE_EQ(trace.containment, 1.0);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 1.0);
}

TEST(ClassifyEntityRelationTest, IdenticalRectsWithConflictingClaimsClassifyConflictRegardlessOfReliability) {
    expect_classification(RelationClassification::kConflict, RectF{2.0F, 3.0F, 10.0F, 10.0F},
                          RectF{2.0F, 3.0F, 10.0F, 10.0F}, false, true);
    expect_classification(RelationClassification::kConflict, RectF{2.0F, 3.0F, 10.0F, 10.0F},
                          RectF{2.0F, 3.0F, 10.0F, 10.0F}, false, false);
}

TEST(ClassifyEntityRelationTest, IdenticalRectsWithCompatibleSemanticsButNoReliabilityStayIndependent) {
    // Reliability only gates the merge: duplicate geometry without it degrades
    // to plain overlap, nothing is destroyed (DEC-026 section 4).
    expect_classification(RelationClassification::kOverlap, RectF{2.0F, 3.0F, 10.0F, 10.0F},
                          RectF{2.0F, 3.0F, 10.0F, 10.0F}, true, false);
}

TEST(ClassifyEntityRelationTest, IouBoundaryIsInclusiveOnTheDuplicatePath) {
    // A={0,0,10,10} (area 100), B={0,0,10,5} (area 50): iou = 50/100 = 0.5
    // exactly, containment = 1.0, size_ratio = 0.5. The containment duplicate
    // path cannot fire (0.5 is not > 0.5), so the IoU path decides.
    RelationDecision trace;
    RelationClassifyOptions options;
    options.duplicate_iou = 0.5;
    const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, 5.0F}, true,
                                                 true, options, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kDuplicate);
    EXPECT_DOUBLE_EQ(trace.iou, 0.5);
    EXPECT_DOUBLE_EQ(trace.containment, 1.0);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 0.5);
}

TEST(ClassifyEntityRelationTest, IouBelowCustomThresholdLeavesPartOfGeometry) {
    // Same pair with the threshold just above the measured IoU: the pair falls
    // through to part-of (containment 1.0 >= 0.6, size_ratio 0.5 <= 0.5).
    RelationClassifyOptions options;
    options.duplicate_iou = 0.6;
    expect_classification(RelationClassification::kPartOf, RectF{0.0F, 0.0F, 10.0F, 10.0F},
                          RectF{0.0F, 0.0F, 10.0F, 5.0F}, true, true, options);
}

TEST(ClassifyEntityRelationTest, DuplicateContainmentBoundaryIsInclusive) {
    // A={0,0,8,8}, B={0,1,8,8}: both area 64, intersection 56, so
    // containment = 56/64 = 0.875 exactly, size_ratio = 1.0 and
    // iou = 56/72 < 0.8 keeps the IoU path out of the decision.
    RelationDecision trace;
    RelationClassifyOptions options;
    options.duplicate_containment = 0.875;
    const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 8.0F, 8.0F}, RectF{0.0F, 1.0F, 8.0F, 8.0F}, true,
                                                 true, options, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kDuplicate);
    EXPECT_DOUBLE_EQ(trace.containment, 0.875);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 1.0);
    EXPECT_DOUBLE_EQ(trace.iou, 56.0 / 72.0);
}

TEST(ClassifyEntityRelationTest, DuplicateContainmentBelowThresholdWithSimilarSizesStaysOverlap) {
    RelationClassifyOptions options;
    options.duplicate_containment = 0.88;
    // containment 0.875 < 0.88: no duplicate geometry; part-of is out because
    // size_ratio 1.0 is not <= 0.5; positive intersection remains.
    expect_classification(RelationClassification::kOverlap, RectF{0.0F, 0.0F, 8.0F, 8.0F},
                          RectF{0.0F, 1.0F, 8.0F, 8.0F}, true, true, options);
}

TEST(ClassifyEntityRelationTest, DuplicateGeometryViaContainmentPathClassifiesConflictWhenIncompatible) {
    // A={0,0,10,10}, B={0,0,10,6}: containment 1.0 >= 0.9 and
    // size_ratio 0.6 > 0.5 form duplicate geometry without the IoU path
    // (60/100 = 0.6 < 0.8); contradicting claims turn it into a conflict.
    RelationDecision trace;
    const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, 6.0F}, false,
                                                 true, {}, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kConflict);
    EXPECT_DOUBLE_EQ(trace.iou, 0.6);
    EXPECT_DOUBLE_EQ(trace.containment, 1.0);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 0.6);
}

TEST(ClassifyEntityRelationTest, DuplicateGeometryViaContainmentPathDegradesToOverlapWithoutReliability) {
    expect_classification(RelationClassification::kOverlap, RectF{0.0F, 0.0F, 10.0F, 10.0F},
                          RectF{0.0F, 0.0F, 10.0F, 6.0F}, true, false);
}

TEST(ClassifyEntityRelationTest, PartOfSizeRatioBoundaryIsInclusiveUnderDefaults) {
    // A={0,0,10,10} (area 100), B={0,0,10,5} (area 50): containment 1.0 and
    // size_ratio exactly 0.5 <= 0.5 make a component, not a similar-size
    // duplicate (which would require size_ratio strictly greater).
    RelationDecision trace;
    const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, 5.0F}, true,
                                                 true, {}, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kPartOf);
    EXPECT_DOUBLE_EQ(trace.iou, 0.5);
    EXPECT_DOUBLE_EQ(trace.containment, 1.0);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 0.5);
}

TEST(ClassifyEntityRelationTest, SizeRatioJustAboveBoundaryIsDuplicateGeometryViaContainment) {
    expect_classification(RelationClassification::kDuplicate, RectF{0.0F, 0.0F, 10.0F, 10.0F},
                          RectF{0.0F, 0.0F, 10.0F, 6.0F}, true, true);
}

TEST(ClassifyEntityRelationTest, PartOfContainmentBoundaryIsInclusiveWithCustomOptions) {
    // A={0,0,8,8} (area 64), B={0,5,8,4} (area 32): intersection 24,
    // containment = 24/32 = 0.75 exactly, size_ratio = 0.5.
    RelationDecision trace;
    RelationClassifyOptions options;
    options.part_of_containment = 0.75;
    const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 8.0F, 8.0F}, RectF{0.0F, 5.0F, 8.0F, 4.0F}, true,
                                                 true, options, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kPartOf);
    EXPECT_DOUBLE_EQ(trace.containment, 0.75);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 0.5);
}

TEST(ClassifyEntityRelationTest, PartOfContainmentBelowThresholdStaysOverlap) {
    RelationClassifyOptions options;
    options.part_of_containment = 0.76;
    expect_classification(RelationClassification::kOverlap, RectF{0.0F, 0.0F, 8.0F, 8.0F},
                          RectF{0.0F, 5.0F, 8.0F, 4.0F}, true, true, options);
}

TEST(ClassifyEntityRelationTest, ContainedComponentWithIncompatibleSemanticsStaysIndependent) {
    // Part-of geometry (containment 1.0, size_ratio 0.25) but contradicting
    // claims: no part-of edge, plain overlap keeps both sides.
    expect_classification(RelationClassification::kOverlap, RectF{0.0F, 0.0F, 10.0F, 10.0F},
                          RectF{0.0F, 0.0F, 5.0F, 5.0F}, false, true);
}

TEST(ClassifyEntityRelationTest, ContainmentWithOversizedComponentStaysOverlap) {
    // A={0,0,8,8} (64), B={0,3,5,8} (40): intersection 25, containment
    // 25/40 = 0.625 >= 0.6 but size_ratio 40/64 = 0.625 > 0.5 blocks part-of,
    // and containment 0.625 < 0.9 plus iou 25/79 < 0.8 block duplicate.
    RelationDecision trace;
    const auto result =
        classify_entity_relation(RectF{0.0F, 0.0F, 8.0F, 8.0F}, RectF{0.0F, 3.0F, 5.0F, 8.0F}, true, true, {}, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kOverlap);
    EXPECT_DOUBLE_EQ(trace.iou, 25.0 / 79.0);
    EXPECT_DOUBLE_EQ(trace.containment, 0.625);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 0.625);
}

TEST(ClassifyEntityRelationTest, PlainPartialOverlapClassifiesOverlap) {
    RelationDecision trace;
    const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{5.0F, 0.0F, 10.0F, 10.0F}, true,
                                                 true, {}, &trace);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), RelationClassification::kOverlap);
    EXPECT_DOUBLE_EQ(trace.iou, 50.0 / 150.0);
    EXPECT_DOUBLE_EQ(trace.containment, 0.5);
    EXPECT_DOUBLE_EQ(trace.size_ratio, 1.0);
}

TEST(ClassifyEntityRelationTest, DecisionIsSymmetricInBothBounds) {
    const std::vector<std::pair<RectF, RectF>> pairs{
        {RectF{2.0F, 3.0F, 10.0F, 10.0F}, RectF{2.0F, 3.0F, 10.0F, 10.0F}},
        {RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{20.0F, 20.0F, 5.0F, 5.0F}},
        {RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, 5.0F}},
        {RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, 6.0F}},
        {RectF{0.0F, 0.0F, 8.0F, 8.0F}, RectF{0.0F, 3.0F, 5.0F, 8.0F}},
        {RectF{0.0F, 0.0F, 8.0F, 8.0F}, RectF{0.0F, 5.0F, 8.0F, 4.0F}},
    };
    for (const auto& pair : pairs) {
        for (const bool compatible : {true, false}) {
            for (const bool reliable : {true, false}) {
                expect_symmetric_pair(pair.first, pair.second, compatible, reliable);
            }
        }
    }
}

TEST(ClassifyEntityRelationTest, RepeatedCallsAreBitwiseDeterministic) {
    RelationDecision first_trace;
    const auto first = classify_entity_relation(RectF{0.0F, 0.0F, 8.0F, 8.0F}, RectF{0.0F, 1.0F, 8.0F, 8.0F}, true,
                                                true, {}, &first_trace);
    ASSERT_TRUE(first.ok());
    for (int iteration = 0; iteration < 8; ++iteration) {
        RelationDecision trace;
        const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 8.0F, 8.0F}, RectF{0.0F, 1.0F, 8.0F, 8.0F}, true,
                                                     true, {}, &trace);
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result.value(), first.value());
        EXPECT_EQ(trace.iou, first_trace.iou);
        EXPECT_EQ(trace.containment, first_trace.containment);
        EXPECT_EQ(trace.size_ratio, first_trace.size_ratio);
    }
}

TEST(ClassifyEntityRelationTest, DevelopmentDefaultsMatchTheDec026Pin) {
    const RelationClassifyOptions defaults{};
    EXPECT_DOUBLE_EQ(defaults.duplicate_iou, 0.80);
    EXPECT_DOUBLE_EQ(defaults.duplicate_containment, 0.90);
    EXPECT_DOUBLE_EQ(defaults.part_of_containment, 0.60);
    EXPECT_DOUBLE_EQ(defaults.part_of_max_size_ratio, 0.50);
}

TEST(ClassifyEntityRelationTest, ZeroSizeBoundsAreNotAnError) {
    // The contract rejects only negative-size bounds; zero-area input is valid
    // and, carrying no intersection, classifies as unrelated.
    expect_classification(RelationClassification::kNone, RectF{0.0F, 0.0F, 0.0F, 10.0F},
                          RectF{0.0F, 0.0F, 10.0F, 10.0F}, true, true);
    expect_classification(RelationClassification::kNone, RectF{0.0F, 0.0F, 0.0F, 0.0F}, RectF{0.0F, 0.0F, 0.0F, 0.0F},
                          true, true);
}

TEST(ClassifyEntityRelationTest, NonFiniteOrNegativeSizeBoundsAreRejected) {
    const RectF valid{0.0F, 0.0F, 10.0F, 10.0F};
    const std::vector<RectF> invalid{
        RectF{kNaN, 0.0F, 10.0F, 10.0F}, RectF{0.0F, kNaN, 10.0F, 10.0F}, RectF{0.0F, 0.0F, kNaN, 10.0F},
        RectF{0.0F, 0.0F, 10.0F, kNaN},  RectF{kInf, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, kInf, 10.0F},
        RectF{0.0F, 0.0F, -1.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, -1.0F},
    };
    for (const RectF& bounds : invalid) {
        expect_invalid_bounds(bounds, valid);
        expect_invalid_bounds(valid, bounds);
        expect_invalid_bounds(bounds, bounds);
    }
}

// --- entity_semantics_compatible: frozen predicate table --------------------

TEST(EntitySemanticsCompatibleTest, DecisionTable) {
    const EntitySemantics empty{};
    const EntitySemantics save_text{"Save", ""};
    const EntitySemantics button_label{"", "button"};
    const EntitySemantics save_button{"Save", "button"};
    const EntitySemantics quit_button{"Quit", "button"};
    const EntitySemantics save_icon{"Save", "icon"};
    const EntitySemantics quit_icon{"Quit", "icon"};
    const EntitySemantics lowercase_text{"save", ""};

    // Neutral when a claim is missing on either side.
    EXPECT_TRUE(entity_semantics_compatible(empty, empty));
    EXPECT_TRUE(entity_semantics_compatible(save_text, save_text));
    EXPECT_TRUE(entity_semantics_compatible(button_label, button_label));
    EXPECT_TRUE(entity_semantics_compatible(save_button, save_button));
    // Exact match of both text and label is always compatible.
    EXPECT_TRUE(entity_semantics_compatible(save_button, EntitySemantics{"Save", "button"}));
    // One-sided claims never conflict.
    EXPECT_TRUE(entity_semantics_compatible(save_text, button_label));
    EXPECT_TRUE(entity_semantics_compatible(save_button, empty));
    EXPECT_TRUE(entity_semantics_compatible(save_button, save_text));

    // Fully specified contradicting labels are incompatible.
    EXPECT_FALSE(entity_semantics_compatible(save_button, save_icon));
    EXPECT_FALSE(entity_semantics_compatible(button_label, EntitySemantics{"", "icon"}));
    // Fully specified contradicting texts are incompatible.
    EXPECT_FALSE(entity_semantics_compatible(save_text, EntitySemantics{"Quit", ""}));
    // Either axis alone decides against.
    EXPECT_FALSE(entity_semantics_compatible(save_button, quit_button));
    EXPECT_FALSE(entity_semantics_compatible(save_button, quit_icon));
    EXPECT_FALSE(entity_semantics_compatible(save_text, quit_button));
    // Exact comparison: case differences contradict.
    EXPECT_FALSE(entity_semantics_compatible(save_text, lowercase_text));
    // Symmetric predicate.
    EXPECT_FALSE(entity_semantics_compatible(save_icon, save_button));
    EXPECT_FALSE(entity_semantics_compatible(quit_button, save_text));
}

// --- relation_view_consistent: structural invariants ------------------------

TEST(RelationViewConsistentTest, EmptyViewIsConsistent) {
    const RelationView view;
    EXPECT_TRUE(relation_view_consistent(view));
}

TEST(RelationViewConsistentTest, DuplicateEdgeWithMergedFromIsConsistent) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}, {1, 2}));
    view.relations.push_back(make_relation(RelationKind::kDuplicateEvidence, 0, 0, {0, 1}));
    EXPECT_TRUE(relation_view_consistent(view));
}

TEST(RelationViewConsistentTest, OverlapAndConflictEdgesWithEmptyMergedFromAreConsistent) {
    RelationView overlap_view;
    overlap_view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    overlap_view.entities.push_back(make_entity(RectF{5.0F, 0.0F, 10.0F, 10.0F}));
    overlap_view.relations.push_back(make_relation(RelationKind::kOverlap, 0, 1));
    EXPECT_TRUE(relation_view_consistent(overlap_view));

    RelationView conflict_view;
    conflict_view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    conflict_view.entities.push_back(make_entity(RectF{2.0F, 3.0F, 10.0F, 10.0F}));
    conflict_view.relations.push_back(make_relation(RelationKind::kConflict, 1, 0));
    EXPECT_TRUE(relation_view_consistent(conflict_view));
}

TEST(RelationViewConsistentTest, PartOfEdgeWithEmptyMergedFromIsConsistent) {
    // Contract (header): only kDuplicateEvidence carries merged_from; a
    // part-of edge runs child (smaller area, first) -> parent (second).
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 5.0F, 5.0F}));    // child, area 25
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));  // parent, area 100
    view.relations.push_back(make_relation(RelationKind::kPartOf, 0, 1));
    EXPECT_TRUE(relation_view_consistent(view));
}

TEST(RelationViewConsistentTest, PartOfChildAreaEqualToParentIsConsistent) {
    // The frozen rule forbids only child area > parent area (acyclicity);
    // equal areas are allowed by "child <= parent".
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    view.entities.push_back(make_entity(RectF{5.0F, 5.0F, 10.0F, 10.0F}));
    view.relations.push_back(make_relation(RelationKind::kPartOf, 0, 1));
    EXPECT_TRUE(relation_view_consistent(view));
}

TEST(RelationViewConsistentTest, BuilderShapedViewWithPartOfIsConsistent) {
    RelationView view;
    view.space = CoordinateSpaceId::kDisplay;
    view.coverage = CoverageState::kComplete;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 5.0F, 5.0F}, {1, 2}));
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}, {1}));
    view.entities.push_back(make_entity(RectF{40.0F, 40.0F, 4.0F, 4.0F}, {3}));
    auto duplicate = make_relation(RelationKind::kDuplicateEvidence, 1, 1, {0, 1});
    duplicate.relation_id = 1;
    view.relations.push_back(duplicate);
    auto part_of = make_relation(RelationKind::kPartOf, 0, 1);
    part_of.relation_id = 2;
    view.relations.push_back(part_of);
    auto overlap = make_relation(RelationKind::kOverlap, 2, 1);
    overlap.relation_id = 3;
    view.relations.push_back(overlap);
    EXPECT_TRUE(relation_view_consistent(view));
}

TEST(RelationViewConsistentTest, RelationIdsMustBeSequentialFromOne) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    view.entities.push_back(make_entity(RectF{5.0F, 0.0F, 10.0F, 10.0F}));
    auto not_from_one = make_relation(RelationKind::kOverlap, 0, 1);
    not_from_one.relation_id = 2;
    view.relations.push_back(not_from_one);
    EXPECT_FALSE(relation_view_consistent(view));

    RelationView gapped;
    gapped.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    gapped.entities.push_back(make_entity(RectF{5.0F, 0.0F, 10.0F, 10.0F}));
    auto first = make_relation(RelationKind::kOverlap, 0, 1);
    auto third = make_relation(RelationKind::kOverlap, 1, 0);
    third.relation_id = 3;
    gapped.relations.push_back(first);
    gapped.relations.push_back(third);
    EXPECT_FALSE(relation_view_consistent(gapped));
}

TEST(RelationViewConsistentTest, EntityIndexesMustBeInRange) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    view.entities.push_back(make_entity(RectF{5.0F, 0.0F, 10.0F, 10.0F}));

    const RelationRecord out_of_range_first = make_relation(RelationKind::kOverlap, 2, 1);
    view.relations.push_back(out_of_range_first);
    EXPECT_FALSE(relation_view_consistent(view));

    view.relations.clear();
    const RelationRecord out_of_range_second = make_relation(RelationKind::kOverlap, 0, 2);
    view.relations.push_back(out_of_range_second);
    EXPECT_FALSE(relation_view_consistent(view));

    RelationView no_entities;
    no_entities.relations.push_back(make_relation(RelationKind::kOverlap, 0, 0));
    EXPECT_FALSE(relation_view_consistent(no_entities));
}

TEST(RelationViewConsistentTest, OnlyDuplicateEvidenceMayCarryMergedFrom) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 5.0F, 5.0F}));
    view.entities.push_back(make_entity(RectF{5.0F, 0.0F, 10.0F, 10.0F}));

    RelationView duplicate_without_trace = view;
    duplicate_without_trace.relations.push_back(make_relation(RelationKind::kDuplicateEvidence, 0, 0));
    EXPECT_FALSE(relation_view_consistent(duplicate_without_trace));

    RelationView part_of_with_trace = view;
    part_of_with_trace.relations.push_back(make_relation(RelationKind::kPartOf, 1, 0, {0}));
    EXPECT_FALSE(relation_view_consistent(part_of_with_trace));

    RelationView overlap_with_trace = view;
    overlap_with_trace.relations.push_back(make_relation(RelationKind::kOverlap, 0, 2, {0}));
    EXPECT_FALSE(relation_view_consistent(overlap_with_trace));

    RelationView conflict_with_trace = view;
    conflict_with_trace.relations.push_back(make_relation(RelationKind::kConflict, 2, 0, {1}));
    EXPECT_FALSE(relation_view_consistent(conflict_with_trace));
}

TEST(RelationViewConsistentTest, MergedFromMustBeAscending) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}, {1, 2}));
    const RelationRecord descending = make_relation(RelationKind::kDuplicateEvidence, 0, 0, {1, 0});
    view.relations.push_back(descending);
    EXPECT_FALSE(relation_view_consistent(view));
}

TEST(RelationViewConsistentTest, PartOfEdgesMustNotBeSelfEdgesOrInverted) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 10.0F, 10.0F}));  // area 100
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 5.0F, 5.0F}));    // area 25
    view.entities.push_back(make_entity(RectF{5.0F, 5.0F, 10.0F, 10.0F}));  // area 100

    RelationView self_edge = view;
    self_edge.relations.push_back(make_relation(RelationKind::kPartOf, 0, 0));
    EXPECT_FALSE(relation_view_consistent(self_edge));

    RelationView inverted = view;
    inverted.relations.push_back(make_relation(RelationKind::kPartOf, 0, 1));  // larger child first
    EXPECT_FALSE(relation_view_consistent(inverted));

    // The same masking applies to the overlap/conflict self-edge rule.
    RelationView overlap_self = view;
    overlap_self.relations.push_back(make_relation(RelationKind::kOverlap, 1, 1));
    EXPECT_FALSE(relation_view_consistent(overlap_self));

    RelationView conflict_self = view;
    conflict_self.relations.push_back(make_relation(RelationKind::kConflict, 2, 2));
    EXPECT_FALSE(relation_view_consistent(conflict_self));
}

// --- relation_view_owned_bytes ----------------------------------------------

TEST(RelationViewOwnedBytesTest, EmptyViewCountsStructOnly) {
    const RelationView view;
    EXPECT_EQ(relation_view_owned_bytes(view), static_cast<int64_t>(sizeof(RelationView)));
}

TEST(RelationViewOwnedBytesTest, CountsEntityAndRelationBufferCapacities) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 5.0F, 5.0F}, {1, 2, 3}));           // capacity 3
    view.entities.push_back(make_entity(RectF{10.0F, 10.0F, 5.0F, 5.0F}));                    // capacity 0
    view.relations.push_back(make_relation(RelationKind::kDuplicateEvidence, 0, 0, {0, 1}));  // capacity 2
    const int64_t expected = static_cast<int64_t>(sizeof(RelationView)) +
                             2 * static_cast<int64_t>(sizeof(EntityRecord)) +
                             3 * static_cast<int64_t>(sizeof(uint64_t)) + static_cast<int64_t>(sizeof(RelationRecord)) +
                             2 * static_cast<int64_t>(sizeof(size_t));
    EXPECT_EQ(relation_view_owned_bytes(view), expected);
}

TEST(RelationViewOwnedBytesTest, CountsReservedCapacityNotSize) {
    RelationView view;
    EntityRecord entity = make_entity(RectF{0.0F, 0.0F, 5.0F, 5.0F});
    entity.evidence_ids.reserve(8);
    entity.evidence_ids.push_back(1);
    view.entities.push_back(std::move(entity));
    RelationRecord relation = make_relation(RelationKind::kDuplicateEvidence, 0, 0);
    relation.merged_from.reserve(4);
    relation.merged_from.push_back(0);
    view.relations.push_back(std::move(relation));
    const int64_t expected = static_cast<int64_t>(sizeof(RelationView)) + static_cast<int64_t>(sizeof(EntityRecord)) +
                             8 * static_cast<int64_t>(sizeof(uint64_t)) + static_cast<int64_t>(sizeof(RelationRecord)) +
                             4 * static_cast<int64_t>(sizeof(size_t));
    EXPECT_EQ(relation_view_owned_bytes(view), expected);
}

TEST(RelationViewOwnedBytesTest, ByteCountIsDeterministic) {
    RelationView view;
    view.entities.push_back(make_entity(RectF{0.0F, 0.0F, 5.0F, 5.0F}, {1}));
    view.relations.push_back(make_relation(RelationKind::kDuplicateEvidence, 0, 0, {0}));
    EXPECT_EQ(relation_view_owned_bytes(view), relation_view_owned_bytes(view));
}

// --- validate_attention_policy ----------------------------------------------

TEST(ValidateAttentionPolicyTest, DefaultPolicyAndEverySingleCriterionAreValid) {
    EXPECT_TRUE(validate_attention_policy(AttentionPolicy{}).ok());

    AttentionPolicy roi_only;
    roi_only.conditions.push_back(roi_condition());
    EXPECT_TRUE(validate_attention_policy(roi_only).ok());

    AttentionPolicy id_only;
    id_only.conditions.push_back(id_condition(9, 3));
    EXPECT_TRUE(validate_attention_policy(id_only).ok());

    AttentionPolicy text_only;
    text_only.conditions.push_back(text_condition("Save"));
    EXPECT_TRUE(validate_attention_policy(text_only).ok());

    AttentionPolicy label_only;
    label_only.conditions.push_back(label_condition("button"));
    EXPECT_TRUE(validate_attention_policy(label_only).ok());

    AttentionPolicy combined;
    AttentionCondition everything = roi_condition();
    everything.bind_stable_id = true;
    everything.stable_id = 9;
    everything.generation = 3;
    everything.text_equals = "Save";
    everything.label_equals = "button";
    combined.conditions.push_back(everything);
    EXPECT_TRUE(validate_attention_policy(combined).ok());
}

TEST(ValidateAttentionPolicyTest, FocusConditionCapIsSixtyFourInclusive) {
    AttentionPolicy full;
    full.conditions.assign(64, roi_condition());
    EXPECT_TRUE(validate_attention_policy(full).ok());

    AttentionPolicy over;
    over.conditions.assign(65, roi_condition());
    expect_budget_exceeded(validate_attention_policy(over));
}

TEST(ValidateAttentionPolicyTest, MaxConditionBytesMustBePositive) {
    AttentionPolicy zero;
    zero.max_condition_bytes = 0;
    expect_invalid_argument(validate_attention_policy(zero));

    AttentionPolicy negative;
    negative.max_condition_bytes = -1;
    expect_invalid_argument(validate_attention_policy(negative));

    // The byte-cap checks run even with no conditions to match.
    AttentionPolicy empty_zero;
    empty_zero.max_condition_bytes = 0;
    expect_invalid_argument(validate_attention_policy(empty_zero));
}

TEST(ValidateAttentionPolicyTest, MaxConditionBytesIsCappedAtTheReferenceConstant) {
    AttentionPolicy at_cap;
    at_cap.max_condition_bytes = kContinuousMaxCoordinationBytes;
    EXPECT_TRUE(validate_attention_policy(at_cap).ok());

    AttentionPolicy over;
    over.max_condition_bytes = kContinuousMaxCoordinationBytes + 1;
    expect_budget_exceeded(validate_attention_policy(over));
}

TEST(ValidateAttentionPolicyTest, ConditionWithoutCriterionIsRejected) {
    AttentionPolicy policy;
    policy.conditions.push_back(AttentionCondition{});
    expect_invalid_argument(validate_attention_policy(policy));
}

TEST(ValidateAttentionPolicyTest, ZeroAreaRoiAloneCarriesNoCriterion) {
    AttentionPolicy zero_width;
    AttentionCondition widthless = AttentionCondition{};
    widthless.roi = RectF{5.0F, 5.0F, 0.0F, 10.0F};
    zero_width.conditions.push_back(widthless);
    expect_invalid_argument(validate_attention_policy(zero_width));

    AttentionPolicy zero_height;
    AttentionCondition heightless = AttentionCondition{};
    heightless.roi = RectF{5.0F, 5.0F, 10.0F, 0.0F};
    zero_height.conditions.push_back(heightless);
    expect_invalid_argument(validate_attention_policy(zero_height));
}

TEST(ValidateAttentionPolicyTest, NonFiniteOrNegativeRoiIsRejectedEvenWithOtherCriteria) {
    const std::vector<RectF> invalid{
        RectF{kNaN, 0.0F, 10.0F, 10.0F},
        RectF{0.0F, 0.0F, kNaN, 10.0F},
        RectF{0.0F, 0.0F, 10.0F, -1.0F},
        RectF{0.0F, 0.0F, -1.0F, 10.0F},
    };
    for (const RectF& bounds : invalid) {
        AttentionPolicy roi_carrier;
        AttentionCondition condition = AttentionCondition{};
        condition.roi = bounds;
        roi_carrier.conditions.push_back(condition);
        expect_invalid_argument(validate_attention_policy(roi_carrier));

        AttentionPolicy text_carrier;
        AttentionCondition text_with_bad_roi = text_condition("Save");
        text_with_bad_roi.roi = bounds;
        text_carrier.conditions.push_back(text_with_bad_roi);
        expect_invalid_argument(validate_attention_policy(text_carrier));
    }
}

TEST(ValidateAttentionPolicyTest, StableIdBindingRequiresNonZeroId) {
    AttentionPolicy policy;
    policy.conditions.push_back(id_condition(0, 3));
    expect_invalid_argument(validate_attention_policy(policy));

    AttentionPolicy with_text;
    AttentionCondition condition = text_condition("Save");
    condition.bind_stable_id = true;
    condition.stable_id = 0;
    with_text.conditions.push_back(condition);
    expect_invalid_argument(validate_attention_policy(with_text));
}

TEST(ValidateAttentionPolicyTest, MatcherBytesAreCappedAtMaxConditionBytes) {
    AttentionCondition condition = text_condition("abcd");  // 4 bytes
    condition.label_equals = "ef";                          // +2 bytes

    AttentionPolicy exact;
    exact.conditions.push_back(condition);
    exact.max_condition_bytes = 6;
    EXPECT_TRUE(validate_attention_policy(exact).ok());

    AttentionPolicy tight;
    tight.conditions.push_back(condition);
    tight.max_condition_bytes = 5;
    expect_budget_exceeded(validate_attention_policy(tight));

    AttentionPolicy split;
    split.conditions.push_back(text_condition("abc"));
    split.conditions.push_back(text_condition("abc"));
    split.max_condition_bytes = 6;
    EXPECT_TRUE(validate_attention_policy(split).ok());
    split.max_condition_bytes = 5;
    expect_budget_exceeded(validate_attention_policy(split));
}

// --- attention_policy_digest -------------------------------------------------

AttentionPolicy base_policy() {
    AttentionPolicy policy;
    policy.max_condition_bytes = 100;
    AttentionCondition condition;
    condition.roi = RectF{1.0F, 2.0F, 3.0F, 4.0F};
    condition.roi_space = CoordinateSpaceId::kCropped;
    condition.bind_stable_id = true;
    condition.stable_id = 9;
    condition.generation = 3;
    condition.text_equals = "Save";
    condition.label_equals = "button";
    policy.conditions.push_back(condition);
    return policy;
}

TEST(AttentionPolicyDigestTest, EqualInputsProduceEqualDigestsAcrossCallsAndObjects) {
    EXPECT_EQ(attention_policy_digest(base_policy()), attention_policy_digest(base_policy()));
    const AttentionPolicy policy = base_policy();
    EXPECT_EQ(attention_policy_digest(policy), attention_policy_digest(policy));
}

TEST(AttentionPolicyDigestTest, DigestIsSensitiveToEveryDecisionRelevantField) {
    const uint64_t base = attention_policy_digest(base_policy());

    AttentionPolicy bytes = base_policy();
    bytes.max_condition_bytes = 101;
    EXPECT_NE(attention_policy_digest(bytes), base);

    AttentionPolicy x = base_policy();
    x.conditions[0].roi.x += 1.0F;
    EXPECT_NE(attention_policy_digest(x), base);

    AttentionPolicy y = base_policy();
    y.conditions[0].roi.y += 1.0F;
    EXPECT_NE(attention_policy_digest(y), base);

    AttentionPolicy width = base_policy();
    width.conditions[0].roi.width += 1.0F;
    EXPECT_NE(attention_policy_digest(width), base);

    AttentionPolicy height = base_policy();
    height.conditions[0].roi.height += 1.0F;
    EXPECT_NE(attention_policy_digest(height), base);

    AttentionPolicy space = base_policy();
    space.conditions[0].roi_space = CoordinateSpaceId::kFrame;
    EXPECT_NE(attention_policy_digest(space), base);

    AttentionPolicy binding = base_policy();
    binding.conditions[0].bind_stable_id = false;
    EXPECT_NE(attention_policy_digest(binding), base);

    AttentionPolicy stable_id = base_policy();
    stable_id.conditions[0].stable_id = 10;
    EXPECT_NE(attention_policy_digest(stable_id), base);

    AttentionPolicy generation = base_policy();
    generation.conditions[0].generation = 4;
    EXPECT_NE(attention_policy_digest(generation), base);

    AttentionPolicy text = base_policy();
    text.conditions[0].text_equals = "Save!";
    EXPECT_NE(attention_policy_digest(text), base);

    AttentionPolicy label = base_policy();
    label.conditions[0].label_equals = "icon";
    EXPECT_NE(attention_policy_digest(label), base);

    AttentionPolicy extra_condition = base_policy();
    extra_condition.conditions.push_back(roi_condition());
    EXPECT_NE(attention_policy_digest(extra_condition), base);

    const AttentionPolicy empty{};
    EXPECT_NE(attention_policy_digest(empty), base);
}

// --- resolve_attention_focus -------------------------------------------------

TEST(ResolveAttentionFocusTest, StableIdResolvesToTheSnapshotPosition) {
    const SemanticSnapshot snapshot = make_focused_snapshot();
    const auto result = resolve_attention_focus(id_condition(200, 7), snapshot);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value().resolved);
    // reason is unspecified when resolved (contract: valid iff !resolved).
    EXPECT_EQ(result.value().focus.stable_id, 200U);
    EXPECT_EQ(result.value().focus.generation, 7U);
    EXPECT_EQ(result.value().focus.roi, (RectF{20.0F, 20.0F, 5.0F, 5.0F}));
    EXPECT_EQ(result.value().focus.space, CoordinateSpaceId::kDisplay);
}

TEST(ResolveAttentionFocusTest, StaleGenerationIsReportedBeforeTheIdLookup) {
    const SemanticSnapshot snapshot = make_focused_snapshot();

    const auto stale_only = resolve_attention_focus(id_condition(100, 8), snapshot);
    ASSERT_TRUE(stale_only.ok());
    EXPECT_FALSE(stale_only.value().resolved);
    EXPECT_EQ(stale_only.value().reason, FocusUnresolvedReason::kStaleGeneration);

    // Both wrong: the frozen order reports the stale generation, not the
    // unknown id.
    const auto stale_and_unknown = resolve_attention_focus(id_condition(999, 8), snapshot);
    ASSERT_TRUE(stale_and_unknown.ok());
    EXPECT_FALSE(stale_and_unknown.value().resolved);
    EXPECT_EQ(stale_and_unknown.value().reason, FocusUnresolvedReason::kStaleGeneration);
}

TEST(ResolveAttentionFocusTest, UnknownStableIdIsReportedWhenGenerationMatches) {
    const SemanticSnapshot snapshot = make_focused_snapshot();
    const auto result = resolve_attention_focus(id_condition(999, 7), snapshot);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().resolved);
    EXPECT_EQ(result.value().reason, FocusUnresolvedReason::kUnknownStableId);
}

TEST(ResolveAttentionFocusTest, BoundStableIdNeverFallsThroughToTextOrRoi) {
    const SemanticSnapshot snapshot = make_focused_snapshot();

    // The text criterion would match, but the bound id decides alone.
    AttentionCondition bound_with_text = id_condition(999, 7);
    bound_with_text.text_equals = "Save";
    const auto result = resolve_attention_focus(bound_with_text, snapshot);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().resolved);
    EXPECT_EQ(result.value().reason, FocusUnresolvedReason::kUnknownStableId);

    // A set ROI is ignored for a bound id: the position comes from the
    // snapshot, not from a memorized coordinate.
    AttentionCondition bound_with_roi = id_condition(100, 7);
    bound_with_roi.roi = RectF{50.0F, 50.0F, 1.0F, 1.0F};
    const auto roi_ignored = resolve_attention_focus(bound_with_roi, snapshot);
    ASSERT_TRUE(roi_ignored.ok());
    ASSERT_TRUE(roi_ignored.value().resolved);
    EXPECT_EQ(roi_ignored.value().focus.roi, (RectF{1.0F, 1.0F, 10.0F, 10.0F}));
}

TEST(ResolveAttentionFocusTest, TextResolvesTheFirstStoredMatch) {
    const SemanticSnapshot snapshot = make_focused_snapshot();
    const auto result = resolve_attention_focus(text_condition("Save"), snapshot);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value().resolved);
    EXPECT_EQ(result.value().focus.stable_id, 100U);
    EXPECT_EQ(result.value().focus.roi, (RectF{1.0F, 1.0F, 10.0F, 10.0F}));
    EXPECT_EQ(result.value().focus.space, CoordinateSpaceId::kDisplay);
}

TEST(ResolveAttentionFocusTest, TextAndLabelFormAConjunction) {
    const SemanticSnapshot snapshot = make_focused_snapshot();

    const auto both_match = resolve_attention_focus(text_condition("Save"), snapshot);
    ASSERT_TRUE(both_match.ok());

    AttentionCondition text_and_label = text_condition("Save");
    text_and_label.label_equals = "icon";
    const auto conjunction = resolve_attention_focus(text_and_label, snapshot);
    ASSERT_TRUE(conjunction.ok());
    ASSERT_TRUE(conjunction.value().resolved);
    EXPECT_EQ(conjunction.value().focus.stable_id, 200U);

    AttentionCondition contradicting = text_condition("Save");
    contradicting.label_equals = "menu";
    const auto no_match = resolve_attention_focus(contradicting, snapshot);
    ASSERT_TRUE(no_match.ok());
    EXPECT_FALSE(no_match.value().resolved);
    EXPECT_EQ(no_match.value().reason, FocusUnresolvedReason::kNotObserved);
}

TEST(ResolveAttentionFocusTest, LabelOnlyResolvesTheFirstStoredMatch) {
    const SemanticSnapshot snapshot = make_focused_snapshot();
    const auto result = resolve_attention_focus(label_condition("button"), snapshot);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value().resolved);
    EXPECT_EQ(result.value().focus.stable_id, 100U);
}

TEST(ResolveAttentionFocusTest, UnmatchedTextReportsNotObserved) {
    const SemanticSnapshot snapshot = make_focused_snapshot();
    const auto result = resolve_attention_focus(text_condition("Close"), snapshot);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().resolved);
    EXPECT_EQ(result.value().reason, FocusUnresolvedReason::kNotObserved);

    const auto empty_snapshot = resolve_attention_focus(text_condition("Save"), SemanticSnapshot{});
    ASSERT_TRUE(empty_snapshot.ok());
    EXPECT_FALSE(empty_snapshot.value().resolved);
    EXPECT_EQ(empty_snapshot.value().reason, FocusUnresolvedReason::kNotObserved);
}

TEST(ResolveAttentionFocusTest, RoiOnlyResolvesAsDeclaredWithoutConversion) {
    const SemanticSnapshot snapshot = make_focused_snapshot();  // space kDisplay
    const auto result = resolve_attention_focus(roi_condition(), snapshot);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value().resolved);
    EXPECT_EQ(result.value().focus.roi, (RectF{5.0F, 5.0F, 20.0F, 20.0F}));
    EXPECT_EQ(result.value().focus.space, CoordinateSpaceId::kFrame);
}

TEST(ResolveAttentionFocusTest, MalformedConditionsAreRejected) {
    const SemanticSnapshot snapshot = make_focused_snapshot();

    const auto no_criterion = resolve_attention_focus(AttentionCondition{}, snapshot);
    EXPECT_FALSE(no_criterion.ok());
    EXPECT_EQ(no_criterion.status().code(), ErrorCode::kInvalidArgument);

    AttentionCondition zero_area = AttentionCondition{};
    zero_area.roi = RectF{5.0F, 5.0F, 0.0F, 10.0F};
    const auto zero_area_result = resolve_attention_focus(zero_area, snapshot);
    EXPECT_FALSE(zero_area_result.ok());
    EXPECT_EQ(zero_area_result.status().code(), ErrorCode::kInvalidArgument);

    AttentionCondition nan_roi = text_condition("Save");
    nan_roi.roi = RectF{kNaN, 0.0F, 10.0F, 10.0F};
    const auto nan_result = resolve_attention_focus(nan_roi, snapshot);
    EXPECT_FALSE(nan_result.ok());
    EXPECT_EQ(nan_result.status().code(), ErrorCode::kInvalidArgument);

    AttentionCondition negative_roi = text_condition("Save");
    negative_roi.roi = RectF{0.0F, 0.0F, -5.0F, 10.0F};
    const auto negative_result = resolve_attention_focus(negative_roi, snapshot);
    EXPECT_FALSE(negative_result.ok());
    EXPECT_EQ(negative_result.status().code(), ErrorCode::kInvalidArgument);

    const auto zero_id = resolve_attention_focus(id_condition(0, 7), snapshot);
    EXPECT_FALSE(zero_id.ok());
    EXPECT_EQ(zero_id.status().code(), ErrorCode::kInvalidArgument);
}

// --- validate_work_token -----------------------------------------------------

TEST(ValidateWorkTokenTest, FullyBoundTokenIsValid) {
    EXPECT_TRUE(validate_work_token(make_token()).ok());
}

TEST(ValidateWorkTokenTest, EmptySourceIdIsRejected) {
    WorkToken token = make_token();
    token.source_id.clear();
    expect_invalid_argument(validate_work_token(token));
}

TEST(ValidateWorkTokenTest, NonFiniteOrNegativeRoiIsRejected) {
    const std::vector<RectF> invalid{
        RectF{kNaN, 0.0F, 10.0F, 10.0F},
        RectF{0.0F, 0.0F, kInf, 10.0F},
        RectF{0.0F, 0.0F, -1.0F, 10.0F},
        RectF{0.0F, 0.0F, 10.0F, -1.0F},
    };
    for (const RectF& bounds : invalid) {
        WorkToken token = make_token();
        token.roi = bounds;
        expect_invalid_argument(validate_work_token(token));
    }
}

TEST(ValidateWorkTokenTest, ZeroSizeRoiIsFiniteAndAccepted) {
    WorkToken token = make_token();
    token.roi = RectF{};
    EXPECT_TRUE(validate_work_token(token).ok());
}

// --- work_token_stale_reason: frozen check order ------------------------------

TEST(WorkTokenStaleReasonTest, FullyAlignedTokenIsNotStale) {
    EXPECT_EQ(work_token_stale_reason(make_token(), make_context()), std::nullopt);
}

TEST(WorkTokenStaleReasonTest, EachFieldMismatchReportsItsOwnReason) {
    WorkToken foreign = make_token();
    foreign.source_id = "other-cam";
    EXPECT_EQ(work_token_stale_reason(foreign, make_context()), WorkRejectionReason::kForeignSource);

    WorkToken stale_epoch = make_token();
    stale_epoch.source_epoch = 1;
    EXPECT_EQ(work_token_stale_reason(stale_epoch, make_context()), WorkRejectionReason::kStaleSourceEpoch);

    WorkToken stale_revision = make_token();
    stale_revision.policy_revision = 2;
    EXPECT_EQ(work_token_stale_reason(stale_revision, make_context()), WorkRejectionReason::kStalePolicyRevision);

    WorkToken stale_generation = make_token();
    stale_generation.generation = 4;
    EXPECT_EQ(work_token_stale_reason(stale_generation, make_context()), WorkRejectionReason::kStaleGeneration);

    WorkToken stale_frame = make_token();
    stale_frame.frame_sequence = 9;
    EXPECT_EQ(work_token_stale_reason(stale_frame, make_context()), WorkRejectionReason::kStaleFrameSequence);
}

TEST(WorkTokenStaleReasonTest, UnseenFutureFramesAreStaleToo) {
    WorkToken token = make_token();
    token.frame_sequence = 11;
    EXPECT_EQ(work_token_stale_reason(token, make_context()), WorkRejectionReason::kStaleFrameSequence);
}

TEST(WorkTokenStaleReasonTest, MultipleMismatchesReportTheEarliestFrozenCheck) {
    WorkToken all = make_token();
    all.source_id = "other-cam";
    all.source_epoch = 1;
    all.policy_revision = 2;
    all.generation = 4;
    all.frame_sequence = 9;
    EXPECT_EQ(work_token_stale_reason(all, make_context()), WorkRejectionReason::kForeignSource);

    WorkToken from_epoch = make_token();
    from_epoch.source_epoch = 1;
    from_epoch.policy_revision = 2;
    from_epoch.generation = 4;
    from_epoch.frame_sequence = 9;
    EXPECT_EQ(work_token_stale_reason(from_epoch, make_context()), WorkRejectionReason::kStaleSourceEpoch);

    WorkToken from_revision = make_token();
    from_revision.policy_revision = 2;
    from_revision.generation = 4;
    from_revision.frame_sequence = 9;
    EXPECT_EQ(work_token_stale_reason(from_revision, make_context()), WorkRejectionReason::kStalePolicyRevision);

    WorkToken from_generation = make_token();
    from_generation.generation = 4;
    from_generation.frame_sequence = 9;
    EXPECT_EQ(work_token_stale_reason(from_generation, make_context()), WorkRejectionReason::kStaleGeneration);
}

// --- semantic_event_consistent ------------------------------------------------

TEST(SemanticEventConsistentTest, FrozenSemanticChangeKindPairingTable) {
    const std::vector<std::pair<EventSemantic, SemanticChangeKind>> all_pairs{
        {EventSemantic::kBegin, SemanticChangeKind::kAppeared},
        {EventSemantic::kBegin, SemanticChangeKind::kTextChanged},
        {EventSemantic::kBegin, SemanticChangeKind::kAppearanceChanged},
        {EventSemantic::kBegin, SemanticChangeKind::kRelationChanged},
        {EventSemantic::kBegin, SemanticChangeKind::kDisappeared},
        {EventSemantic::kBegin, SemanticChangeKind::kSourceReset},
        {EventSemantic::kUpdate, SemanticChangeKind::kAppeared},
        {EventSemantic::kUpdate, SemanticChangeKind::kTextChanged},
        {EventSemantic::kUpdate, SemanticChangeKind::kAppearanceChanged},
        {EventSemantic::kUpdate, SemanticChangeKind::kRelationChanged},
        {EventSemantic::kUpdate, SemanticChangeKind::kDisappeared},
        {EventSemantic::kUpdate, SemanticChangeKind::kSourceReset},
        {EventSemantic::kEnd, SemanticChangeKind::kAppeared},
        {EventSemantic::kEnd, SemanticChangeKind::kTextChanged},
        {EventSemantic::kEnd, SemanticChangeKind::kAppearanceChanged},
        {EventSemantic::kEnd, SemanticChangeKind::kRelationChanged},
        {EventSemantic::kEnd, SemanticChangeKind::kDisappeared},
        {EventSemantic::kEnd, SemanticChangeKind::kSourceReset},
    };
    for (const auto& pair : all_pairs) {
        const bool pairing_ok =
            (pair.first == EventSemantic::kBegin && pair.second == SemanticChangeKind::kAppeared) ||
            (pair.first == EventSemantic::kUpdate && (pair.second == SemanticChangeKind::kTextChanged ||
                                                      pair.second == SemanticChangeKind::kAppearanceChanged ||
                                                      pair.second == SemanticChangeKind::kRelationChanged)) ||
            (pair.first == EventSemantic::kEnd &&
             (pair.second == SemanticChangeKind::kDisappeared || pair.second == SemanticChangeKind::kSourceReset));
        // Reset ends carry zero bounds; every other pairing carries geometry.
        const bool zero_bounds = pair.second == SemanticChangeKind::kSourceReset;
        SemanticEvent event = make_event(pair.first, pair.second);
        if (zero_bounds) {
            event.bounds = RectF{};
        }
        EXPECT_EQ(semantic_event_consistent(event), pairing_ok) << "for the pairing under test";
    }
}

TEST(SemanticEventConsistentTest, SequenceIdentityAndWindowGuards) {
    EXPECT_TRUE(semantic_event_consistent(make_event(EventSemantic::kBegin, SemanticChangeKind::kAppeared)));

    SemanticEvent zero_sequence = make_event(EventSemantic::kBegin, SemanticChangeKind::kAppeared);
    zero_sequence.event_sequence = 0;
    EXPECT_FALSE(semantic_event_consistent(zero_sequence));

    SemanticEvent zero_id = make_event(EventSemantic::kBegin, SemanticChangeKind::kAppeared);
    zero_id.stable_id = 0;
    EXPECT_FALSE(semantic_event_consistent(zero_id));

    SemanticEvent no_source = make_event(EventSemantic::kUpdate, SemanticChangeKind::kTextChanged);
    no_source.window.source_id.clear();
    EXPECT_FALSE(semantic_event_consistent(no_source));

    SemanticEvent frame_order = make_event(EventSemantic::kUpdate, SemanticChangeKind::kTextChanged);
    frame_order.window.observed_frame = 5;
    frame_order.window.confirmed_frame = 4;
    EXPECT_FALSE(semantic_event_consistent(frame_order));

    SemanticEvent same_frame = make_event(EventSemantic::kUpdate, SemanticChangeKind::kTextChanged);
    same_frame.window.observed_frame = 4;
    same_frame.window.confirmed_frame = 4;
    EXPECT_TRUE(semantic_event_consistent(same_frame));
}

TEST(SemanticEventConsistentTest, SourceResetEndsCarryExactlyZeroBounds) {
    SemanticEvent reset = make_event(EventSemantic::kEnd, SemanticChangeKind::kSourceReset);
    reset.bounds = RectF{};
    EXPECT_TRUE(semantic_event_consistent(reset));

    const std::vector<RectF> non_zero{
        RectF{1.0F, 0.0F, 0.0F, 0.0F},
        RectF{0.0F, 1.0F, 0.0F, 0.0F},
        RectF{0.0F, 0.0F, 1.0F, 0.0F},
        RectF{0.0F, 0.0F, 0.0F, 1.0F},
    };
    for (const RectF& bounds : non_zero) {
        SemanticEvent event = make_event(EventSemantic::kEnd, SemanticChangeKind::kSourceReset);
        event.bounds = bounds;
        EXPECT_FALSE(semantic_event_consistent(event));
    }
}

TEST(SemanticEventConsistentTest, OtherEventsRequirePositiveFiniteBounds) {
    SemanticEvent empty_bounds = make_event(EventSemantic::kEnd, SemanticChangeKind::kDisappeared);
    empty_bounds.bounds = RectF{};
    EXPECT_FALSE(semantic_event_consistent(empty_bounds));

    SemanticEvent zero_width = make_event(EventSemantic::kBegin, SemanticChangeKind::kAppeared);
    zero_width.bounds = RectF{0.0F, 0.0F, 0.0F, 10.0F};
    EXPECT_FALSE(semantic_event_consistent(zero_width));

    SemanticEvent nan_height = make_event(EventSemantic::kBegin, SemanticChangeKind::kAppeared);
    nan_height.bounds = RectF{0.0F, 0.0F, 10.0F, kNaN};
    EXPECT_FALSE(semantic_event_consistent(nan_height));

    SemanticEvent negative = make_event(EventSemantic::kUpdate, SemanticChangeKind::kAppearanceChanged);
    negative.bounds = RectF{0.0F, 0.0F, 10.0F, -5.0F};
    EXPECT_FALSE(semantic_event_consistent(negative));
}

// --- semantic_event_owned_bytes ----------------------------------------------

TEST(SemanticEventOwnedBytesTest, CountsWindowSourceCapacityOnly) {
    SemanticEvent event = make_event(EventSemantic::kBegin, SemanticChangeKind::kAppeared);
    const int64_t short_bytes = semantic_event_owned_bytes(event);
    EXPECT_EQ(short_bytes,
              static_cast<int64_t>(sizeof(SemanticEvent)) + static_cast<int64_t>(event.window.source_id.capacity()));

    event.window.source_id = "camera-with-a-long-identity-string";
    const int64_t long_bytes = semantic_event_owned_bytes(event);
    EXPECT_EQ(long_bytes,
              static_cast<int64_t>(sizeof(SemanticEvent)) + static_cast<int64_t>(event.window.source_id.capacity()));
    EXPECT_GT(long_bytes, short_bytes);
    EXPECT_EQ(semantic_event_owned_bytes(event), long_bytes);
}

// --- ContinuousPerception coordinator identity core ---------------------------

static_assert(!std::is_copy_constructible_v<ContinuousPerception>);
static_assert(!std::is_copy_assignable_v<ContinuousPerception>);
static_assert(std::is_move_constructible_v<ContinuousPerception>);
static_assert(std::is_move_assignable_v<ContinuousPerception>);
static_assert(std::is_nothrow_move_constructible_v<ContinuousPerception>);
static_assert(std::is_default_constructible_v<ContinuousPerception>);

TEST(ContinuousPerceptionCoordinatorTest, CreateWithDefaultsStartsAtEpochOneWithZeroOwnedBytes) {
    const auto created = ContinuousPerception::create(ContinuousPerceptionOptions{});
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(created.value().source_epoch(), 1U);
    EXPECT_EQ(created.value().coordination_owned_bytes(), 0);
    EXPECT_TRUE(created.value().source_id().empty());
    EXPECT_EQ(created.value().options().max_coordination_bytes, kContinuousMaxCoordinationBytes);
    EXPECT_EQ(created.value().options().max_entities, kContinuousMaxEntities);
    EXPECT_EQ(created.value().options().max_relations, kContinuousMaxRelations);
    EXPECT_EQ(created.value().options().max_events, kContinuousMaxEvents);
    EXPECT_EQ(created.value().options().max_focus_conditions, kContinuousMaxFocusConditions);
}

TEST(ContinuousPerceptionCoordinatorTest, CreateRejectsNonPositiveBudgets) {
    for (const int64_t bad : {int64_t{0}, int64_t{-1}, int64_t{-1024}}) {
        ContinuousPerceptionOptions bytes = make_coordinator_options("cam");
        bytes.max_coordination_bytes = bad;
        expect_create_error(bytes, ErrorCode::kInvalidArgument);

        ContinuousPerceptionOptions entities = make_coordinator_options("cam");
        entities.max_entities = static_cast<int32_t>(bad);
        expect_create_error(entities, ErrorCode::kInvalidArgument);

        ContinuousPerceptionOptions relations = make_coordinator_options("cam");
        relations.max_relations = static_cast<int32_t>(bad);
        expect_create_error(relations, ErrorCode::kInvalidArgument);

        ContinuousPerceptionOptions events = make_coordinator_options("cam");
        events.max_events = static_cast<int32_t>(bad);
        expect_create_error(events, ErrorCode::kInvalidArgument);

        ContinuousPerceptionOptions conditions = make_coordinator_options("cam");
        conditions.max_focus_conditions = static_cast<int32_t>(bad);
        expect_create_error(conditions, ErrorCode::kInvalidArgument);
    }
}

TEST(ContinuousPerceptionCoordinatorTest, CreateRejectsBudgetsLooserThanTheReferenceConstants) {
    ContinuousPerceptionOptions bytes = make_coordinator_options("cam");
    bytes.max_coordination_bytes = kContinuousMaxCoordinationBytes + 1;
    expect_create_error(bytes, ErrorCode::kBudgetExceeded);

    ContinuousPerceptionOptions entities = make_coordinator_options("cam");
    entities.max_entities = kContinuousMaxEntities + 1;
    expect_create_error(entities, ErrorCode::kBudgetExceeded);

    ContinuousPerceptionOptions relations = make_coordinator_options("cam");
    relations.max_relations = kContinuousMaxRelations + 1;
    expect_create_error(relations, ErrorCode::kBudgetExceeded);

    ContinuousPerceptionOptions events = make_coordinator_options("cam");
    events.max_events = kContinuousMaxEvents + 1;
    expect_create_error(events, ErrorCode::kBudgetExceeded);

    ContinuousPerceptionOptions conditions = make_coordinator_options("cam");
    conditions.max_focus_conditions = kContinuousMaxFocusConditions + 1;
    expect_create_error(conditions, ErrorCode::kBudgetExceeded);
}

TEST(ContinuousPerceptionCoordinatorTest, CreateAcceptsBudgetsTightenedToTheExactLimits) {
    ContinuousPerceptionOptions bytes = make_coordinator_options("cam");
    bytes.max_coordination_bytes = kContinuousMaxCoordinationBytes;
    EXPECT_TRUE(ContinuousPerception::create(bytes).ok());

    ContinuousPerceptionOptions entities = make_coordinator_options("cam");
    entities.max_entities = kContinuousMaxEntities;
    EXPECT_TRUE(ContinuousPerception::create(entities).ok());

    ContinuousPerceptionOptions relations = make_coordinator_options("cam");
    relations.max_relations = kContinuousMaxRelations;
    EXPECT_TRUE(ContinuousPerception::create(relations).ok());

    ContinuousPerceptionOptions events = make_coordinator_options("cam");
    events.max_events = kContinuousMaxEvents;
    EXPECT_TRUE(ContinuousPerception::create(events).ok());

    ContinuousPerceptionOptions conditions = make_coordinator_options("cam");
    conditions.max_focus_conditions = kContinuousMaxFocusConditions;
    EXPECT_TRUE(ContinuousPerception::create(conditions).ok());

    ContinuousPerceptionOptions tightened = make_coordinator_options("cam");
    tightened.max_coordination_bytes = 1024;
    tightened.max_entities = 16;
    tightened.max_relations = 32;
    tightened.max_events = 8;
    tightened.max_focus_conditions = 4;
    const auto created = ContinuousPerception::create(tightened);
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(created.value().options().max_coordination_bytes, 1024);
    EXPECT_EQ(created.value().options().max_entities, 16);
}

TEST(ContinuousPerceptionCoordinatorTest, SourceEpochIsMonotonicWithExplicitResetReports) {
    auto created = ContinuousPerception::create(make_coordinator_options("cam"));
    ASSERT_TRUE(created.ok());
    ContinuousPerception coordinator = created.take_value();
    EXPECT_EQ(coordinator.source_epoch(), 1U);

    const SourceResetReport first = coordinator.notify_source_reset();
    EXPECT_EQ(first.previous_epoch, 1U);
    EXPECT_EQ(first.new_epoch, 2U);
    EXPECT_EQ(coordinator.source_epoch(), 2U);

    const SourceResetReport second = coordinator.notify_source_reset();
    EXPECT_EQ(second.previous_epoch, 2U);
    EXPECT_EQ(second.new_epoch, 3U);
    EXPECT_EQ(coordinator.source_epoch(), 3U);

    const SourceResetReport third = coordinator.notify_source_reset();
    EXPECT_EQ(third.previous_epoch, 3U);
    EXPECT_EQ(third.new_epoch, 4U);
}

TEST(ContinuousPerceptionCoordinatorTest, CoordinationBytesStayZeroWhileNoStateIsStored) {
    auto created = ContinuousPerception::create(make_coordinator_options("cam"));
    ASSERT_TRUE(created.ok());
    ContinuousPerception coordinator = created.take_value();
    EXPECT_EQ(coordinator.coordination_owned_bytes(), 0);
    static_cast<void>(coordinator.notify_source_reset());
    EXPECT_EQ(coordinator.coordination_owned_bytes(), 0);
}

TEST(ContinuousPerceptionCoordinatorTest, MoveSemanticsPreserveIdentity) {
    auto created = ContinuousPerception::create(make_coordinator_options("cam"));
    ASSERT_TRUE(created.ok());
    ContinuousPerception moved = created.take_value();
    static_cast<void>(moved.notify_source_reset());

    ContinuousPerception target = std::move(moved);
    EXPECT_EQ(target.source_epoch(), 2U);
    EXPECT_EQ(target.source_id(), "cam");
    EXPECT_EQ(target.coordination_owned_bytes(), 0);

    auto other = ContinuousPerception::create(make_coordinator_options("screen"));
    ASSERT_TRUE(other.ok());
    ContinuousPerception assigned = other.take_value();
    assigned = std::move(target);
    EXPECT_EQ(assigned.source_epoch(), 2U);
    EXPECT_EQ(assigned.source_id(), "cam");
}

TEST(ContinuousPerceptionCoordinatorTest, TwinCoordinatorsBehaveIdentically) {
    auto first_result = ContinuousPerception::create(make_coordinator_options("cam"));
    auto second_result = ContinuousPerception::create(make_coordinator_options("cam"));
    ASSERT_TRUE(first_result.ok());
    ASSERT_TRUE(second_result.ok());
    ContinuousPerception first = first_result.take_value();
    ContinuousPerception second = second_result.take_value();

    EXPECT_EQ(first.options(), second.options());
    EXPECT_EQ(first.source_epoch(), second.source_epoch());
    EXPECT_EQ(first.coordination_owned_bytes(), second.coordination_owned_bytes());

    const SourceResetReport first_report = first.notify_source_reset();
    const SourceResetReport second_report = second.notify_source_reset();
    EXPECT_EQ(first_report, second_report);
    EXPECT_EQ(first.source_epoch(), second.source_epoch());

    // The pure surface agrees on identical inputs across instances, too.
    const AttentionPolicy policy = base_policy();
    EXPECT_EQ(attention_policy_digest(policy), attention_policy_digest(policy));
    const RelationDecision first_trace = [&] {
        RelationDecision trace;
        const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, 5.0F},
                                                     true, true, {}, &trace);
        EXPECT_TRUE(result.ok());
        return trace;
    }();
    const RelationDecision second_trace = [&] {
        RelationDecision trace;
        const auto result = classify_entity_relation(RectF{0.0F, 0.0F, 10.0F, 10.0F}, RectF{0.0F, 0.0F, 10.0F, 5.0F},
                                                     true, true, {}, &trace);
        EXPECT_TRUE(result.ok());
        return trace;
    }();
    EXPECT_EQ(first_trace.iou, second_trace.iou);
    EXPECT_EQ(first_trace.containment, second_trace.containment);
    EXPECT_EQ(first_trace.size_ratio, second_trace.size_ratio);
}

}  // namespace
