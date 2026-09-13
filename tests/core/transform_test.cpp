#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using mirador::CoordinateSpaceId;
using mirador::LineSegment;
using mirador::PointF;
using mirador::RectF;
using mirador::Rotation;
using mirador::Transform2D;

using mirador::bounding_rect;
using mirador::compose;
using mirador::corners;
using mirador::inverse;
using mirador::make_crop;
using mirador::make_identity;
using mirador::make_letterbox;
using mirador::make_mirror;
using mirador::make_rotation;
using mirador::make_scale;
using mirador::oriented_size;
using mirador::transform_point;
using mirador::transform_points;
using mirador::transform_rect;
using mirador::transform_segment;

constexpr double kEps = 1e-6;
constexpr float kFloatEps = 1e-3F;

void expect_point_near(const PointF& actual, const PointF& expected, float tolerance = kFloatEps) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
}

TEST(Geometry, CornersAndBoundingRect) {
    const RectF rect{10.0F, 20.0F, 30.0F, 40.0F};
    const auto rect_corners = corners(rect);
    expect_point_near(rect_corners[0], {10.0F, 20.0F});
    expect_point_near(rect_corners[1], {40.0F, 20.0F});
    expect_point_near(rect_corners[2], {40.0F, 60.0F});
    expect_point_near(rect_corners[3], {10.0F, 60.0F});

    const RectF bounds = bounding_rect(rect_corners);
    EXPECT_NEAR(bounds.x, 10.0F, kEps);
    EXPECT_NEAR(bounds.y, 20.0F, kEps);
    EXPECT_NEAR(bounds.width, 30.0F, kEps);
    EXPECT_NEAR(bounds.height, 40.0F, kEps);
    const RectF empty = bounding_rect(std::span<const PointF>{});
    EXPECT_FLOAT_EQ(empty.width, 0.0F);
    EXPECT_FLOAT_EQ(empty.height, 0.0F);
}

TEST(Transform, IdentityLeavesPointsUnchanged) {
    const Transform2D identity = make_identity(CoordinateSpaceId::kFrame, CoordinateSpaceId::kFrame);
    const PointF mapped = transform_point(identity, PointF{12.5F, -3.25F});
    expect_point_near(mapped, {12.5F, -3.25F}, 0.0F);
}

TEST(Transform, ScaleMapsPointsAndRects) {
    const Transform2D scale = make_scale(2.0, 0.5, CoordinateSpaceId::kFrame, CoordinateSpaceId::kModelInput);
    expect_point_near(transform_point(scale, PointF{10.0F, 8.0F}), {20.0F, 4.0F}, 0.0F);
    const RectF mapped = transform_rect(scale, RectF{10.0F, 10.0F, 20.0F, 20.0F});
    EXPECT_NEAR(mapped.x, 20.0F, kEps);
    EXPECT_NEAR(mapped.y, 5.0F, kEps);
    EXPECT_NEAR(mapped.width, 40.0F, kEps);
    EXPECT_NEAR(mapped.height, 10.0F, kEps);
}

