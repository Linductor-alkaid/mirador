// M4-06 render link-closure probe (DEC-013): links the SoM renderer and its
// transitive fusion dependency; the readelf NEEDED check asserts the whole
// render closure still loads only standard-library runtime.
#include <mirador/grid_partition.hpp>
#include <mirador/set_of_mark.hpp>

int main() {
    const mirador::Result<mirador::GridPartition> grid = mirador::make_grid_partition(64, 48, 16);
    return grid.ok() ? grid.value().columns : 1;
}
