// M5-05 kDisplay coordinate-space fusion tests (DEC-016): the full
// item/target space conversion matrix through
// `FusionOptions::display_transform`, rotation-composed round trips
// (DOD-03), a PerceptionSession fusion publishing a kDisplay snapshot with
// stable ids, the explicit negative paths (missing / reversed / non-finite /
// singular display transforms, unsupported target spaces) and the EvidenceSet
// container accepting kDisplay items.

#include <mirador/fusion.hpp>
#include <mirador/perception_session.hpp>

#include <mirador/detector_backend.hpp>
#include <mirador/evidence.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::CoordinateSpaceId;
using mirador::DetectionRegion;
using mirador::ErrorCode;
using mirador::EvidenceItem;
using mirador::EvidenceSet;
using mirador::ExternalRegion;
using mirador::Frame;
using mirador::FusionOptions;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::RegionSource;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::TextRegion;
using mirador::Transform2D;
using mirador::VisualRegion;

constexpr const char* kSourceId = "screen-main";

/// Fixture mapping (DEC-016): oriented -> display is `p_display = 2 * p_oriented
/// + (10, 5)` — a scale(2, 2) followed by a translation.
Transform2D make_display_transform() {
    const Transform2D scale = mirador::make_scale(2.0, 2.0, CoordinateSpaceId::kOriented, CoordinateSpaceId::kOriented);
    const Transform2D shift =
        mirador::make_translation(10.0, 5.0, CoordinateSpaceId::kOriented, CoordinateSpaceId::kDisplay);
    return mirador::compose(scale, shift).take_value();
}

/// Minimal owned frame; `fuse_evidence` reads rotation/dimensions only, but the
/// buffer is real so the fixture stays valid for any consumer.
struct TestFrame {
    std::vector<std::byte> bytes;
    Frame frame;
};

TestFrame make_fusion_frame(int32_t width, int32_t height, Rotation rotation) {
    TestFrame image;
    const int64_t stride = static_cast<int64_t>(width) * 1;  // gray8
    image.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{0});
    image.frame.image.data = image.bytes.data();
    image.frame.image.width = width;
    image.frame.image.height = height;
    image.frame.image.row_stride_bytes = stride;
    image.frame.image.format = PixelFormat::kGray8;
    image.frame.image.rotation = rotation;
    image.frame.sequence = 1;
    return image;
}

TestFrame make_session_frame(int32_t width, int32_t height, uint64_t sequence) {
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
    image.frame.source_id = kSourceId;
    image.frame.owner = std::make_shared<const std::vector<std::byte>>(image.bytes);
    return image;
}

TextRegion make_text(float x, float y, float width, float height, std::string text, float confidence = 0.9F) {
    TextRegion region;
    region.bounds = RectF{x, y, width, height};
    region.utf8_text = std::move(text);
    region.confidence = confidence;
    return region;
}

ExternalRegion make_external(float x, float y, float width, float height) {
    ExternalRegion region;
    region.bounds = RectF{x, y, width, height};
    region.text = "OK";
    region.role = "button";
    region.confidence = 1.0F;
    return region;
}

void expect_rect_near(const RectF& actual, double x, double y, double width, double height, double tolerance) {
    EXPECT_NEAR(actual.x, x, tolerance);
    EXPECT_NEAR(actual.y, y, tolerance);
    EXPECT_NEAR(actual.width, width, tolerance);
    EXPECT_NEAR(actual.height, height, tolerance);
}

/// DOD-03 round-trip tolerance: 1e-6 relative (at least 1e-6 absolute).
void expect_rect_near_relative(const RectF& actual, const RectF& expected) {
    const double x_tolerance = 1e-6 * std::max(1.0, std::fabs(static_cast<double>(expected.x)));
    const double y_tolerance = 1e-6 * std::max(1.0, std::fabs(static_cast<double>(expected.y)));
    const double width_tolerance = 1e-6 * std::max(1.0, std::fabs(static_cast<double>(expected.width)));
    const double height_tolerance = 1e-6 * std::max(1.0, std::fabs(static_cast<double>(expected.height)));
    EXPECT_NEAR(actual.x, expected.x, x_tolerance);
    EXPECT_NEAR(actual.y, expected.y, y_tolerance);
    EXPECT_NEAR(actual.width, expected.width, width_tolerance);
    EXPECT_NEAR(actual.height, expected.height, height_tolerance);
}

// --- The 3x3 item/target space conversion matrix -----------------------------------

