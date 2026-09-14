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

TEST(GridPartitionTest, LocateAssignsCellsAndCellLocalCoordinates) {
    const GridPartition grid = make_grid(10, 7, 3);

    auto located = mirador::grid_locate(grid, PointF{1.5F, 2.5F});
    ASSERT_TRUE(located.ok());
    EXPECT_EQ(located.value().column, 0);
    EXPECT_EQ(located.value().row, 0);
    EXPECT_FLOAT_EQ(located.value().local.x, 1.5F);
    EXPECT_FLOAT_EQ(located.value().local.y, 2.5F);

    // A point exactly on a cell boundary belongs to the cell it faces.
    located = mirador::grid_locate(grid, PointF{3.0F, 3.0F});
    ASSERT_TRUE(located.ok());
    EXPECT_EQ(located.value().column, 1);
    EXPECT_EQ(located.value().row, 1);
    EXPECT_FLOAT_EQ(located.value().local.x, 0.0F);
    EXPECT_FLOAT_EQ(located.value().local.y, 0.0F);

    // Edge point (w-1, h-1) lands in the clipped corner cell.
    located = mirador::grid_locate(grid, PointF{9.0F, 6.0F});
    ASSERT_TRUE(located.ok());
    EXPECT_EQ(located.value().column, 3);
    EXPECT_EQ(located.value().row, 2);
    EXPECT_FLOAT_EQ(located.value().local.x, 0.0F);
    EXPECT_FLOAT_EQ(located.value().local.y, 0.0F);

    // The extreme interior point maps through the column clamp.
    located = mirador::grid_locate(grid, PointF{9.5F, 6.5F});
    ASSERT_TRUE(located.ok());
    EXPECT_EQ(located.value().column, 3);
    EXPECT_EQ(located.value().row, 2);
    EXPECT_FLOAT_EQ(located.value().local.x, 0.5F);
    EXPECT_FLOAT_EQ(located.value().local.y, 0.5F);
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

TEST(GridPartitionTest, LocateAndImagePointRoundTripWithinTolerance) {
    const GridPartition grid = make_grid(37, 23, 8);  // odd sizes, partial edge cells

    std::vector<PointF> probes;
    probes.push_back(PointF{0.0F, 0.0F});
    probes.push_back(PointF{36.0F, 22.0F});  // (w-1, h-1)
    probes.push_back(PointF{35.75F, 22.5F});
    for (int32_t row = 0; row < grid.rows; ++row) {
        for (int32_t column = 0; column < grid.columns; ++column) {
            const RectI cell = mirador::grid_cell_bounds(grid, column, row).value();
            // Cell center and top-left corner.
            probes.push_back(PointF{static_cast<float>(cell.x) + static_cast<float>(cell.width) / 2.0F,
                                    static_cast<float>(cell.y) + static_cast<float>(cell.height) / 2.0F});
            probes.push_back(PointF{static_cast<float>(cell.x), static_cast<float>(cell.y)});
        }
    }

    for (const PointF& point : probes) {
        const auto located = mirador::grid_locate(grid, point);
        ASSERT_TRUE(located.ok()) << "point (" << point.x << ", " << point.y << ")";
        const auto restored = mirador::grid_image_point(grid, located.value());
        ASSERT_TRUE(restored.ok());
        EXPECT_NEAR(restored.value().x, point.x, 1e-4F);
        EXPECT_NEAR(restored.value().y, point.y, 1e-4F);

        // And the restored point locates into the same cell.
        const auto relocated = mirador::grid_locate(grid, restored.value());
        ASSERT_TRUE(relocated.ok());
        EXPECT_EQ(relocated.value().column, located.value().column);
        EXPECT_EQ(relocated.value().row, located.value().row);
    }
}

TEST(GridPartitionTest, SingleCellGridRoundTripsEveryPoint) {
    const GridPartition grid = make_grid(5, 4, 64);  // one cell covering the image
    ASSERT_EQ(grid.columns, 1);
    ASSERT_EQ(grid.rows, 1);
    for (const PointF& point : std::vector<PointF>{{0.0F, 0.0F}, {2.5F, 2.0F}, {4.0F, 3.0F}}) {
        const auto located = mirador::grid_locate(grid, point);
        ASSERT_TRUE(located.ok());
        const auto restored = mirador::grid_image_point(grid, located.value());
        ASSERT_TRUE(restored.ok());
        EXPECT_NEAR(restored.value().x, point.x, 1e-4F);
        EXPECT_NEAR(restored.value().y, point.y, 1e-4F);
    }
    // The single cell is clipped to the image bounds.
    EXPECT_EQ(mirador::grid_cell_bounds(grid, 0, 0).value(), (RectI{0, 0, 5, 4}));
}

}  // namespace
