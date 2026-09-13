#include <mirador/geometry.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>

namespace {

using mirador::CoordinateSpaceId;
using mirador::PointF;
using mirador::RectF;
using mirador::Rotation;
using mirador::Transform2D;

using mirador::compose;
using mirador::inverse;
using mirador::make_crop;
using mirador::make_letterbox;
using mirador::make_rotation;
using mirador::make_scale;
using mirador::transform_point;
using mirador::transform_rect;

// Design section 7 / plan DOD-03: any legal crop/scale/rotation preprocessing chain
// must map the full frame strictly into the target bounds and round-trip within a
// tight tolerance. Deterministic seeding keeps failures reproducible.
constexpr float kRoundTripTolerance = 1e-3F;
constexpr double kBoundsEpsilon = 1e-3;

// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest EXPECT_* macro expansion dominates the metric
TEST(TransformProperty, LegalChainsStayInBoundsAndRoundTrip) {
    std::mt19937 rng(20260913U);
    std::uniform_int_distribution<int32_t> dim(1, 4096);
    std::uniform_real_distribution<float> ratio(0.0F, 1.0F);
    std::uniform_real_distribution<double> scale(0.25, 4.0);
    std::uniform_int_distribution<int32_t> letterbox_dim(1, 2048);
    std::uniform_int_distribution<int> rotation_index(0, 3);
    const std::array<const Rotation, 4> rotations{Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270};

    for (int iteration = 0; iteration < 200; ++iteration) {
        const int32_t raw_w = dim(rng);
        const int32_t raw_h = dim(rng);
        const Rotation rotation = rotations[rotation_index(rng)];

        // Legal crop in oriented space.
        const auto [oriented_w, oriented_h] = mirador::oriented_size(rotation, raw_w, raw_h);
        const float crop_x = ratio(rng) * static_cast<float>(oriented_w) / 2.0F;
        const float crop_y = ratio(rng) * static_cast<float>(oriented_h) / 2.0F;
        const float crop_w = std::max(1.0F, ratio(rng) * static_cast<float>(oriented_w) - crop_x);
        const float crop_h = std::max(1.0F, ratio(rng) * static_cast<float>(oriented_h) - crop_y);
        const RectF crop_rect{crop_x, crop_y, crop_w, crop_h};

        const double sx = scale(rng);
        const double sy = scale(rng);

        const Transform2D rotation_transform =
            make_rotation(rotation, raw_w, raw_h, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
        const Transform2D crop_transform =
            make_crop(crop_rect, CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
        const Transform2D scale_transform =
            make_scale(sx, sy, CoordinateSpaceId::kCropped, CoordinateSpaceId::kModelInput);

        const auto first_leg = compose(rotation_transform, crop_transform);
        ASSERT_TRUE(first_leg.ok()) << "iteration " << iteration;
        const auto forward = compose(first_leg.value(), scale_transform);
        ASSERT_TRUE(forward.ok()) << "iteration " << iteration;
        const auto backward = inverse(forward.value());
        ASSERT_TRUE(backward.ok()) << "iteration " << iteration;

        // 1a. The full frame maps strictly into the oriented view bounds.
        const RectF full_frame{0.0F, 0.0F, static_cast<float>(raw_w), static_cast<float>(raw_h)};
        const RectF oriented_full = transform_rect(rotation_transform, full_frame);
        EXPECT_GE(oriented_full.x, -kBoundsEpsilon) << "iteration " << iteration;
        EXPECT_GE(oriented_full.y, -kBoundsEpsilon) << "iteration " << iteration;
        EXPECT_LE(oriented_full.x + oriented_full.width, static_cast<double>(oriented_w) + kBoundsEpsilon)
            << "iteration " << iteration;
        EXPECT_LE(oriented_full.y + oriented_full.height, static_cast<double>(oriented_h) + kBoundsEpsilon)
            << "iteration " << iteration;
        EXPECT_TRUE(std::isfinite(oriented_full.x) && std::isfinite(oriented_full.y)) << "iteration " << iteration;

        // 1b. The cropped region maps exactly into the scaled crop tile: the pre-image
        // of the crop in frame space (axis-aligned rotations keep rects exact) must
        // land inside [0, final_w] x [0, final_h] under the full forward chain.
        const auto rotation_back = inverse(rotation_transform);
        ASSERT_TRUE(rotation_back.ok()) << "iteration " << iteration;
        const RectF crop_pre_image = transform_rect(rotation_back.value(), crop_rect);
        const double final_w = static_cast<double>(crop_w) * sx;
        const double final_h = static_cast<double>(crop_h) * sy;
        const RectF mapped = transform_rect(forward.value(), crop_pre_image);
        EXPECT_GE(mapped.x, -kBoundsEpsilon) << "iteration " << iteration;
        EXPECT_GE(mapped.y, -kBoundsEpsilon) << "iteration " << iteration;
        EXPECT_LE(mapped.x + mapped.width, final_w + kBoundsEpsilon) << "iteration " << iteration;
        EXPECT_LE(mapped.y + mapped.height, final_h + kBoundsEpsilon) << "iteration " << iteration;

        // 2. Random sample points round-trip through the inverse chain.
        for (int sample = 0; sample < 8; ++sample) {
            const float px = ratio(rng) * static_cast<float>(raw_w);
            const float py = ratio(rng) * static_cast<float>(raw_h);
            const PointF forward_point = transform_point(forward.value(), PointF{px, py});
            const PointF round_trip = transform_point(backward.value(), forward_point);
            EXPECT_NEAR(round_trip.x, px, kRoundTripTolerance) << "iteration " << iteration;
            EXPECT_NEAR(round_trip.y, py, kRoundTripTolerance) << "iteration " << iteration;
        }

        // 3. A letterbox variant must map the oriented image into the destination.
        const int32_t dst_w = letterbox_dim(rng);
        const int32_t dst_h = letterbox_dim(rng);
        const auto letterbox = make_letterbox(oriented_w, oriented_h, dst_w, dst_h, CoordinateSpaceId::kOriented,
                                              CoordinateSpaceId::kModelInput);
        ASSERT_TRUE(letterbox.ok()) << "iteration " << iteration;
        const RectF oriented_image{0.0F, 0.0F, static_cast<float>(oriented_w), static_cast<float>(oriented_h)};
        const RectF letterboxed = transform_rect(letterbox.value(), oriented_image);
        EXPECT_GE(letterboxed.x, -kBoundsEpsilon) << "iteration " << iteration;
        EXPECT_GE(letterboxed.y, -kBoundsEpsilon) << "iteration " << iteration;
        EXPECT_LE(letterboxed.x + letterboxed.width, static_cast<double>(dst_w) + kBoundsEpsilon)
            << "iteration " << iteration;
        EXPECT_LE(letterboxed.y + letterboxed.height, static_cast<double>(dst_h) + kBoundsEpsilon)
            << "iteration " << iteration;
        const auto letterbox_back = inverse(letterbox.value());
        ASSERT_TRUE(letterbox_back.ok());
        const PointF center{static_cast<float>(oriented_w) / 2.0F, static_cast<float>(oriented_h) / 2.0F};
        const PointF recovered = transform_point(letterbox_back.value(), transform_point(letterbox.value(), center));
        EXPECT_NEAR(recovered.x, center.x, kRoundTripTolerance) << "iteration " << iteration;
        EXPECT_NEAR(recovered.y, center.y, kRoundTripTolerance) << "iteration " << iteration;
    }
}

}  // namespace