TEST(Transform, CropShiftsIntoLocalSpace) {
    const Transform2D crop =
        make_crop(RectF{10.0F, 20.0F, 30.0F, 40.0F}, CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
    expect_point_near(transform_point(crop, PointF{10.0F, 20.0F}), {0.0F, 0.0F}, 0.0F);
    const RectF local = transform_rect(crop, RectF{15.0F, 25.0F, 5.0F, 5.0F});
    EXPECT_NEAR(local.x, 5.0F, kEps);
    EXPECT_NEAR(local.y, 5.0F, kEps);
}

TEST(Transform, RotationDirectionsMatchDesignConventions) {
    // Raw frame 3 wide x 5 tall; area coordinates.
    const auto check_directions = [](Rotation rotation, const PointF& expected_top_left) {
        const Transform2D rotation_transform =
            make_rotation(rotation, 3, 5, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
        expect_point_near(transform_point(rotation_transform, PointF{0.0F, 0.0F}), expected_top_left, 0.0F);
    };
    check_directions(Rotation::k0, {0.0F, 0.0F});
    check_directions(Rotation::k90, {5.0F, 0.0F});   // top-left -> top-right (clockwise)
    check_directions(Rotation::k180, {3.0F, 5.0F});  // top-left -> bottom-right
    check_directions(Rotation::k270, {0.0F, 3.0F});  // top-left -> bottom-left
}

TEST(Transform, OrientedSizeSwapsForQuarterTurns) {
    EXPECT_EQ(oriented_size(Rotation::k0, 3, 5), std::make_pair(3, 5));
    EXPECT_EQ(oriented_size(Rotation::k90, 3, 5), std::make_pair(5, 3));
    EXPECT_EQ(oriented_size(Rotation::k180, 3, 5), std::make_pair(3, 5));
    EXPECT_EQ(oriented_size(Rotation::k270, 3, 5), std::make_pair(5, 3));
}

TEST(Transform, MirrorFlipsAroundCenterLines) {
    const Transform2D horizontal =
        make_mirror(true, false, 100, 50, CoordinateSpaceId::kFrame, CoordinateSpaceId::kFrame);
    expect_point_near(transform_point(horizontal, PointF{0.0F, 7.0F}), {100.0F, 7.0F}, 0.0F);
    const Transform2D both = make_mirror(true, true, 100, 50, CoordinateSpaceId::kFrame, CoordinateSpaceId::kFrame);
    expect_point_near(transform_point(both, PointF{0.0F, 0.0F}), {100.0F, 50.0F}, 0.0F);
}

TEST(Transform, LetterboxCentersAndPreservesAspect) {
    const auto letterbox =
        make_letterbox(100, 50, 100, 100, CoordinateSpaceId::kOriented, CoordinateSpaceId::kModelInput);
    ASSERT_TRUE(letterbox.ok());
    // Uniform scale 1.0, vertical padding 25.
    expect_point_near(transform_point(letterbox.value(), PointF{50.0F, 25.0F}), {50.0F, 50.0F}, kFloatEps);
    const RectF mapped = transform_rect(letterbox.value(), RectF{0.0F, 0.0F, 100.0F, 50.0F});
    EXPECT_NEAR(mapped.y, 25.0F, kEps);
    EXPECT_NEAR(mapped.height, 50.0F, kEps);
    EXPECT_NEAR(mapped.x, 0.0F, kEps);
    EXPECT_NEAR(mapped.width, 100.0F, kEps);
}

TEST(Transform, LetterboxRejectsNonPositiveSizes) {
    const auto result = make_letterbox(0, 50, 100, 100, CoordinateSpaceId::kOriented, CoordinateSpaceId::kModelInput);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), mirador::ErrorCode::kInvalidArgument);
}

TEST(Transform, ComposeChainsSpacesAndRejectsMismatches) {
    const Transform2D rotation =
        make_rotation(Rotation::k90, 100, 50, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
    const Transform2D crop =
        make_crop(RectF{0.0F, 0.0F, 50.0F, 100.0F}, CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
    const auto chained = compose(rotation, crop);
    ASSERT_TRUE(chained.ok());
    EXPECT_EQ(chained.value().from, CoordinateSpaceId::kFrame);
    EXPECT_EQ(chained.value().to, CoordinateSpaceId::kCropped);
    expect_point_near(transform_point(chained.value(), PointF{10.0F, 20.0F}),
                      transform_point(crop, transform_point(rotation, PointF{10.0F, 20.0F})), kEps);

    const Transform2D unrelated = make_scale(1.0, 1.0, CoordinateSpaceId::kDisplay, CoordinateSpaceId::kDisplay);
    const auto mismatch = compose(rotation, unrelated);
    ASSERT_FALSE(mismatch.ok());
    EXPECT_EQ(mismatch.status().code(), mirador::ErrorCode::kCoordinateTransform);
}

TEST(Transform, InverseRejectsSingularMatrices) {
    const Transform2D zero_scale = make_scale(0.0, 1.0, CoordinateSpaceId::kFrame, CoordinateSpaceId::kModelInput);
    const auto result = inverse(zero_scale);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), mirador::ErrorCode::kCoordinateTransform);
}