TEST(DisplaySpaceFusionTest, NineSpaceCombinationsConvertThroughDisplayTransform) {
    // Rotation k0 keeps kFrame and kOriented coordinates equal, so every
    // expected value is exact: the oriented rect {1,2,3,4} maps through the
    // fixture transform to the display rect {12,9,6,8} (and back by its
    // inverse). Each fusion carries a single evidence item, so no merging
    // interferes with the numbers.
    const Transform2D display = make_display_transform();
    struct Case {
        CoordinateSpaceId item_space;
        CoordinateSpaceId target_space;
        RectF evidence;
        double x;
        double y;
        double width;
        double height;
    };
    const std::vector<Case> cases{
        {CoordinateSpaceId::kFrame, CoordinateSpaceId::kFrame, RectF{1.0F, 2.0F, 3.0F, 4.0F}, 1.0, 2.0, 3.0, 4.0},
        {CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented, RectF{1.0F, 2.0F, 3.0F, 4.0F}, 1.0, 2.0, 3.0, 4.0},
        {CoordinateSpaceId::kFrame, CoordinateSpaceId::kDisplay, RectF{1.0F, 2.0F, 3.0F, 4.0F}, 12.0, 9.0, 6.0, 8.0},
        {CoordinateSpaceId::kOriented, CoordinateSpaceId::kFrame, RectF{1.0F, 2.0F, 3.0F, 4.0F}, 1.0, 2.0, 3.0, 4.0},
        {CoordinateSpaceId::kOriented, CoordinateSpaceId::kOriented, RectF{1.0F, 2.0F, 3.0F, 4.0F}, 1.0, 2.0, 3.0, 4.0},
        {CoordinateSpaceId::kOriented, CoordinateSpaceId::kDisplay, RectF{1.0F, 2.0F, 3.0F, 4.0F}, 12.0, 9.0, 6.0, 8.0},
        {CoordinateSpaceId::kDisplay, CoordinateSpaceId::kFrame, RectF{12.0F, 9.0F, 6.0F, 8.0F}, 1.0, 2.0, 3.0, 4.0},
        {CoordinateSpaceId::kDisplay, CoordinateSpaceId::kOriented, RectF{12.0F, 9.0F, 6.0F, 8.0F}, 1.0, 2.0, 3.0, 4.0},
        {CoordinateSpaceId::kDisplay, CoordinateSpaceId::kDisplay, RectF{12.0F, 9.0F, 6.0F, 8.0F}, 12.0, 9.0, 6.0, 8.0},
    };

    for (const Case& item_case : cases) {
        SCOPED_TRACE(testing::Message() << "item=" << static_cast<int>(item_case.item_space)
                                        << " target=" << static_cast<int>(item_case.target_space));
        EvidenceSet evidence;
        ASSERT_TRUE(evidence
                        .add_text(make_text(item_case.evidence.x, item_case.evidence.y, item_case.evidence.width,
                                            item_case.evidence.height, "t"),
                                  item_case.item_space)
                        .ok());
        const TestFrame frame = make_fusion_frame(16, 16, Rotation::k0);

        FusionOptions options;
        options.target_space = item_case.target_space;
        options.display_transform = display;
        const auto fused = mirador::fuse_evidence(evidence, frame.frame, options);
        ASSERT_TRUE(fused.ok());
        ASSERT_EQ(fused.value().regions.size(), 1U);
        expect_rect_near(fused.value().regions[0].bounds, item_case.x, item_case.y, item_case.width, item_case.height,
                         1e-9);
    }
}

TEST(DisplaySpaceFusionTest, FrameOrientedConversionsDoNotRequireDisplayTransform) {
    // DEC-016 scope guard: conversions purely inside the kFrame/kOriented pair
    // stay legal without a display_transform (raw 5x3 capture rotated k90).
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(1.0F, 0.5F, 2.0F, 1.0F, "t"), CoordinateSpaceId::kFrame).ok());
    const TestFrame frame = make_fusion_frame(3, 5, Rotation::k90);

    FusionOptions options;
    options.target_space = CoordinateSpaceId::kOriented;  // no display_transform
    const auto fused = mirador::fuse_evidence(evidence, frame.frame, options);
    ASSERT_TRUE(fused.ok());
    ASSERT_EQ(fused.value().regions.size(), 1U);
    // k90 maps x_o = h - y_r, y_o = x_r: {1, 0.5, 2, 1} -> {1.5, 1, 1, 2}.
    expect_rect_near(fused.value().regions[0].bounds, 1.5, 1.0, 1.0, 2.0, 1e-9);
}

