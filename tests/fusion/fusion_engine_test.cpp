// M4 fusion engine tests (M4-03): gate boundaries (IoU/containment inclusive),
// class compatibility, no distance-only association, deterministic output and
// trace, aggregation rules, coordinate conversion across rotations (incl. odd
// 5x3 raw sizes), explicit errors and budgets.

#include <mirador/fusion.hpp>

#include <mirador/evidence.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace {

using mirador::AssociationObservation;
using mirador::AssociationRule;
using mirador::ConfidenceContribution;
using mirador::CoordinateSpaceId;
using mirador::DetectionRegion;
using mirador::ErrorCode;
using mirador::EvidenceSet;
using mirador::ExecutionContext;
using mirador::ExternalRegion;
using mirador::Frame;
using mirador::FusionOptions;
using mirador::FusionOutput;
using mirador::PointF;
using mirador::RectF;
using mirador::RegionSource;
using mirador::Rotation;
using mirador::TextRegion;
using mirador::VisualRegion;

/// Minimal owned frame; `fuse_evidence` reads rotation/dimensions only, but the
/// buffer is real so the fixture stays valid for any consumer.
struct TestFrame {
    std::vector<std::byte> bytes;
    Frame frame;
};

TestFrame make_frame(int32_t width, int32_t height, Rotation rotation) {
    TestFrame image;
    const int64_t stride = static_cast<int64_t>(width) * 1;  // gray8
    image.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{0});
    image.frame.image.data = image.bytes.data();
    image.frame.image.width = width;
    image.frame.image.height = height;
    image.frame.image.row_stride_bytes = stride;
    image.frame.image.format = mirador::PixelFormat::kGray8;
    image.frame.image.rotation = rotation;
    image.frame.sequence = 1;
    return image;
}

TextRegion make_text(float x, float y, float width, float height, std::string text, float confidence = 0.5F) {
    TextRegion region;
    region.bounds = RectF{x, y, width, height};
    region.utf8_text = std::move(text);
    region.confidence = confidence;
    return region;
}

DetectionRegion make_detection(float x, float y, float width, float height, int32_t class_id, std::string label,
                               float confidence = 0.25F) {
    DetectionRegion region;
    region.bounds = RectF{x, y, width, height};
    region.class_id = class_id;
    region.label = std::move(label);
    region.confidence = confidence;
    return region;
}

ExternalRegion make_external(float x, float y, float width, float height, std::string text = {},
                             std::string role = {}) {
    ExternalRegion region;
    region.bounds = RectF{x, y, width, height};
    region.text = std::move(text);
    region.role = std::move(role);
    region.confidence = 1.0F;
    return region;
}

void expect_region_bounds(const VisualRegion& region, double x, double y, double width, double height) {
    EXPECT_NEAR(region.bounds.x, x, 1e-4);
    EXPECT_NEAR(region.bounds.y, y, 1e-4);
    EXPECT_NEAR(region.bounds.width, width, 1e-4);
    EXPECT_NEAR(region.bounds.height, height, 1e-4);
}

// --- IoU gate boundaries -----------------------------------------------------------

TEST(FusionGateTest, IouExactlyAtThresholdAssociates) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 10.0F, 10.0F, "a")).ok());
    ASSERT_TRUE(evidence.add_text(make_text(4.0F, 0.0F, 10.0F, 10.0F, "b")).ok());

    // Intersection 60, union 140: the implementation computes 60.0 / 140.0 in
    // double from these exact float coordinates.
    FusionOptions options;
    options.iou_threshold = 60.0 / 140.0;
    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame, options);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 1U);
    ASSERT_EQ(fused.value().trace.regions.size(), 1U);
    ASSERT_EQ(fused.value().trace.regions[0].associations.size(), 1U);
    const AssociationObservation& observation = fused.value().trace.regions[0].associations[0];
    EXPECT_EQ(observation.rule, AssociationRule::kIou);
    EXPECT_DOUBLE_EQ(observation.iou, 60.0 / 140.0);
    EXPECT_EQ(observation.first_evidence_id, 1U);
    EXPECT_EQ(observation.second_evidence_id, 2U);
}

