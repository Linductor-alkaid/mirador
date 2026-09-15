// M4 grid partition tests (M4-06): ceil semantics, edge-cell clipping,
// index/bounds validation and locate/image_point round trips (incl. edge
// points and cell centers).

#include <mirador/grid_partition.hpp>

#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::GridLocation;
using mirador::GridPartition;
using mirador::PointF;
using mirador::RectI;

GridPartition make_grid(int32_t width, int32_t height, int32_t side) {
    const auto grid = mirador::make_grid_partition(width, height, side);
    EXPECT_TRUE(grid.ok());
    return grid.value_or(GridPartition{});
}

// --- Partition shape ------------------------------------------------------------------

TEST(GridPartitionTest, ColumnsAndRowsUseCeilSemantics) {
    const GridPartition grid = make_grid(10, 7, 3);
    EXPECT_EQ(grid.image_width, 10);
    EXPECT_EQ(grid.image_height, 7);
    EXPECT_EQ(grid.cell_width, 3);
    EXPECT_EQ(grid.cell_height, 3);
    EXPECT_EQ(grid.columns, 4);  // ceil(10/3)
    EXPECT_EQ(grid.rows, 3);     // ceil(7/3)

    const GridPartition exact = make_grid(9, 6, 3);
    EXPECT_EQ(exact.columns, 3);
    EXPECT_EQ(exact.rows, 2);

    const GridPartition one_cell = make_grid(4, 4, 16);  // side larger than the image
    EXPECT_EQ(one_cell.columns, 1);
    EXPECT_EQ(one_cell.rows, 1);
}

TEST(GridPartitionTest, EdgeCellsAreClippedToTheImage) {
    const GridPartition grid = make_grid(10, 7, 3);
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 0, 0).value(), (RectI{0, 0, 3, 3}));
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 3, 0).value(), (RectI{9, 0, 1, 3}));  // clipped width
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 0, 2).value(), (RectI{0, 6, 3, 1}));  // clipped height
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 3, 2).value(), (RectI{9, 6, 1, 1}));  // corner cell
}

TEST(GridPartitionTest, InvalidDimensionsOrCellSideAreRejected) {
    EXPECT_EQ(mirador::make_grid_partition(0, 10, 4).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::make_grid_partition(10, 0, 4).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::make_grid_partition(-3, 10, 4).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::make_grid_partition(10, 10, 0).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::make_grid_partition(10, 10, -1).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::make_grid_partition(10, 10, 4097).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_TRUE(mirador::make_grid_partition(10, 10, 4096).ok());  // inclusive upper bound
    EXPECT_TRUE(mirador::make_grid_partition(10, 10, 1).ok());     // inclusive lower bound
}

TEST(GridPartitionTest, OutOfRangeCellIndicesAreRejected) {
    const GridPartition grid = make_grid(10, 7, 3);
    EXPECT_EQ(mirador::grid_cell_bounds(grid, -1, 0).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 0, -1).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::grid_cell_bounds(grid, grid.columns, 0).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 0, grid.rows).status().code(), ErrorCode::kInvalidArgument);
}

// --- Locate / image_point round trips ---------------------------------------------------

/// Expected outcome of one `grid_locate` call, table-driven.
struct LocateExpectation {
    PointF point;
    int32_t column;
    int32_t row;
    float local_x;
    float local_y;
};

void expect_locate(const GridPartition& grid, const LocateExpectation& expected) {
    const auto located = mirador::grid_locate(grid, expected.point);
    ASSERT_TRUE(located.ok());
    EXPECT_EQ(located.value().column, expected.column);
    EXPECT_EQ(located.value().row, expected.row);
    EXPECT_FLOAT_EQ(located.value().local.x, expected.local_x);
    EXPECT_FLOAT_EQ(located.value().local.y, expected.local_y);
}

TEST(GridPartitionTest, LocateAssignsCellsAndCellLocalCoordinates) {
    const GridPartition grid = make_grid(10, 7, 3);
    const std::vector<LocateExpectation> cases{
        {PointF{1.5F, 2.5F}, 0, 0, 1.5F, 2.5F},
        {PointF{3.0F, 3.0F}, 1, 1, 0.0F, 0.0F},  // boundary point belongs to the cell it faces
        {PointF{9.0F, 6.0F}, 3, 2, 0.0F, 0.0F},  // edge point (w-1, h-1): clipped corner cell
        {PointF{9.5F, 6.5F}, 3, 2, 0.5F, 0.5F},  // extreme interior point maps through the column clamp
    };
    for (const LocateExpectation& expected : cases) {
        expect_locate(grid, expected);
    }
}

