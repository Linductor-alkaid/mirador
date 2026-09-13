#include <mirador/geometry.hpp>

#include <gtest/gtest.h>
#include <cstdint>

namespace {

using mirador::contains;
using mirador::intersect;
using mirador::is_valid;
using mirador::PointF;
using mirador::RectI;

TEST(RectI, ValidityRejectsNegativeSizeAndEdgeOverflow) {
    EXPECT_TRUE(is_valid(RectI{0, 0, 0, 0}));
    EXPECT_TRUE(is_valid(RectI{-5, -5, 10, 10}));
    EXPECT_TRUE(is_valid(RectI{INT32_MAX - 3, 0, 3, 4}));

    EXPECT_FALSE(is_valid(RectI{0, 0, -1, 4}));
    EXPECT_FALSE(is_valid(RectI{0, 0, 4, -1}));
    EXPECT_FALSE(is_valid(RectI{INT32_MAX - 3, 0, 4, 4}));  // right edge overflows
    EXPECT_FALSE(is_valid(RectI{0, INT32_MAX - 3, 4, 4}));  // bottom edge overflows
}

TEST(RectI, PointContainmentFollowsPixelAreaConvention) {
    const RectI rect{2, 3, 5, 4};
    EXPECT_TRUE(contains(rect, PointF{2.0F, 3.0F}));
    EXPECT_TRUE(contains(rect, PointF{6.9F, 6.9F}));
    EXPECT_FALSE(contains(rect, PointF{7.0F, 3.0F}));  // right edge is exclusive
    EXPECT_FALSE(contains(rect, PointF{2.0F, 7.0F}));  // bottom edge is exclusive
    EXPECT_FALSE(contains(rect, PointF{1.9F, 3.0F}));
    EXPECT_FALSE(contains(rect, PointF{2.0F, 2.9F}));

    EXPECT_FALSE(contains(RectI{0, 0, 0, 0}, PointF{0.0F, 0.0F}));  // empty rect
}

TEST(RectI, RectContainmentRequiresFullCoverage) {
    const RectI outer{0, 0, 10, 10};
    EXPECT_TRUE(contains(outer, RectI{0, 0, 10, 10}));
    EXPECT_TRUE(contains(outer, RectI{3, 4, 5, 6}));
    EXPECT_FALSE(contains(outer, RectI{-1, 0, 5, 5}));
    EXPECT_FALSE(contains(outer, RectI{8, 8, 3, 3}));
    EXPECT_FALSE(contains(RectI{0, 0, 5, 5}, RectI{0, 0, 10, 10}));
}

TEST(RectI, IntersectionComputesOverlapAndHandlesDisjoint) {
    const RectI a{0, 0, 10, 10};
    const RectI b{4, 6, 10, 10};
    const RectI overlap = intersect(a, b);
    EXPECT_EQ(overlap.x, 4);
    EXPECT_EQ(overlap.y, 6);
    EXPECT_EQ(overlap.width, 6);
    EXPECT_EQ(overlap.height, 4);

    const RectI disjoint = intersect(RectI{0, 0, 2, 2}, RectI{5, 5, 2, 2});
    EXPECT_EQ(disjoint.width, 0);
    EXPECT_EQ(disjoint.height, 0);
    EXPECT_EQ(disjoint.x, 5);
    EXPECT_EQ(disjoint.y, 5);

    // Touching edges share a zero-area boundary strip.
    const RectI touching = intersect(RectI{0, 0, 4, 4}, RectI{4, 0, 4, 4});
    EXPECT_EQ(touching.width, 0);
    EXPECT_EQ(touching.height, 4);
}

TEST(RectI, IntersectionStaysExactAtInt32Extremes) {
    // Edges span the full int32 range; the int64 edge arithmetic must not overflow
    // (UBSAN) and must report the geometrically correct empty overlap.
    const RectI huge{INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};  // [INT32_MIN, -1)
    const RectI far_away{INT32_MAX - 2, INT32_MAX - 2, 2, 2};      // [INT32_MAX - 2, INT32_MAX)
    const RectI overlap = intersect(huge, far_away);
    EXPECT_EQ(overlap.width, 0);
    EXPECT_EQ(overlap.height, 0);

    // Rect containment at the same extremes follows int64 edges too.
    EXPECT_TRUE(contains(huge, RectI{INT32_MIN, INT32_MIN, 4, 4}));
    EXPECT_FALSE(contains(huge, RectI{0, 0, 4, 4}));
}

}  // namespace
