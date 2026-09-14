#include <mirador/grid_partition.hpp>

#include <mirador/status.hpp>

#include <algorithm>
#include <cstdint>

namespace mirador {
namespace {

int32_t ceil_div(int32_t value, int32_t divisor) noexcept {
    return (value + divisor - 1) / divisor;
}

}  // namespace

Result<GridPartition> make_grid_partition(int32_t image_width, int32_t image_height,
                                          int32_t target_cell_side) noexcept {
    if (image_width <= 0 || image_height <= 0) {
        return Status(ErrorCode::kInvalidArgument, "grid image dimensions must be positive");
    }
    if (target_cell_side < 1 || target_cell_side > 4096) {
        return Status(ErrorCode::kInvalidArgument, "target_cell_side must be in [1, 4096]");
    }
    GridPartition grid;
    grid.image_width = image_width;
    grid.image_height = image_height;
    grid.cell_width = target_cell_side;
    grid.cell_height = target_cell_side;
    grid.columns = ceil_div(image_width, target_cell_side);
    grid.rows = ceil_div(image_height, target_cell_side);
    return grid;
}

Result<RectI> grid_cell_bounds(const GridPartition& grid, int32_t column, int32_t row) noexcept {
    if (column < 0 || column >= grid.columns || row < 0 || row >= grid.rows) {
        return Status(ErrorCode::kInvalidArgument, "grid cell index out of range");
    }
    const int32_t x = column * grid.cell_width;
    const int32_t y = row * grid.cell_height;
    const int32_t width = std::min(grid.cell_width, grid.image_width - x);
    const int32_t height = std::min(grid.cell_height, grid.image_height - y);
    return RectI{x, y, width, height};
}

Result<GridLocation> grid_locate(const GridPartition& grid, PointF image_point) noexcept {
    if (image_point.x < 0.0F || image_point.x >= static_cast<float>(grid.image_width) || image_point.y < 0.0F ||
        image_point.y >= static_cast<float>(grid.image_height)) {
        return Status(ErrorCode::kInvalidArgument, "image point lies outside the grid");
    }
    GridLocation location;
    location.column =
        std::min(static_cast<int32_t>(image_point.x) / grid.cell_width, grid.columns - 1);
    location.row = std::min(static_cast<int32_t>(image_point.y) / grid.cell_height, grid.rows - 1);
    location.local = PointF{image_point.x - static_cast<float>(location.column * grid.cell_width),
                            image_point.y - static_cast<float>(location.row * grid.cell_height)};
    return location;
}

Result<PointF> grid_image_point(const GridPartition& grid, const GridLocation& location) noexcept {
    if (location.column < 0 || location.column >= grid.columns || location.row < 0 || location.row >= grid.rows) {
        return Status(ErrorCode::kInvalidArgument, "grid cell index out of range");
    }
    return PointF{static_cast<float>(location.column * grid.cell_width) + location.local.x,
                  static_cast<float>(location.row * grid.cell_height) + location.local.y};
}

}  // namespace mirador
