// M4 session integration tests (M4-05): the PerceptionSession::fuse() loop
// over fake backends plus Accessibility-style external evidence — stable ids,
// source masks, generation policy, immutable published snapshots, change
// report plumbing and the explicit error/budget/cancellation contracts.

#include <mirador/perception_session.hpp>

#include "fake_backends.hpp"

#include <mirador/change_detection.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/evidence.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/fusion.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using mirador::test::FakeDetectorBackend;
using mirador::test::FakeOcrBackend;

using mirador::ChangeClassification;
using mirador::ChangeReason;
using mirador::CoordinateSpaceId;
using mirador::DetectionRegion;
using mirador::ErrorCode;
using mirador::EvidenceSet;
using mirador::ExecutionContext;
using mirador::ExternalRegion;
using mirador::Frame;
using mirador::FusionOptions;
using mirador::OcrRequest;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::RegionSource;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::TextRegion;

constexpr const char* kSourceId = "screen-main";

struct TestFrame {
    std::vector<std::byte> bytes;
    Frame frame;
};

TestFrame make_frame(int32_t width, int32_t height, uint64_t sequence, const char* source_id = kSourceId) {
    TestFrame image;
    const int64_t stride = static_cast<int64_t>(width) * 4;  // rgba8
    image.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{9});
    image.frame.image.data = image.bytes.data();
    image.frame.image.width = width;
    image.frame.image.height = height;
    image.frame.image.row_stride_bytes = stride;
    image.frame.image.format = PixelFormat::kRgba8;
    image.frame.image.rotation = Rotation::k0;
    image.frame.sequence = sequence;
    image.frame.source_id = source_id;
    image.frame.owner = std::make_shared<const std::vector<std::byte>>(image.bytes);
    return image;
}

PerceptionSession make_session() {
    PerceptionSessionOptions options;
    options.source_id = kSourceId;
    return PerceptionSession::create(std::move(options)).take_value();
}

TextRegion ocr_style_region() {
    // Same geometry the FakeOcrBackend recovers for a 100x80 full-frame run.
    TextRegion region;
    region.bounds = RectF{25.0F, 20.0F, 50.0F, 40.0F};
    region.utf8_text = "fake";
    region.confidence = 0.9F;
    return region;
}

DetectionRegion detector_style_region() {
    // Same geometry the FakeDetectorBackend recovers for a 100x80 full-frame run.
    DetectionRegion region;
    region.bounds = RectF{0.0F, 0.0F, 50.0F, 40.0F};
    region.class_id = 3;
    region.label = "icon";
    region.confidence = 0.8F;
    return region;
}

ExternalRegion accessibility_button() {
    ExternalRegion region;
    region.bounds = RectF{10.0F, 10.0F, 60.0F, 60.0F};
    region.text = "OK";
    region.role = "button";
    region.confidence = 1.0F;
    region.interactive = true;
    return region;
}

/// Evidence mirroring the fake backend outputs plus an Accessibility button.
/// Expected association: button+text merge (containment 0.9 >= 0.8), the
/// detection stays separate (gates fail against both other boxes).
EvidenceSet make_e2e_evidence() {
    EvidenceSet evidence;
    EXPECT_TRUE(evidence.add_external(accessibility_button(), CoordinateSpaceId::kOriented).ok());
    EXPECT_TRUE(evidence.add_text(ocr_style_region(), CoordinateSpaceId::kOriented).ok());
    EXPECT_TRUE(evidence.add_detection(detector_style_region(), CoordinateSpaceId::kOriented).ok());
    return evidence;
}

// --- End-to-end fusion ----------------------------------------------------------------

