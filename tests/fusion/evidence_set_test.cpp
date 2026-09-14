// M4 evidence model tests (M4-02): sequential evidence ids from 1, kind-wise
// accessors, cache provenance bits, validation of bounds/space/similarity and
// the kMaxItems budget (RULE-06: explicit kBudgetExceeded, never silent drop).

#include <mirador/evidence.hpp>

#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using mirador::CoordinateSpaceId;
using mirador::DetectionRegion;
using mirador::ErrorCode;
using mirador::EvidenceItem;
using mirador::EvidenceKind;
using mirador::EvidenceSet;
using mirador::ExternalRegion;
using mirador::RectF;
using mirador::RegionSource;
using mirador::TextRegion;

ExternalRegion make_external(float x, float y, float width, float height) {
    ExternalRegion region;
    region.bounds = RectF{x, y, width, height};
    region.text = "OK";
    region.role = "button";
    return region;
}

// --- Id assignment and kind-wise accessors ----------------------------------------

TEST(EvidenceSetTest, AssignsSequentialIdsFromOneInAddOrder) {
    EvidenceSet evidence;
    const auto first = evidence.add_external(make_external(0.0F, 0.0F, 10.0F, 10.0F));
    TextRegion text;
    text.bounds = RectF{1.0F, 1.0F, 5.0F, 5.0F};
    text.utf8_text = "hi";
    text.confidence = 0.9F;
    const auto second = evidence.add_text(text);
    DetectionRegion detection;
    detection.bounds = RectF{2.0F, 2.0F, 5.0F, 5.0F};
    detection.class_id = 1;
    detection.label = "icon";
    detection.confidence = 0.8F;
    const auto third = evidence.add_detection(detection);
    const auto fourth = evidence.add_template(77U, RectF{3.0F, 3.0F, 5.0F, 5.0F}, 0.7);

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(third.ok());
    ASSERT_TRUE(fourth.ok());
    EXPECT_EQ(first.value(), 1U);
    EXPECT_EQ(second.value(), 2U);
    EXPECT_EQ(third.value(), 3U);
    EXPECT_EQ(fourth.value(), 4U);
    ASSERT_EQ(evidence.size(), 4U);

    EXPECT_EQ(evidence.items()[0].kind, EvidenceKind::kExternal);
    EXPECT_EQ(evidence.items()[1].kind, EvidenceKind::kText);
    EXPECT_EQ(evidence.items()[2].kind, EvidenceKind::kDetection);
    EXPECT_EQ(evidence.items()[3].kind, EvidenceKind::kTemplate);
}

TEST(EvidenceSetTest, ItemAccessorsDispatchByKind) {
    EvidenceSet evidence;
    const ExternalRegion external = make_external(0.0F, 0.0F, 10.0F, 10.0F);
    TextRegion text;
    text.bounds = RectF{1.0F, 1.0F, 5.0F, 5.0F};
    text.utf8_text = "ocr text";
    text.confidence = 0.5F;
    DetectionRegion detection;
    detection.bounds = RectF{2.0F, 2.0F, 5.0F, 5.0F};
    detection.class_id = 3;
    detection.label = "det-label";
    detection.confidence = 0.25F;

    ASSERT_TRUE(evidence.add_external(external).ok());
    ASSERT_TRUE(evidence.add_text(text).ok());
    ASSERT_TRUE(evidence.add_detection(detection).ok());
    ASSERT_TRUE(evidence.add_template(9U, RectF{3.0F, 3.0F, 5.0F, 5.0F}, 0.7).ok());

    const std::vector<EvidenceItem>& items = evidence.items();
    EXPECT_EQ(items[0].bounds(), external.bounds);
    EXPECT_EQ(items[1].bounds(), text.bounds);
    EXPECT_EQ(items[2].bounds(), detection.bounds);
    EXPECT_EQ(items[3].bounds(), (RectF{3.0F, 3.0F, 5.0F, 5.0F}));

    EXPECT_EQ(items[0].text_or_label(), "OK");
    EXPECT_EQ(items[1].text_or_label(), "ocr text");
    EXPECT_EQ(items[2].text_or_label(), "det-label");
    EXPECT_EQ(items[3].text_or_label(), "");

    EXPECT_FLOAT_EQ(items[0].confidence(), 1.0F);  // ExternalRegion default confidence
    EXPECT_FLOAT_EQ(items[1].confidence(), 0.5F);
    EXPECT_FLOAT_EQ(items[2].confidence(), 0.25F);
    EXPECT_FLOAT_EQ(items[3].confidence(), 0.7F);
    EXPECT_EQ(items[3].template_entry_id, 9U);
}

TEST(EvidenceSetTest, TemplateConfidenceIsClampedIntoUnitInterval) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_template(1U, RectF{0.0F, 0.0F, 4.0F, 4.0F}, 1.5).ok());
    ASSERT_TRUE(evidence.add_template(2U, RectF{0.0F, 0.0F, 4.0F, 4.0F}, -0.5).ok());
    ASSERT_TRUE(evidence.add_template(3U, RectF{0.0F, 0.0F, 4.0F, 4.0F}, 0.3).ok());
    EXPECT_FLOAT_EQ(evidence.items()[0].confidence(), 1.0F);
    EXPECT_FLOAT_EQ(evidence.items()[1].confidence(), 0.0F);
    EXPECT_FLOAT_EQ(evidence.items()[2].confidence(), 0.3F);
}