TEST(GridPartitionTest, PointsOutsideTheImageAreRejected) {
    const GridPartition grid = make_grid(10, 7, 3);
    EXPECT_EQ(mirador::grid_locate(grid, PointF{-0.25F, 3.0F}).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::grid_locate(grid, PointF{10.0F, 3.0F}).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::grid_locate(grid, PointF{3.0F, -1.0F}).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(mirador::grid_locate(grid, PointF{3.0F, 7.0F}).status().code(), ErrorCode::kInvalidArgument);
}

TEST(GridPartitionTest, ImagePointOutOfRangeCellsAreRejected) {
    const GridPartition grid = make_grid(10, 7, 3);
    GridLocation bad;
    bad.column = grid.columns;
    bad.row = 0;
    bad.local = PointF{0.0F, 0.0F};
    EXPECT_EQ(mirador::grid_image_point(grid, bad).status().code(), ErrorCode::kInvalidArgument);

    GridLocation bad_row;
    bad_row.column = 0;
    bad_row.row = -1;
    EXPECT_EQ(mirador::grid_image_point(grid, bad_row).status().code(), ErrorCode::kInvalidArgument);
}

/// The located cell must map back to the original point within 1e-4 and
/// relocate into the same cell.
void expect_maps_back(const GridPartition& grid, const GridLocation& location, PointF point) {
    const auto restored = mirador::grid_image_point(grid, location);
    ASSERT_TRUE(restored.ok());
    EXPECT_NEAR(restored.value().x, point.x, 1e-4F);
    EXPECT_NEAR(restored.value().y, point.y, 1e-4F);
    const auto relocated = mirador::grid_locate(grid, restored.value());
    ASSERT_TRUE(relocated.ok());
    EXPECT_EQ(relocated.value().column, location.column);
    EXPECT_EQ(relocated.value().row, location.row);
}

/// locate -> image_point -> locate must reproduce the point and the same cell.
void expect_locate_roundtrip(const GridPartition& grid, PointF point) {
    const auto located = mirador::grid_locate(grid, point);
    ASSERT_TRUE(located.ok());
    expect_maps_back(grid, located.value(), point);
}

/// Probe points: corners, an extreme interior point and every cell's center
/// plus top-left corner.
std::vector<PointF> grid_probe_points(const GridPartition& grid) {
    std::vector<PointF> probes;
    probes.push_back(PointF{0.0F, 0.0F});
    probes.push_back(PointF{static_cast<float>(grid.image_width - 1), static_cast<float>(grid.image_height - 1)});
    probes.push_back(
        PointF{static_cast<float>(grid.image_width) - 0.25F, static_cast<float>(grid.image_height) - 0.5F});
    for (int32_t row = 0; row < grid.rows; ++row) {
        for (int32_t column = 0; column < grid.columns; ++column) {
            const RectI cell = mirador::grid_cell_bounds(grid, column, row).value();
            probes.push_back(PointF{static_cast<float>(cell.x) + static_cast<float>(cell.width) / 2.0F,
                                    static_cast<float>(cell.y) + static_cast<float>(cell.height) / 2.0F});
            probes.push_back(PointF{static_cast<float>(cell.x), static_cast<float>(cell.y)});
        }
    }
    return probes;
}

TEST(GridPartitionTest, LocateAndImagePointRoundTripWithinTolerance) {
    const GridPartition grid = make_grid(37, 23, 8);  // odd sizes, partial edge cells
    for (const PointF& point : grid_probe_points(grid)) {
        expect_locate_roundtrip(grid, point);
    }
}

TEST(GridPartitionTest, SingleCellGridRoundTripsEveryPoint) {
    const GridPartition grid = make_grid(5, 4, 64);  // one cell covering the image
    ASSERT_EQ(grid.columns, 1);
    ASSERT_EQ(grid.rows, 1);
    for (const PointF& point : std::vector<PointF>{{0.0F, 0.0F}, {2.5F, 2.0F}, {4.0F, 3.0F}}) {
        expect_locate_roundtrip(grid, point);
    }
    // The single cell is clipped to the image bounds.
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 0, 0).value(), (RectI{0, 0, 5, 4}));
}

}  // namespace