TEST(SessionFuseTest, EndToEndFusePublishesStableIdsAndSourceMasks) {
    PerceptionSession session = make_session();
    FakeOcrBackend ocr;
    FakeDetectorBackend detector;
    const TestFrame frame = make_frame(100, 80, 7U);

    // Real capability runs feed the evidence set (fake backends, no runtime).
    const auto texts = session.run_ocr(frame.frame, &ocr, OcrRequest{});
    ASSERT_TRUE(texts.ok());
    ASSERT_EQ(texts.value().size(), 1U);
    const auto detections = session.run_detector(frame.frame, &detector, {});
    ASSERT_TRUE(detections.ok());
    ASSERT_EQ(detections.value().size(), 1U);

    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_external(accessibility_button(), CoordinateSpaceId::kOriented).ok());
    ASSERT_TRUE(evidence.add_text(texts.value()[0], CoordinateSpaceId::kOriented).ok());
    ASSERT_TRUE(evidence.add_detection(detections.value()[0], CoordinateSpaceId::kOriented).ok());

    EXPECT_EQ(session.latest_snapshot(), nullptr);  // nothing published before the first fuse
    const auto change = session.analyze_change(frame.frame);
    ASSERT_TRUE(change.ok());
    ASSERT_EQ(change.value().classification, ChangeClassification::kGlobal);

    const auto fused = session.fuse(frame.frame, evidence);
    ASSERT_TRUE(fused.ok());
    const SemanticSnapshot& snapshot = fused.value();

    EXPECT_EQ(snapshot.frame_sequence, 7U);
    EXPECT_EQ(snapshot.generation, 1U);  // first publication is generation 1
    EXPECT_EQ(snapshot.coordinate_space, CoordinateSpaceId::kOriented);
    EXPECT_EQ(snapshot.change.classification, ChangeClassification::kGlobal);
    EXPECT_EQ(snapshot.change.reason, ChangeReason::kFirstFrame);

    ASSERT_EQ(snapshot.regions.size(), 2U);
    const auto& merged = snapshot.regions[0];  // external + ocr cluster
    EXPECT_EQ(merged.stable_id, 1U);
    EXPECT_TRUE(has_source(merged.source_mask, RegionSource::kExternal));
    EXPECT_TRUE(has_source(merged.source_mask, RegionSource::kOcr));
    EXPECT_FALSE(has_source(merged.source_mask, RegionSource::kDetector));
    EXPECT_EQ(merged.text, "OK\nfake");
    EXPECT_EQ(merged.label, "button");
    EXPECT_EQ(merged.evidence_ids, (std::vector<uint64_t>{1, 2}));

    const auto& icon = snapshot.regions[1];
    EXPECT_EQ(icon.stable_id, 2U);
    EXPECT_TRUE(has_source(icon.source_mask, RegionSource::kDetector));
    EXPECT_EQ(icon.label, "icon");

    EXPECT_EQ(session.last_stable_id(), 2U);
    ASSERT_NE(session.latest_snapshot(), nullptr);
    EXPECT_TRUE(*session.latest_snapshot() == snapshot);
}

TEST(SessionFuseTest, UnchangedSceneKeepsStableIdsAndGeneration) {
    PerceptionSession session = make_session();
    const TestFrame frame = make_frame(100, 80, 1U);
    const EvidenceSet evidence = make_e2e_evidence();

    // Prime the change layer so the later analyze_change compares two frames.
    const auto first_change = session.analyze_change(frame.frame);
    ASSERT_TRUE(first_change.ok());
    ASSERT_EQ(first_change.value().classification, ChangeClassification::kGlobal);

    ASSERT_TRUE(session.fuse(frame.frame, evidence).ok());
    // The change layer sees no change on the identical frame (reuse path).
    const auto change = session.analyze_change(frame.frame);
    ASSERT_TRUE(change.ok());
    ASSERT_EQ(change.value().classification, ChangeClassification::kNone);

    const auto refused = session.fuse(frame.frame, evidence);
    ASSERT_TRUE(refused.ok());
    ASSERT_EQ(refused.value().regions.size(), 2U);
    EXPECT_EQ(refused.value().regions[0].stable_id, 1U);  // retained
    EXPECT_EQ(refused.value().regions[1].stable_id, 2U);
    EXPECT_EQ(refused.value().generation, 1U);  // retained 2/2: no bump
    EXPECT_EQ(refused.value().change.classification, ChangeClassification::kNone);
}

TEST(SessionFuseTest, PublishedSnapshotsAreImmutableAcrossLaterFusions) {
    PerceptionSession session = make_session();
    const TestFrame frame = make_frame(100, 80, 1U);
    const auto first = session.fuse(frame.frame, make_e2e_evidence());
    ASSERT_TRUE(first.ok());

    // Keep the published snapshot alive across a later, unrelated fusion.
    const std::shared_ptr<const SemanticSnapshot> published = session.latest_snapshot();
    ASSERT_NE(published, nullptr);

    EvidenceSet moved;
    ExternalRegion elsewhere;
    elsewhere.bounds = RectF{400.0F, 400.0F, 10.0F, 10.0F};
    ASSERT_TRUE(moved.add_external(elsewhere, CoordinateSpaceId::kOriented).ok());
    ExternalRegion more;
    more.bounds = RectF{600.0F, 400.0F, 10.0F, 10.0F};
    ASSERT_TRUE(moved.add_external(more, CoordinateSpaceId::kOriented).ok());
    const auto second = session.fuse(frame.frame, moved);
    ASSERT_TRUE(second.ok());

    // New regions get fresh ids and the retained ratio (0/2) bumps the generation.
    ASSERT_EQ(second.value().regions.size(), 2U);
    EXPECT_EQ(second.value().regions[0].stable_id, 3U);
    EXPECT_EQ(second.value().regions[1].stable_id, 4U);
    EXPECT_EQ(second.value().generation, 2U);

    // The earlier shared_ptr still observes exactly the first publication.
    EXPECT_EQ(published->generation, 1U);
    ASSERT_EQ(published->regions.size(), 2U);
    EXPECT_EQ(published->regions[0].stable_id, 1U);
    EXPECT_EQ(published->regions[0].bounds, (RectF{10.0F, 10.0F, 65.0F, 60.0F}));
    EXPECT_EQ(published->regions[1].stable_id, 2U);
    EXPECT_NE(published.get(), session.latest_snapshot().get());
}