TEST(FusionGateTest, IouJustBelowThresholdDoesNotAssociate) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 10.0F, 10.0F, "a")).ok());
    ASSERT_TRUE(evidence.add_text(make_text(4.0F, 0.0F, 10.0F, 10.0F, "b")).ok());

    FusionOptions options;
    options.iou_threshold = std::nextafter(60.0 / 140.0, 1.0);  // smallest double above the pair IoU
    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame, options);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 2U);
    EXPECT_TRUE(fused.value().trace.regions[0].associations.empty());
    EXPECT_TRUE(fused.value().trace.regions[1].associations.empty());
}

// --- Containment gate ----------------------------------------------------------------

TEST(FusionGateTest, ContainmentAtThresholdAssociatesSmallBoxInsideLargeBox) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_external(make_external(0.0F, 0.0F, 20.0F, 20.0F)).ok());
    // Intersection 80 of the smaller area 100: containment exactly 0.8; the
    // pair IoU is 80/420, below the default iou_threshold 0.5.
    ASSERT_TRUE(evidence.add_text(make_text(12.0F, 5.0F, 10.0F, 10.0F, "child")).ok());

    FusionOptions options;
    options.containment_threshold = 0.8;
    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame, options);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 1U);
    ASSERT_EQ(fused.value().trace.regions[0].associations.size(), 1U);
    const AssociationObservation& observation = fused.value().trace.regions[0].associations[0];
    EXPECT_EQ(observation.rule, AssociationRule::kContainment);
    EXPECT_DOUBLE_EQ(observation.containment, 0.8);
    EXPECT_DOUBLE_EQ(observation.iou, 80.0 / 420.0);
    expect_region_bounds(fused.value().regions[0], 0.0, 0.0, 22.0, 20.0);  // union bounds
}

TEST(FusionGateTest, ContainmentBelowThresholdDoesNotAssociate) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_external(make_external(0.0F, 0.0F, 20.0F, 20.0F)).ok());
    // Intersection 60 of 100: containment 0.6, below 0.8; IoU 60/440 below 0.5.
    ASSERT_TRUE(evidence.add_text(make_text(14.0F, 5.0F, 10.0F, 10.0F, "child")).ok());

    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    EXPECT_EQ(fused.value().regions.size(), 2U);
}

// --- Class compatibility ---------------------------------------------------------------

TEST(FusionGateTest, IncompatibleDetectionClassesNeverMergeDespiteFullOverlap) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_detection(make_detection(0.0F, 0.0F, 10.0F, 10.0F, 0, "button", 0.9F)).ok());
    ASSERT_TRUE(evidence.add_detection(make_detection(0.0F, 0.0F, 10.0F, 10.0F, 1, "icon", 0.8F)).ok());

    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);  // IoU 1.0, gates pass
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 2U);
    EXPECT_EQ(fused.value().regions[0].label, "button");
    EXPECT_EQ(fused.value().regions[1].label, "icon");
    EXPECT_TRUE(fused.value().trace.regions[0].associations.empty());
    EXPECT_TRUE(fused.value().trace.regions[1].associations.empty());
}

TEST(FusionGateTest, SameClassAndLabelMerges) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_detection(make_detection(0.0F, 0.0F, 10.0F, 10.0F, 4, "button", 0.9F)).ok());
    ASSERT_TRUE(evidence.add_detection(make_detection(2.0F, 0.0F, 10.0F, 10.0F, 4, "button", 0.8F)).ok());

    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 1U);
    EXPECT_EQ(fused.value().regions[0].label, "button");
}

TEST(FusionGateTest, UnknownClassOnOneSideLeavesTheDecisionToTheGates) {
    EvidenceSet evidence;
    DetectionRegion unknown = make_detection(0.0F, 0.0F, 10.0F, 10.0F, -1, "", 0.7F);
    ASSERT_TRUE(evidence.add_detection(unknown).ok());
    ASSERT_TRUE(evidence.add_detection(make_detection(1.0F, 1.0F, 10.0F, 10.0F, 2, "icon", 0.6F)).ok());

    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    EXPECT_EQ(fused.value().regions.size(), 1U);
}

TEST(FusionGateTest, AdjacentNonOverlappingTextsStaySeparate) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 10.0F, 10.0F, "left")).ok());
    ASSERT_TRUE(evidence.add_text(make_text(10.0F, 0.0F, 10.0F, 10.0F, "right")).ok());

    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 2U);
    EXPECT_EQ(fused.value().regions[0].text, "left");
    EXPECT_EQ(fused.value().regions[1].text, "right");
    for (const auto& trace : fused.value().trace.regions) {
        EXPECT_TRUE(trace.associations.empty());  // proximity alone must not merge
    }
}