TEST(EvidenceSetTest, SourceMasksCarryKindBitAndCacheBit) {
    EvidenceSet evidence;
    TextRegion text;
    text.bounds = RectF{0.0F, 0.0F, 4.0F, 4.0F};
    DetectionRegion detection;
    detection.bounds = RectF{0.0F, 0.0F, 4.0F, 4.0F};

    ASSERT_TRUE(evidence.add_external(make_external(0.0F, 0.0F, 4.0F, 4.0F)).ok());
    ASSERT_TRUE(evidence.add_text(text, CoordinateSpaceId::kOriented, false).ok());
    ASSERT_TRUE(evidence.add_text(text, CoordinateSpaceId::kOriented, true).ok());
    ASSERT_TRUE(evidence.add_detection(detection, CoordinateSpaceId::kOriented, false).ok());
    ASSERT_TRUE(evidence.add_detection(detection, CoordinateSpaceId::kOriented, true).ok());
    ASSERT_TRUE(evidence.add_template(1U, RectF{0.0F, 0.0F, 4.0F, 4.0F}, 0.5, CoordinateSpaceId::kOriented, true).ok());

    const std::vector<EvidenceItem>& items = evidence.items();
    EXPECT_EQ(items[0].source, static_cast<uint32_t>(RegionSource::kExternal));
    EXPECT_EQ(items[1].source, static_cast<uint32_t>(RegionSource::kOcr));
    EXPECT_TRUE(has_source(items[2].source, RegionSource::kOcr));
    EXPECT_TRUE(has_source(items[2].source, RegionSource::kCache));
    EXPECT_TRUE(has_source(items[3].source, RegionSource::kDetector));
    EXPECT_FALSE(has_source(items[3].source, RegionSource::kCache));
    EXPECT_TRUE(has_source(items[4].source, RegionSource::kDetector));
    EXPECT_TRUE(has_source(items[4].source, RegionSource::kCache));
    EXPECT_TRUE(has_source(items[5].source, RegionSource::kTemplate));
    EXPECT_TRUE(has_source(items[5].source, RegionSource::kCache));
}

TEST(EvidenceSetTest, RecordedSpaceRoundTrips) {
    EvidenceSet evidence;
    TextRegion text;
    text.bounds = RectF{0.0F, 0.0F, 4.0F, 4.0F};
    ASSERT_TRUE(evidence.add_text(text, CoordinateSpaceId::kFrame).ok());
    EXPECT_EQ(evidence.items()[0].space, CoordinateSpaceId::kFrame);
}

// --- Validation -------------------------------------------------------------------

TEST(EvidenceSetTest, RejectsNonFiniteOrNegativeSizeBounds) {
    EvidenceSet evidence;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    ExternalRegion nan_bounds = make_external(0.0F, 0.0F, nan, 4.0F);
    EXPECT_EQ(evidence.add_external(nan_bounds).status().code(), ErrorCode::kInvalidArgument);
    ExternalRegion inf_bounds = make_external(inf, 0.0F, 4.0F, 4.0F);
    EXPECT_EQ(evidence.add_external(inf_bounds).status().code(), ErrorCode::kInvalidArgument);

    TextRegion negative;
    negative.bounds = RectF{0.0F, 0.0F, 4.0F, -1.0F};
    EXPECT_EQ(evidence.add_text(negative).status().code(), ErrorCode::kInvalidArgument);

    DetectionRegion negative_detection;
    negative_detection.bounds = RectF{0.0F, 0.0F, -4.0F, 1.0F};
    EXPECT_EQ(evidence.add_detection(negative_detection).status().code(), ErrorCode::kInvalidArgument);

    EXPECT_EQ(evidence.add_template(1U, (RectF{0.0F, 0.0F, nan, 1.0F}), 0.5).status().code(),
              ErrorCode::kInvalidArgument);
    EXPECT_EQ(evidence.add_template(1U, (RectF{0.0F, 0.0F, 1.0F, 1.0F}), nan).status().code(),
              ErrorCode::kInvalidArgument);
    EXPECT_TRUE(evidence.empty());
}

TEST(EvidenceSetTest, RejectsSpacesOutsideFrameAndOriented) {
    EvidenceSet evidence;
    TextRegion text;
    text.bounds = RectF{0.0F, 0.0F, 4.0F, 4.0F};

    EXPECT_EQ(evidence.add_text(text, CoordinateSpaceId::kModelInput).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(evidence.add_text(text, CoordinateSpaceId::kCropped).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(evidence.add_text(text, CoordinateSpaceId::kDisplay).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(evidence.add_text(text, mirador::CoordinateSpaceId::kUserBase).status().code(),
              ErrorCode::kInvalidArgument);
    EXPECT_TRUE(evidence.empty());
    ASSERT_TRUE(evidence.add_text(text, CoordinateSpaceId::kFrame).ok());
}

// --- Budget -----------------------------------------------------------------------

TEST(EvidenceSetTest, AddBeyondMaxItemsFailsWithBudgetExceeded) {
    EvidenceSet evidence;
    TextRegion text;
    text.bounds = RectF{0.0F, 0.0F, 1.0F, 1.0F};
    for (size_t index = 0; index < EvidenceSet::kMaxItems; ++index) {
        ASSERT_TRUE(evidence.add_text(text).ok());
    }
    EXPECT_EQ(evidence.size(), EvidenceSet::kMaxItems);
    const auto overflow = evidence.add_text(text);
    ASSERT_FALSE(overflow.ok());
    EXPECT_EQ(overflow.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(evidence.size(), EvidenceSet::kMaxItems);  // the failed add left no trace
}

}  // namespace
