// M4-06 render link-closure probe (DEC-013): links the render module's grid
// partition (and with it the whole render->fusion closure); the readelf
// NEEDED check asserts the closure still loads only standard-library runtime.
#include <mirador/grid_partition.hpp>
#include <mirador/result.hpp>

int main() {
    const mirador::Result<mirador::GridPartition> grid = mirador::make_grid_partition(64, 48, 16);
    return grid.ok() ? grid.value().columns : 1;
}