// --- Error, cancellation and budget contracts ---------------------------------------------

TEST(SessionFuseTest, ForeignSourceAndInvalidFramesAreRejected) {
    PerceptionSession session = make_session();
    const TestFrame foreign = make_frame(40, 30, 1U, "other-window");
    const EvidenceSet evidence = make_e2e_evidence();
    const auto foreign_result = session.fuse(foreign.frame, evidence);
    EXPECT_EQ(foreign_result.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(session.latest_snapshot(), nullptr);  // nothing published on error

    Frame invalid;  // default view carries no data
    invalid.sequence = 1U;
    const auto invalid_result = session.fuse(invalid, evidence);
    EXPECT_EQ(invalid_result.status().code(), ErrorCode::kInvalidArgument);
}

TEST(SessionFuseTest, CancelledAndExpiredContextsAreExplicitAndPublishNothing) {
    PerceptionSession session = make_session();
    const TestFrame frame = make_frame(40, 30, 1U);
    const EvidenceSet evidence = make_e2e_evidence();

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    EXPECT_EQ(session.fuse(frame.frame, evidence, {}, cancelled).status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(session.latest_snapshot(), nullptr);

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    EXPECT_EQ(session.fuse(frame.frame, evidence, {}, expired).status().code(), ErrorCode::kTimeout);
    EXPECT_EQ(session.latest_snapshot(), nullptr);
}

TEST(SessionFuseTest, InvalidTargetSpaceAndEvidenceBudgetsAreExplicit) {
    PerceptionSession session = make_session();
    const TestFrame frame = make_frame(100, 80, 1U);

    FusionOptions bad_space;
    bad_space.target_space = CoordinateSpaceId::kModelInput;
    EXPECT_EQ(session.fuse(frame.frame, make_e2e_evidence(), bad_space).status().code(), ErrorCode::kInvalidArgument);

    // Two disjoint clusters against a one-region budget: explicit, unpublished.
    EvidenceSet disjoint;
    ExternalRegion left;
    left.bounds = RectF{0.0F, 0.0F, 5.0F, 5.0F};
    ExternalRegion right;
    right.bounds = RectF{80.0F, 70.0F, 5.0F, 5.0F};
    ASSERT_TRUE(disjoint.add_external(left, CoordinateSpaceId::kOriented).ok());
    ASSERT_TRUE(disjoint.add_external(right, CoordinateSpaceId::kOriented).ok());
    FusionOptions tight;
    tight.max_regions = 1;
    const auto budget = session.fuse(frame.frame, disjoint, tight);
    ASSERT_FALSE(budget.ok());
    EXPECT_EQ(budget.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(session.latest_snapshot(), nullptr);
}

TEST(SessionFuseTest, EvidenceSetBudgetSurfacesAtAddTime) {
    EvidenceSet evidence;
    ExternalRegion tiny;
    tiny.bounds = RectF{0.0F, 0.0F, 1.0F, 1.0F};
    for (size_t index = 0; index < EvidenceSet::kMaxItems; ++index) {
        ASSERT_TRUE(evidence.add_external(tiny, CoordinateSpaceId::kOriented).ok());
    }
    const auto overflow = evidence.add_external(tiny, CoordinateSpaceId::kOriented);
    ASSERT_FALSE(overflow.ok());
    EXPECT_EQ(overflow.status().code(), ErrorCode::kBudgetExceeded);
}

TEST(SessionFuseTest, StableIdSpaceIsPerSession) {
    PerceptionSession first = make_session();
    PerceptionSession second = make_session();
    const TestFrame frame = make_frame(100, 80, 1U);
    const EvidenceSet evidence = make_e2e_evidence();
    ASSERT_TRUE(first.fuse(frame.frame, evidence).ok());
    ASSERT_TRUE(second.fuse(frame.frame, evidence).ok());
    // RULE-09: ids are unique within one session, not globally (both start at 1).
    EXPECT_EQ(first.last_stable_id(), 2U);
    EXPECT_EQ(second.last_stable_id(), 2U);
}

}  // namespace