// --- Rotation composition and round trips (DOD-03) ----------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest EXPECT_* macro expansion dominates the metric
TEST(DisplaySpaceFusionTest, DisplayEvidenceRoundTripsThroughAllRotations) {
    // Raw 5x3 capture in all four orientations. The kDisplay evidence rect is
    // derived from the raw-frame rect {1, 0.5, 2, 1} through the public
    // rotation and display transforms; fusing it back to kOriented / kFrame
    // must reproduce those rects, and re-applying the forward chain must
    // recover the original display coordinates. For k90: {1, 0.5, 2, 1} ->
    // oriented {1.5, 1, 1, 2} -> display {13, 7, 2, 4}.
    const Transform2D display = make_display_transform();
    constexpr int32_t kRawWidth = 5;
    constexpr int32_t kRawHeight = 3;
    const RectF frame_rect{1.0F, 0.5F, 2.0F, 1.0F};

    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        SCOPED_TRACE(testing::Message() << "rotation=" << static_cast<int>(rotation));
        const auto [view_width, view_height] = mirador::oriented_size(rotation, kRawWidth, kRawHeight);
        const TestFrame frame = make_fusion_frame(view_width, view_height, rotation);
        const Transform2D frame_to_oriented = mirador::make_rotation(
            rotation, kRawWidth, kRawHeight, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
        const RectF oriented_rect = mirador::transform_rect(frame_to_oriented, frame_rect);
        const RectF display_rect = mirador::transform_rect(display, oriented_rect);

        // kDisplay evidence fused to kOriented...
        EvidenceSet oriented_evidence;
        ASSERT_TRUE(
            oriented_evidence
                .add_text(make_text(display_rect.x, display_rect.y, display_rect.width, display_rect.height, "t"),
                          CoordinateSpaceId::kDisplay)
                .ok());
        FusionOptions to_oriented;
        to_oriented.target_space = CoordinateSpaceId::kOriented;
        to_oriented.display_transform = display;
        const auto fused_oriented = mirador::fuse_evidence(oriented_evidence, frame.frame, to_oriented);
        ASSERT_TRUE(fused_oriented.ok());
        ASSERT_EQ(fused_oriented.value().regions.size(), 1U);
        const RectF recovered_oriented = fused_oriented.value().regions[0].bounds;
        expect_rect_near_relative(recovered_oriented, oriented_rect);
        // ...and the forward display transform recovers the original coords.
        expect_rect_near_relative(mirador::transform_rect(display, recovered_oriented), display_rect);

        // The same kDisplay evidence fused to kFrame...
        EvidenceSet frame_evidence;
        ASSERT_TRUE(
            frame_evidence
                .add_text(make_text(display_rect.x, display_rect.y, display_rect.width, display_rect.height, "t"),
                          CoordinateSpaceId::kDisplay)
                .ok());
        FusionOptions to_frame;
        to_frame.target_space = CoordinateSpaceId::kFrame;
        to_frame.display_transform = display;
        const auto fused_frame = mirador::fuse_evidence(frame_evidence, frame.frame, to_frame);
        ASSERT_TRUE(fused_frame.ok());
        ASSERT_EQ(fused_frame.value().regions.size(), 1U);
        const RectF recovered_frame = fused_frame.value().regions[0].bounds;
        expect_rect_near_relative(recovered_frame, frame_rect);
        // ...and rotation + display transform chain back to display space.
        const RectF chain_oriented = mirador::transform_rect(frame_to_oriented, recovered_frame);
        expect_rect_near_relative(chain_oriented, oriented_rect);
        expect_rect_near_relative(mirador::transform_rect(display, chain_oriented), display_rect);
    }
}

// --- Session end-to-end: a kDisplay snapshot -----------------------------------------