// --- Aggregation rules -----------------------------------------------------------------

TEST(FusionAggregationTest, MasksIdsTextLabelBoundsAnchorAndConfidenceFollowTheDocumentedRules) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_external(make_external(0.0F, 0.0F, 20.0F, 20.0F, "E", "")).ok());             // id 1
    ASSERT_TRUE(evidence.add_text(make_text(2.0F, 2.0F, 20.0F, 20.0F, "T", 0.5F)).ok());                   // id 2
    ASSERT_TRUE(evidence.add_detection(make_detection(4.0F, 4.0F, 20.0F, 20.0F, 7, "icon", 0.25F)).ok());  // id 3
    ASSERT_TRUE(evidence.add_template(42U, RectF{100.0F, 100.0F, 10.0F, 10.0F}, 0.6).ok());                // id 4

    const TestFrame frame = make_frame(256, 256, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 2U);

    const VisualRegion& merged = fused.value().regions[0];
    EXPECT_EQ(merged.stable_id, 0U);  // stateless fusion leaves identity assignment to the tracker
    expect_region_bounds(merged, 0.0, 0.0, 24.0, 24.0);
    EXPECT_NEAR(merged.anchor.x, 12.0, 1e-5);
    EXPECT_NEAR(merged.anchor.y, 12.0, 1e-5);
    EXPECT_EQ(merged.text, "E\nT");  // external text participates, "\n"-joined in evidence order
    EXPECT_EQ(merged.label, "icon");
    EXPECT_EQ(merged.source_mask, static_cast<uint32_t>(RegionSource::kExternal) |
                                      static_cast<uint32_t>(RegionSource::kOcr) |
                                      static_cast<uint32_t>(RegionSource::kDetector));
    EXPECT_EQ(merged.evidence_ids, (std::vector<uint64_t>{1, 2, 3}));
    // confidence = sum(w*c) / sum(w) = (1.0*1.0 + 0.9*0.5 + 0.8*0.25) / 2.7
    EXPECT_NEAR(merged.confidence, (1.0F * 1.0F + 0.9F * 0.5F + 0.8F * 0.25F) / (1.0F + 0.9F + 0.8F), 1e-6F);

    const VisualRegion& lone = fused.value().regions[1];
    EXPECT_TRUE(has_source(lone.source_mask, RegionSource::kTemplate));
    EXPECT_EQ(lone.evidence_ids, std::vector<uint64_t>{4});
    EXPECT_NEAR(lone.confidence, 0.6, 1e-6);  // clamped similarity, single contributor

    // Trace is parallel to the regions and explains every decision.
    ASSERT_EQ(fused.value().trace.regions.size(), 2U);
    EXPECT_EQ(fused.value().trace.input_evidence_count, 4U);
    const auto& merged_trace = fused.value().trace.regions[0];
    EXPECT_EQ(merged_trace.evidence_ids, merged.evidence_ids);
    EXPECT_EQ(merged_trace.source_mask, merged.source_mask);
    ASSERT_EQ(merged_trace.confidence_contributions.size(), 3U);
    EXPECT_EQ(merged_trace.confidence_contributions[0].evidence_id, 1U);
    EXPECT_FLOAT_EQ(merged_trace.confidence_contributions[0].weight, 1.0F);
    EXPECT_FLOAT_EQ(merged_trace.confidence_contributions[0].confidence, 1.0F);
    EXPECT_EQ(merged_trace.confidence_contributions[1].evidence_id, 2U);
    EXPECT_FLOAT_EQ(merged_trace.confidence_contributions[1].weight, 0.9F);
    EXPECT_FLOAT_EQ(merged_trace.confidence_contributions[2].evidence_id, 3U);
    EXPECT_FLOAT_EQ(merged_trace.confidence_contributions[2].weight, 0.8F);
    ASSERT_EQ(merged_trace.associations.size(), 2U);
    EXPECT_EQ(merged_trace.associations[0].first_evidence_id, 1U);
    EXPECT_EQ(merged_trace.associations[0].second_evidence_id, 2U);
    EXPECT_EQ(merged_trace.associations[0].rule, AssociationRule::kIou);
    EXPECT_DOUBLE_EQ(merged_trace.associations[0].iou, 324.0 / 476.0);
    EXPECT_EQ(merged_trace.associations[1].first_evidence_id, 2U);
    EXPECT_EQ(merged_trace.associations[1].second_evidence_id, 3U);
    EXPECT_DOUBLE_EQ(merged_trace.associations[1].iou, 324.0 / 476.0);
}