TEST(Transform, RectTransformIsCornerBoundingBox) {
    // A 30x40 rect rotated 90 degrees becomes 40x30 with swapped offsets.
    const Transform2D rotation =
        make_rotation(Rotation::k90, 100, 100, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
    const RectF mapped = transform_rect(rotation, RectF{10.0F, 20.0F, 30.0F, 40.0F});
    EXPECT_NEAR(mapped.x, 100.0F - 60.0F, kEps);  // y_raw in [20, 60) -> x_o = 100 - y
    EXPECT_NEAR(mapped.y, 10.0F, kEps);
    EXPECT_NEAR(mapped.width, 40.0F, kEps);
    EXPECT_NEAR(mapped.height, 30.0F, kEps);
}

TEST(Transform, PolygonAndSegmentCarryThrough) {
    const Transform2D scale = make_scale(2.0, 2.0, CoordinateSpaceId::kFrame, CoordinateSpaceId::kModelInput);
    const std::vector<PointF> polygon{PointF{1.0F, 2.0F}, PointF{3.0F, 4.0F}, PointF{5.0F, 6.0F}};
    const std::vector<PointF> mapped = transform_points(scale, polygon);
    ASSERT_EQ(mapped.size(), 3U);
    expect_point_near(mapped[2], {10.0F, 12.0F}, kEps);

    const LineSegment segment{PointF{1.0F, 1.0F}, PointF{4.0F, 4.0F}, 0.75F};
    const LineSegment mapped_segment = transform_segment(scale, segment);
    expect_point_near(mapped_segment.begin, {2.0F, 2.0F}, kEps);
    expect_point_near(mapped_segment.end, {8.0F, 8.0F}, kEps);
    EXPECT_NEAR(mapped_segment.confidence, 0.75F, kEps);
}

// Design section 7: coordinates are a function of geometry, never of memory layout.
// Views with identical dimensions but different (non-contiguous) strides must produce
// identical transforms and identical mapped coordinates.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest EXPECT_* macro expansion dominates the metric
TEST(Transform, CoordinateResultsAreStrideInvariant) {
    const RectF crop_rect{10.0F, 10.0F, 100.0F, 100.0F};
    RectF previous_mapped;
    bool first_iteration = true;
    const std::byte dummy_storage{};
    for (const int32_t stride_multiplier : {1, 2, 3}) {
        mirador::ImageView view;
        view.data = &dummy_storage;
        view.width = 200;
        view.height = 200;
        view.row_stride_bytes = static_cast<int64_t>(view.width) * 4 * stride_multiplier;
        view.format = mirador::PixelFormat::kRgba8;
        ASSERT_TRUE(mirador::validate(view).ok());

        const Transform2D crop = make_crop(crop_rect, CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
        const Transform2D scale = make_scale(0.5, 0.5, CoordinateSpaceId::kCropped, CoordinateSpaceId::kModelInput);
        const auto chained = compose(crop, scale);
        ASSERT_TRUE(chained.ok());
        const RectF mapped = transform_rect(chained.value(), RectF{20.0F, 30.0F, 40.0F, 50.0F});
        EXPECT_NEAR(mapped.x, 5.0F, kEps);
        EXPECT_NEAR(mapped.y, 10.0F, kEps);
        EXPECT_NEAR(mapped.width, 20.0F, kEps);
        EXPECT_NEAR(mapped.height, 25.0F, kEps);
        if (!first_iteration) {
            EXPECT_FLOAT_EQ(mapped.x, previous_mapped.x);
            EXPECT_FLOAT_EQ(mapped.y, previous_mapped.y);
        }
        previous_mapped = mapped;
        first_iteration = false;
    }
}

// Full pipeline: frame -> rotation -> crop -> letterbox, then recovery to frame space.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest EXPECT_* macro expansion dominates the metric
TEST(Transform, PreprocessingChainRecoversFrameCoordinates) {
    const int32_t raw_w = 1280;
    const int32_t raw_h = 720;
    const Transform2D rotation =
        make_rotation(Rotation::k90, raw_w, raw_h, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
    const RectF crop_rect{100.0F, 200.0F, 300.0F, 400.0F};  // in oriented space (720x1280)
    const Transform2D crop = make_crop(crop_rect, CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
    const auto letterbox =
        make_letterbox(300, 400, 320, 320, CoordinateSpaceId::kCropped, CoordinateSpaceId::kModelInput);
    ASSERT_TRUE(letterbox.ok());

    const auto rotation_to_cropped = compose(rotation, crop);
    ASSERT_TRUE(rotation_to_cropped.ok());
    const auto forward = compose(rotation_to_cropped.value(), letterbox.value());
    ASSERT_TRUE(forward.ok());
    EXPECT_EQ(forward.value().from, CoordinateSpaceId::kFrame);
    EXPECT_EQ(forward.value().to, CoordinateSpaceId::kModelInput);

    // Point inside the crop region, expressed in both spaces.
    const PointF oriented_point{150.0F, 250.0F};
    const PointF frame_point = transform_point(inverse(rotation).value(), oriented_point);
    expect_point_near(frame_point, {250.0F, 570.0F}, kEps);  // x_r = y_o, y_r = 720 - x_o

    // Forward: crop-local (50, 50); letterbox scale 0.8, pad_x 40, pad_y 0.
    const PointF model_point = transform_point(forward.value(), frame_point);
    expect_point_near(model_point, {80.0F, 40.0F}, kFloatEps);
    EXPECT_GE(model_point.x, 0.0F);
    EXPECT_GE(model_point.y, 0.0F);
    EXPECT_LE(model_point.x, 320.0F);
    EXPECT_LE(model_point.y, 320.0F);

    // The crop center lands at the scaled content center (scale 0.8, pad_x 40, pad_y 0).
    const PointF crop_center_model =
        transform_point(forward.value(), transform_point(inverse(rotation).value(), PointF{250.0F, 400.0F}));
    expect_point_near(crop_center_model, {160.0F, 160.0F}, kFloatEps);

    const auto backward = inverse(forward.value());
    ASSERT_TRUE(backward.ok());
    EXPECT_EQ(backward.value().from, CoordinateSpaceId::kModelInput);
    EXPECT_EQ(backward.value().to, CoordinateSpaceId::kFrame);
    const PointF frame_again = transform_point(backward.value(), model_point);
    expect_point_near(frame_again, frame_point, kFloatEps);
}

}  // namespace
