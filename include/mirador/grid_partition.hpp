#ifndef MIRADOR_GRID_PARTITION_HPP
#define MIRADOR_GRID_PARTITION_HPP

#include <mirador/geometry.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <optional>

namespace mirador {

/// Uniform grid over an image (design section 17): the coarse fallback cells
/// an upper layer may refine when region proposals miss the target. Pure
/// geometry — cell *selection strategy*, refinement loops and VLM control
/// flow stay with the caller. Edge cells are clipped to the image, so every
/// returned rect is inside the image.
struct GridPartition {
    int32_t image_width = 0;
    int32_t image_height = 0;
    int32_t columns = 0;    ///< in [1, image_width]
    int32_t rows = 0;       ///< in [1, image_height]
    int32_t cell_width = 0;   ///< nominal cell width; edge cells may be narrower
    int32_t cell_height = 0;  ///< nominal cell height; edge cells may be shorter
};

/// Partitions `image_width x image_height` into cells of approximately
/// `target_cell_side` pixels per side. Errors: kInvalidArgument for
/// non-positive dimensions or a target cell side outside [1, 4096]. Never
/// throws.
[[nodiscard]] Result<GridPartition> make_grid_partition(int32_t image_width, int32_t image_height,
                                                        int32_t target_cell_side) noexcept;

/// Pixel bounds of one cell; edge cells are clipped to the image. Returns
/// kInvalidArgument for out-of-range column/row. Never throws.
[[nodiscard]] Result<RectI> grid_cell_bounds(const GridPartition& grid, int32_t column, int32_t row) noexcept;

/// Position of an image point inside the grid: its cell plus cell-local
/// coordinates. Errors: kInvalidArgument when the point lies outside the
/// image. Never throws.
struct GridLocation {
    int32_t column = 0;
    int32_t row = 0;
    PointF local;
};

[[nodiscard]] Result<GridLocation> grid_locate(const GridPartition& grid, PointF image_point) noexcept;

/// Maps a cell-local point back to image coordinates (the inverse of
/// `grid_locate` for points inside the image). Errors: kInvalidArgument for
/// out-of-range cell indices. Never throws.
[[nodiscard]] Result<PointF> grid_image_point(const GridPartition& grid, const GridLocation& location) noexcept;

}  // namespace mirador

#endif  // MIRADOR_GRID_PARTITION_HPP
