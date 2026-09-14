// M4 semantic snapshot tests (M4-02/03 foundations): RegionSource bitmask
// algebra, the free lookup/staleness helpers and component equality.

#include <mirador/semantic_snapshot.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/geometry.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using mirador::ChangeClassification;
using mirador::ChangeReport;
using mirador::CoordinateSpaceId;
using mirador::PointF;
using mirador::RectF;
using mirador::RegionSource;
using mirador::SemanticSnapshot;
using mirador::VisualRegion;

TEST(RegionSourceTest, BitmaskOperatorsComposeAndQuery) {
    const uint32_t combined = RegionSource::kExternal | RegionSource::kOcr;
    EXPECT_EQ(combined, 3U);
    EXPECT_TRUE(has_source(combined, RegionSource::kExternal));
    EXPECT_TRUE(has_source(combined, RegionSource::kOcr));
    EXPECT_FALSE(has_source(combined, RegionSource::kDetector));

    // Cache provenance composes on top of any source bit.
    const uint32_t cached = combined | RegionSource::kCache;
    EXPECT_TRUE(has_source(cached, RegionSource::kExternal));
    EXPECT_TRUE(has_source(cached, RegionSource::kCache));
    EXPECT_FALSE(has_source(cached, RegionSource::kTemplate));

    // operator& yields the shared bits; disjoint masks intersect to zero.
    EXPECT_EQ(combined & RegionSource::kOcr, static_cast<uint32_t>(RegionSource::kOcr));
    EXPECT_EQ(RegionSource::kDetector & RegionSource::kOcr, 0U);
    EXPECT_EQ(RegionSource::kNone | RegionSource::kNone, 0U);
    EXPECT_FALSE(has_source(0U, RegionSource::kExternal));
}

TEST(SemanticSnapshotTest, FindRegionReturnsMatchOrNullptr) {
    VisualRegion first;
    first.stable_id = 7U;
    first.bounds = RectF{0.0F, 0.0F, 5.0F, 5.0F};
    VisualRegion second;
    second.stable_id = 9U;

    SemanticSnapshot snapshot;
    snapshot.generation = 2U;
    snapshot.regions = {first, second};

    const mirador::VisualRegion* found = mirador::find_region(snapshot, 9U);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->stable_id, 9U);
    EXPECT_EQ(mirador::find_region(snapshot, 7U)->bounds, first.bounds);
    EXPECT_EQ(mirador::find_region(snapshot, 8U), nullptr);

    const SemanticSnapshot empty;
    EXPECT_EQ(mirador::find_region(empty, 1U), nullptr);
}

TEST(SemanticSnapshotTest, IsGenerationCurrentMatchesOnlyTheSnapshotGeneration) {
    SemanticSnapshot snapshot;
    snapshot.generation = 5U;
    EXPECT_TRUE(mirador::is_generation_current(snapshot, 5U));
    EXPECT_FALSE(mirador::is_generation_current(snapshot, 4U));
    EXPECT_FALSE(mirador::is_generation_current(snapshot, 6U));
}

TEST(SemanticSnapshotTest, ComponentEqualityComparesEveryField) {
    SemanticSnapshot base;
    base.frame_sequence = 3U;
    base.generation = 2U;
    base.coordinate_space = CoordinateSpaceId::kOriented;

    SemanticSnapshot same = base;
    EXPECT_TRUE(base == same);

    SemanticSnapshot other_space = base;
    other_space.coordinate_space = CoordinateSpaceId::kFrame;
    EXPECT_FALSE(base == other_space);

    SemanticSnapshot other_change = base;
    other_change.change.classification = ChangeClassification::kGlobal;
    EXPECT_FALSE(base == other_change);

    VisualRegion region;
    region.stable_id = 1U;
    VisualRegion region_copy = region;
    EXPECT_TRUE(region == region_copy);
    region_copy.confidence = 0.5F;
    EXPECT_FALSE(region == region_copy);
    region_copy = region;
    region_copy.evidence_ids = {1U};
    EXPECT_FALSE(region == region_copy);
}

}  // namespace