TEST(DisplaySpaceSessionTest, FusePublishesDisplaySpaceSnapshotWithStableIds) {
    PerceptionSessionOptions session_options;
    session_options.source_id = kSourceId;
    PerceptionSession session = PerceptionSession::create(std::move(session_options)).take_value();
    const TestFrame frame = make_session_frame(100, 80, 11U);

    EvidenceSet evidence;
    // Accessibility button given directly in display space; it is the display
    // image of the oriented rect {0, 0, 100, 100} through the fixture transform.
    ASSERT_TRUE(evidence.add_external(make_external(10.0F, 5.0F, 200.0F, 200.0F), CoordinateSpaceId::kDisplay).ok());
    // OCR text in oriented space; its display image is {20, 15, 180, 180},
    // overlapping the button with IoU 32400/40000 = 0.81 >= 0.5 (merge).
    ASSERT_TRUE(evidence.add_text(make_text(5.0F, 5.0F, 90.0F, 90.0F, "fake"), CoordinateSpaceId::kOriented).ok());

    FusionOptions options;
    options.target_space = CoordinateSpaceId::kDisplay;
    options.display_transform = make_display_transform();
    const auto fused = session.fuse(frame.frame, evidence, options);
    ASSERT_TRUE(fused.ok());
    const SemanticSnapshot& snapshot = fused.value();

    EXPECT_EQ(snapshot.frame_sequence, 11U);
    EXPECT_EQ(snapshot.generation, 1U);  // first publication is generation 1
    EXPECT_EQ(snapshot.coordinate_space, CoordinateSpaceId::kDisplay);

    ASSERT_EQ(snapshot.regions.size(), 1U);
    const VisualRegion& merged = snapshot.regions[0];
    EXPECT_EQ(merged.stable_id, 1U);  // stable ids are assigned by the session
    // Union of the display rects {10,5,200,200} and {20,15,180,180}, i.e. the
    // merged region lives in display coordinates.
    expect_rect_near(merged.bounds, 10.0, 5.0, 200.0, 200.0, 1e-9);
    EXPECT_NEAR(merged.anchor.x, 110.0, 1e-5);
    EXPECT_NEAR(merged.anchor.y, 105.0, 1e-5);
    EXPECT_EQ(merged.text, "OK\nfake");
    EXPECT_EQ(merged.label, "button");
    EXPECT_EQ(merged.evidence_ids, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(merged.source_mask,
              static_cast<uint32_t>(RegionSource::kExternal) | static_cast<uint32_t>(RegionSource::kOcr));

    // The single-region count plus the union bounds above prove the IoU
    // association happened across the two input spaces (kDisplay external +
    // kOriented text); the session snapshot itself carries no merge trace.
    EXPECT_EQ(session.last_stable_id(), 1U);
    ASSERT_NE(session.latest_snapshot(), nullptr);
    EXPECT_TRUE(*session.latest_snapshot() == snapshot);
}

// --- Negative paths -------------------------------------------------------------------

TEST(DisplaySpaceFusionTest, MissingDisplayTransformIsRejected) {
    const TestFrame frame = make_fusion_frame(16, 16, Rotation::k0);

    // Target kDisplay without a display_transform.
    EvidenceSet oriented_evidence;
    ASSERT_TRUE(oriented_evidence.add_text(make_text(1.0F, 2.0F, 3.0F, 4.0F, "t")).ok());
    FusionOptions to_display;
    to_display.target_space = CoordinateSpaceId::kDisplay;
    EXPECT_EQ(mirador::fuse_evidence(oriented_evidence, frame.frame, to_display).status().code(),
              ErrorCode::kInvalidArgument);

    // kDisplay evidence without a display_transform (target kOriented).
    EvidenceSet display_evidence;
    ASSERT_TRUE(display_evidence.add_text(make_text(12.0F, 9.0F, 6.0F, 8.0F, "t"), CoordinateSpaceId::kDisplay).ok());
    FusionOptions to_oriented;
    to_oriented.target_space = CoordinateSpaceId::kOriented;
    EXPECT_EQ(mirador::fuse_evidence(display_evidence, frame.frame, to_oriented).status().code(),
              ErrorCode::kInvalidArgument);

    // item == target == kDisplay still requires the transform (DEC-016: every
    // kDisplay fusion is self-describing, even without numeric conversion).
    FusionOptions display_to_display;
    display_to_display.target_space = CoordinateSpaceId::kDisplay;
    EXPECT_EQ(mirador::fuse_evidence(display_evidence, frame.frame, display_to_display).status().code(),
              ErrorCode::kInvalidArgument);
}

TEST(DisplaySpaceFusionTest, WronglyDirectedAndNonFiniteTransformsAreRejected) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(1.0F, 2.0F, 3.0F, 4.0F, "t")).ok());
    const TestFrame frame = make_fusion_frame(16, 16, Rotation::k0);

    // Reversed direction: the contract freezes from == kOriented, to == kDisplay.
    Transform2D reversed;
    reversed.from = CoordinateSpaceId::kDisplay;
    reversed.to = CoordinateSpaceId::kOriented;
    FusionOptions wrong_direction;
    wrong_direction.target_space = CoordinateSpaceId::kDisplay;
    wrong_direction.display_transform = reversed;
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, wrong_direction).status().code(),
              ErrorCode::kInvalidArgument);

    // Non-finite matrix entry.
    Transform2D with_nan = make_display_transform();
    with_nan.matrix[2] = std::nan("");
    FusionOptions non_finite;
    non_finite.target_space = CoordinateSpaceId::kOriented;
    non_finite.display_transform = with_nan;
    EvidenceSet display_evidence;
    ASSERT_TRUE(display_evidence.add_text(make_text(12.0F, 9.0F, 6.0F, 8.0F, "t"), CoordinateSpaceId::kDisplay).ok());
    EXPECT_EQ(mirador::fuse_evidence(display_evidence, frame.frame, non_finite).status().code(),
              ErrorCode::kInvalidArgument);
}