TEST(FusionAggregationTest, ExternalRoleProvidesLabelWhenDetectionIsAbsent) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_external(make_external(0.0F, 0.0F, 10.0F, 10.0F, "", "button")).ok());
    const TestFrame frame = make_frame(64, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 1U);
    EXPECT_EQ(fused.value().regions[0].label, "button");
    EXPECT_EQ(fused.value().regions[0].source_mask, static_cast<uint32_t>(RegionSource::kExternal));
}

TEST(FusionAggregationTest, OutputRegionsAreOrderedBySmallestMemberEvidenceId) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 10.0F, 10.0F, "a")).ok());    // id 1
    ASSERT_TRUE(evidence.add_text(make_text(100.0F, 0.0F, 10.0F, 10.0F, "b")).ok());  // id 2, own cluster
    ASSERT_TRUE(evidence.add_text(make_text(1.0F, 0.0F, 10.0F, 10.0F, "c")).ok());    // id 3, joins id 1

    const TestFrame frame = make_frame(256, 64, Rotation::k0);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 2U);
    EXPECT_EQ(fused.value().regions[0].evidence_ids, (std::vector<uint64_t>{1, 3}));
    EXPECT_EQ(fused.value().regions[1].evidence_ids, std::vector<uint64_t>{2});
    EXPECT_EQ(fused.value().trace.regions[0].evidence_ids, (std::vector<uint64_t>{1, 3}));
}

TEST(FusionAggregationTest, SameInputProducesIdenticalOutputIncludingTrace) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_external(make_external(0.0F, 0.0F, 20.0F, 20.0F, "E", "button")).ok());
    ASSERT_TRUE(evidence.add_text(make_text(2.0F, 2.0F, 20.0F, 20.0F, "T")).ok());
    ASSERT_TRUE(evidence.add_detection(make_detection(4.0F, 4.0F, 20.0F, 20.0F, 7, "icon")).ok());
    ASSERT_TRUE(evidence.add_text(make_text(60.0F, 60.0F, 8.0F, 8.0F, "lonely")).ok());
    ASSERT_TRUE(evidence.add_template(9U, RectF{60.5F, 60.0F, 8.0F, 8.0F}, 0.55).ok());

    const TestFrame frame = make_frame(256, 256, Rotation::k0);
    const auto first = mirador::fuse_evidence(evidence, frame.frame);
    const auto second = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());

    ASSERT_EQ(first.value().regions.size(), second.value().regions.size());
    for (size_t index = 0; index < first.value().regions.size(); ++index) {
        EXPECT_TRUE(first.value().regions[index] == second.value().regions[index]) << "region " << index;
    }
    EXPECT_EQ(first.value().trace.input_evidence_count, second.value().trace.input_evidence_count);
    ASSERT_EQ(first.value().trace.regions.size(), second.value().trace.regions.size());
    for (size_t index = 0; index < first.value().trace.regions.size(); ++index) {
        const auto& left = first.value().trace.regions[index];
        const auto& right = second.value().trace.regions[index];
        EXPECT_TRUE(left.evidence_ids == right.evidence_ids);
        EXPECT_EQ(left.source_mask, right.source_mask);
        ASSERT_EQ(left.confidence_contributions.size(), right.confidence_contributions.size());
        for (size_t k = 0; k < left.confidence_contributions.size(); ++k) {
            EXPECT_EQ(left.confidence_contributions[k].evidence_id, right.confidence_contributions[k].evidence_id);
            EXPECT_EQ(left.confidence_contributions[k].weight, right.confidence_contributions[k].weight);
            EXPECT_EQ(left.confidence_contributions[k].confidence, right.confidence_contributions[k].confidence);
        }
        ASSERT_EQ(left.associations.size(), right.associations.size());
        for (size_t k = 0; k < left.associations.size(); ++k) {
            EXPECT_TRUE(left.associations[k].first_evidence_id == right.associations[k].first_evidence_id &&
                        left.associations[k].second_evidence_id == right.associations[k].second_evidence_id &&
                        left.associations[k].rule == right.associations[k].rule &&
                        left.associations[k].iou == right.associations[k].iou &&
                        left.associations[k].containment == right.associations[k].containment &&
                        left.associations[k].center_distance == right.associations[k].center_distance);
        }
    }
}

// --- Coordinate conversion --------------------------------------------------------------

