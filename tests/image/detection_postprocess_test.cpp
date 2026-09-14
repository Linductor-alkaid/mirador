// M3-05: unit tests for the detection reference components NMS and
// category filter (design section 14).

#include <cstdint>
#include <mirador/detection_postprocess.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "mirador/detector_backend.hpp"
#include "mirador/geometry.hpp"
#include "mirador/status.hpp"

namespace {

using mirador::DetectionFilterParams;
using mirador::DetectionRegion;
using mirador::ErrorCode;
using mirador::NmsParams;
using mirador::RectF;

DetectionRegion make_region(float x, float y, float w, float h, float confidence, int32_t class_id = 0,
                            std::string label = "") {
    DetectionRegion region;
    region.bounds = RectF{x, y, w, h};
    region.confidence = confidence;
    region.class_id = class_id;
    region.label = std::move(label);
    return region;
}

TEST(IntersectionOverUnion, IdenticalBoxesAreOneAndDisjointAreZero) {
    const RectF a{0.0F, 0.0F, 10.0F, 10.0F};
    EXPECT_DOUBLE_EQ(mirador::intersection_over_union(a, a), 1.0);
    EXPECT_DOUBLE_EQ(mirador::intersection_over_union(a, RectF{20.0F, 20.0F, 5.0F, 5.0F}), 0.0);
    // Half-overlap of two 10x10 boxes: intersection 50, union 150.
    EXPECT_NEAR(mirador::intersection_over_union(a, RectF{5.0F, 0.0F, 10.0F, 10.0F}), 1.0 / 3.0, 1e-12);
    EXPECT_DOUBLE_EQ(mirador::intersection_over_union(a, RectF{0.0F, 0.0F, 0.0F, 10.0F}), 0.0);  // degenerate
}

TEST(Nms, SuppressesOverlappingKeepsDistinct) {
    const std::vector<DetectionRegion> regions{
        make_region(0.0F, 0.0F, 10.0F, 10.0F, 0.9F),
        make_region(1.0F, 0.0F, 10.0F, 10.0F, 0.8F),    // IoU 9/11 > 0.5 with the first
        make_region(50.0F, 50.0F, 10.0F, 10.0F, 0.7F),  // disjoint
    };
    const auto kept = mirador::nms(regions, NmsParams{});
    ASSERT_TRUE(kept.ok()) << kept.status().message();
    ASSERT_EQ(kept.value().size(), 2U);
    EXPECT_FLOAT_EQ(kept.value()[0].confidence, 0.9F);  // score-descending output
    EXPECT_FLOAT_EQ(kept.value()[1].confidence, 0.7F);
}

TEST(Nms, ClassAwareSuppressionIsolatesClasses) {
    const std::vector<DetectionRegion> regions{
        make_region(0.0F, 0.0F, 10.0F, 10.0F, 0.9F, 1),
        make_region(0.0F, 0.0F, 10.0F, 10.0F, 0.8F, 2),  // same box, other class: survives
        make_region(0.0F, 0.0F, 10.0F, 10.0F, 0.7F, 1),  // same class: suppressed
    };
    auto kept = mirador::nms(regions, NmsParams{0.5F, true, 0});
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 2U);
    EXPECT_EQ(kept.value()[0].class_id, 1);
    EXPECT_EQ(kept.value()[1].class_id, 2);

    kept = mirador::nms(regions, NmsParams{0.5F, false, 0});
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 1U);  // class-blind: only the best box survives
}

TEST(Nms, EqualConfidenceBreaksTieByInputOrder) {
    const std::vector<DetectionRegion> regions{
        make_region(1.0F, 1.0F, 4.0F, 4.0F, 0.5F), make_region(20.0F, 20.0F, 4.0F, 4.0F, 0.5F),
        make_region(0.5F, 0.5F, 4.0F, 4.0F, 0.5F),  // overlaps box 0: suppressed by it
    };
    const auto kept = mirador::nms(regions, NmsParams{0.1F, false, 0});
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 2U);
    EXPECT_FLOAT_EQ(kept.value()[0].bounds.x, 1.0F);   // index 0 wins the tie...
    EXPECT_FLOAT_EQ(kept.value()[1].bounds.x, 20.0F);  // ...then the disjoint index 1
}

TEST(Nms, MaxOutputCapsAndSuppressionIgnoresSurplus) {
    const std::vector<DetectionRegion> regions{
        make_region(0.0F, 0.0F, 10.0F, 10.0F, 0.9F),
        make_region(0.0F, 0.0F, 10.0F, 10.0F, 0.8F),
        make_region(40.0F, 40.0F, 4.0F, 4.0F, 0.7F),
    };
    const auto kept = mirador::nms(regions, NmsParams{0.5F, true, 1});
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 1U);
    EXPECT_FLOAT_EQ(kept.value()[0].confidence, 0.9F);
}

TEST(Nms, RejectsInvalidParamsAndInputs) {
    const std::vector<DetectionRegion> regions{make_region(0.0F, 0.0F, 4.0F, 4.0F, 0.5F)};

    ASSERT_EQ(mirador::nms(regions, NmsParams{1.5F, true, 0}).status().code(), ErrorCode::kInvalidArgument);
    ASSERT_EQ(mirador::nms(regions, NmsParams{0.5F, true, -1}).status().code(), ErrorCode::kInvalidArgument);

    std::vector<DetectionRegion> nan_input{
        make_region(0.0F, 0.0F, 4.0F, 4.0F, std::numeric_limits<float>::quiet_NaN())};
    ASSERT_EQ(mirador::nms(nan_input, NmsParams{}).status().code(), ErrorCode::kInvalidArgument);
}

TEST(FilterDetections, ConfidenceClassAndLabelConjoin) {
    const std::vector<DetectionRegion> regions{
        make_region(0.0F, 0.0F, 4.0F, 4.0F, 0.9F, 1, "icon"),
        make_region(5.0F, 0.0F, 4.0F, 4.0F, 0.6F, 2, "text"),
        make_region(9.0F, 0.0F, 4.0F, 4.0F, 0.3F, 1, "icon"),
    };

    DetectionFilterParams params;
    params.min_confidence = 0.5F;
    auto kept = mirador::filter_detections(regions, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 2U);  // drops the 0.3 proposal
    EXPECT_FLOAT_EQ(kept.value()[0].confidence, 0.9F);
    EXPECT_FLOAT_EQ(kept.value()[1].confidence, 0.6F);

    params.class_ids = {2};
    kept = mirador::filter_detections(regions, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 1U);
    EXPECT_EQ(kept.value()[0].class_id, 2);

    params = DetectionFilterParams{};
    params.labels = {"icon"};
    kept = mirador::filter_detections(regions, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 2U);
    EXPECT_EQ(kept.value()[1].confidence, 0.3F);  // order preserved, no confidence gate
}

TEST(FilterDetections, RejectsInvalidParams) {
    const std::vector<DetectionRegion> regions{make_region(0.0F, 0.0F, 4.0F, 4.0F, 0.5F)};
    DetectionFilterParams params;
    params.min_confidence = -0.1F;
    ASSERT_EQ(mirador::filter_detections(regions, params).status().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