TEST(DisplaySpaceFusionTest, SingularDisplayTransformSurfacesAtInversion) {
    // scale(0, 0) is finite and correctly directed, so validation passes; the
    // singular matrix surfaces when the fusion actually inverts it
    // (kDisplay -> kOriented).
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(12.0F, 9.0F, 6.0F, 8.0F, "t"), CoordinateSpaceId::kDisplay).ok());
    const TestFrame frame = make_fusion_frame(16, 16, Rotation::k0);

    FusionOptions options;
    options.target_space = CoordinateSpaceId::kOriented;
    options.display_transform =
        mirador::make_scale(0.0, 0.0, CoordinateSpaceId::kOriented, CoordinateSpaceId::kDisplay);
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, options).status().code(), ErrorCode::kCoordinateTransform);
}

TEST(DisplaySpaceFusionTest, CroppedTargetSpaceIsRejected) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_text(make_text(1.0F, 2.0F, 3.0F, 4.0F, "t")).ok());
    const TestFrame frame = make_fusion_frame(16, 16, Rotation::k0);

    FusionOptions options;
    options.target_space = CoordinateSpaceId::kCropped;
    options.display_transform = make_display_transform();
    EXPECT_EQ(mirador::fuse_evidence(evidence, frame.frame, options).status().code(), ErrorCode::kInvalidArgument);
}

TEST(DisplaySpaceFusionTest, EmptyEvidenceSetWithDisplayTargetRequiresTransform) {
    // DEC-016: a kDisplay target space requires display_transform even when
    // the evidence set is empty — the requirement rides on the options (checked
    // in validate_options), not on individual items, so every kDisplay fusion
    // stays self-describing.
    const TestFrame frame = make_fusion_frame(16, 16, Rotation::k0);
    const EvidenceSet empty;

    FusionOptions without_transform;
    without_transform.target_space = CoordinateSpaceId::kDisplay;
    const auto rejected = mirador::fuse_evidence(empty, frame.frame, without_transform);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument);

    // Control: with the transform present, an empty set fuses to an empty
    // kDisplay output instead of an error.
    FusionOptions with_transform;
    with_transform.target_space = CoordinateSpaceId::kDisplay;
    with_transform.display_transform = make_display_transform();
    const auto accepted = mirador::fuse_evidence(empty, frame.frame, with_transform);
    ASSERT_TRUE(accepted.ok());
    EXPECT_TRUE(accepted.value().regions.empty());
    EXPECT_EQ(accepted.value().trace.input_evidence_count, 0U);
}

// --- EvidenceSet container contract -----------------------------------------------------

TEST(DisplaySpaceEvidenceTest, AllEvidenceKindsAreAcceptedInDisplaySpace) {
    EvidenceSet evidence;
    ASSERT_TRUE(evidence.add_external(make_external(1.0F, 2.0F, 3.0F, 4.0F), CoordinateSpaceId::kDisplay).ok());
    ASSERT_TRUE(evidence.add_text(make_text(1.0F, 2.0F, 3.0F, 4.0F, "t"), CoordinateSpaceId::kDisplay, true).ok());
    DetectionRegion detection;
    detection.bounds = RectF{1.0F, 2.0F, 3.0F, 4.0F};
    detection.class_id = 7;
    detection.label = "icon";
    detection.confidence = 0.25F;
    ASSERT_TRUE(evidence.add_detection(detection, CoordinateSpaceId::kDisplay).ok());
    ASSERT_TRUE(evidence.add_template(42U, RectF{1.0F, 2.0F, 3.0F, 4.0F}, 0.5, CoordinateSpaceId::kDisplay, true).ok());

    ASSERT_EQ(evidence.size(), 4U);
    for (const EvidenceItem& item : evidence.items()) {
        EXPECT_EQ(item.space, CoordinateSpaceId::kDisplay);
    }
    EXPECT_EQ(evidence.items()[3].template_entry_id, 42U);
}

}  // namespace