TEST(FusionCoordinatesTest, FrameEvidenceLandsOnRotatedOrientedTargets) {
    // Raw 5x3 capture: view is 3x5 for k90/k270 and 5x3 for k180. Frame-space
    // rect {1, 0.5, 2, 1} and the hand-computed oriented expectations follow
    // make_rotation: k90 maps x_o = h - y, y_o = x (clockwise).
    struct Case {
        Rotation rotation;
        int32_t view_width;
        int32_t view_height;
        double x;
        double y;
        double width;
        double height;
    };
    const std::vector<Case> cases{
        {Rotation::k90, 3, 5, 1.5, 1.0, 1.0, 2.0},
        {Rotation::k180, 5, 3, 2.0, 1.5, 2.0, 1.0},
        {Rotation::k270, 3, 5, 0.5, 2.0, 1.0, 2.0},
    };
    for (const Case& expected : cases) {
        EvidenceSet evidence;
        ASSERT_TRUE(evidence.add_text(make_text(1.0F, 0.5F, 2.0F, 1.0F, "t"), CoordinateSpaceId::kFrame).ok());
        const TestFrame frame = make_frame(expected.view_width, expected.view_height, expected.rotation);
        const auto fused = mirador::fuse_evidence(evidence, frame.frame);  // target defaults to kOriented
        ASSERT_TRUE(fused.ok()) << "rotation " << static_cast<int>(expected.rotation);
        ASSERT_EQ(fused.value().regions.size(), 1U);
        SCOPED_TRACE(static_cast<int>(expected.rotation));
        expect_region_bounds(fused.value().regions[0], expected.x, expected.y, expected.width, expected.height);
    }
}

TEST(FusionCoordinatesTest, OrientedEvidenceConvertsBackToFrameTarget) {
    // View of a raw 5x3 capture rotated k90 (view 3x5). Evidence given in the
    // oriented space must come back in raw-frame coordinates.
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(1.5F, 1.0F, 1.0F, 2.0F, "t"), CoordinateSpaceId::kOriented).ok());
    const TestFrame frame = make_frame(3, 5, Rotation::k90);

    FusionOptions options;
    options.target_space = CoordinateSpaceId::kFrame;
    const auto fused = mirador::fuse_evidence(evidence, frame.frame, options);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 1U);
    expect_region_bounds(fused.value().regions[0], 1.0, 0.5, 2.0, 1.0);
    EXPECT_EQ(fused.value().regions[0].anchor.x, 2.0F);
    EXPECT_EQ(fused.value().regions[0].anchor.y, 1.0F);  // center of {1, 0.5, 2, 1}
}

TEST(FusionCoordinatesTest, SameSpaceEvidenceIsUntouched) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(1.0F, 2.0F, 3.0F, 4.0F, "t"), CoordinateSpaceId::kOriented).ok());
    const TestFrame frame = make_frame(16, 16, Rotation::k90);
    const auto fused = mirador::fuse_evidence(evidence, frame.frame);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 1U);
    expect_region_bounds(fused.value().regions[0], 1.0, 2.0, 3.0, 4.0);
}

// --- Errors, budgets, cancellation --------------------------------------------------------

TEST(FusionErrorsTest, UnsupportedTargetSpaceIsRejected) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 4.0F, 4.0F, "t")).ok());
    const TestFrame frame = make_frame(16, 16, Rotation::k0);

    FusionOptions options;
    options.target_space = CoordinateSpaceId::kModelInput;
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, options).status().code(), ErrorCode::kInvalidArgument);
}

TEST(FusionErrorsTest, InvalidOptionsAreRejected) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 4.0F, 4.0F, "t")).ok());
    const TestFrame frame = make_frame(16, 16, Rotation::k0);

    FusionOptions nan_threshold;
    nan_threshold.iou_threshold = std::nan("");
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, nan_threshold).status().code(),
              ErrorCode::kInvalidArgument);

    FusionOptions big_containment;
    big_containment.containment_threshold = 1.1;
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, big_containment).status().code(),
              ErrorCode::kInvalidArgument);

    FusionOptions negative_weight;
    negative_weight.ocr_weight = -0.1F;
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, negative_weight).status().code(),
              ErrorCode::kInvalidArgument);

    FusionOptions zero_budget;
    zero_budget.max_regions = 0;
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, zero_budget).status().code(), ErrorCode::kInvalidArgument);
}

TEST(FusionErrorsTest, MoreClustersThanMaxRegionsFailsWithBudgetExceeded) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 4.0F, 4.0F, "a")).ok());
    ASSERT_TRUE(evidence.add_text(make_text(50.0F, 50.0F, 4.0F, 4.0F, "b")).ok());
    const TestFrame frame = make_frame(128, 128, Rotation::k0);

    FusionOptions options;
    options.max_regions = 1;
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, options).status().code(), ErrorCode::kBudgetExceeded);
}

TEST(FusionErrorsTest, CancelledContextAndDeadlineAreExplicit) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(0.0F, 0.0F, 4.0F, 4.0F, "t")).ok());
    const TestFrame frame = make_frame(16, 16, Rotation::k0);

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, {}, cancelled).status().code(), ErrorCode::kCancelled);

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, {}, expired).status().code(), ErrorCode::kTimeout);
}

TEST(FusionErrorsTest, CancellationIsPolledEverySixtyFourPairScanStripes) {
    // 65 items: the pair scan polls at stripes 0 and 64. The context allows the
    // first two polls (entry + stripe 0) and cancels on the third (stripe 64),
    // proving the cadence rather than an entry-only check.
    EvidenceSet evidence;
    for (int32_t index = 0; index < 65; ++index) {
        ASSERT_TRUE(evidence.add_text(make_text(static_cast<float>(index) * 100.0F, 0.0F, 10.0F, 10.0F, "t")).ok());
    }
    const TestFrame frame = make_frame(8192, 16, Rotation::k0);

    int polls = 0;
    ExecutionContext late_cancel;
    late_cancel.is_cancelled = [&polls] { return ++polls > 2; };
    const auto fused = mirador::fuse_evidence(evidence, frame.frame, {}, late_cancel);
    ASSERT_FALSE(fused.ok());
    EXPECT_EQ(fused.status().code(), ErrorCode::kCancelled);
    EXPECT_GE(polls, 3);
}

// --- Property-style invariants -------------------------------------------------------------

TEST(FusionPropertyTest, RandomLegalCropsFuseIntoFiniteBoundsWithAnchorsInside) {
    std::mt19937 rng(20260915U);
    std::uniform_int_distribution<int32_t> origin(0, 48);
    std::uniform_int_distribution<int32_t> extent(1, 16);
    std::uniform_int_distribution<int> kind(0, 2);

    const TestFrame frame = make_frame(128, 128, Rotation::k0);
    for (int iteration = 0; iteration < 200; ++iteration) {
        EvidenceSet evidence;
        const int32_t count = static_cast<int32_t>(kind(rng)) + 1;
        for (int32_t index = 0; index < count; ++index) {
            const float x = static_cast<float>(origin(rng));
            const float y = static_cast<float>(origin(rng));
            const float width = static_cast<float>(extent(rng));
            const float height = static_cast<float>(extent(rng));
            switch (kind(rng)) {
                case 0:
                    ASSERT_TRUE(evidence.add_external(make_external(x, y, width, height)).ok());
                    break;
                case 1:
                    ASSERT_TRUE(evidence.add_text(make_text(x, y, width, height, "t")).ok());
                    break;
                default:
                    ASSERT_TRUE(
                        evidence.add_template(static_cast<uint64_t>(index), RectF{x, y, width, height}, 0.5).ok());
                    break;
            }
        }

        const auto fused = mirador::fuse_evidence(evidence, frame.frame);
        ASSERT_TRUE(fused.ok());
        ASSERT_FALSE(fused.value().regions.empty());
        for (const VisualRegion& region : fused.value().regions) {
            EXPECT_TRUE(std::isfinite(region.bounds.x));
            EXPECT_TRUE(std::isfinite(region.bounds.y));
            EXPECT_TRUE(std::isfinite(region.bounds.width));
            EXPECT_TRUE(std::isfinite(region.bounds.height));
            EXPECT_GE(region.bounds.width, 0.0F);
            EXPECT_GE(region.bounds.height, 0.0F);
            // anchor = union-bounds center: always inside (or on) the bounds.
            EXPECT_GE(region.anchor.x, region.bounds.x - 1e-5F);
            EXPECT_LE(region.anchor.x, region.bounds.x + region.bounds.width + 1e-5F);
            EXPECT_GE(region.anchor.y, region.bounds.y - 1e-5F);
            EXPECT_LE(region.anchor.y, region.bounds.y + region.bounds.height + 1e-5F);
        }
    }
}

}  // namespace
