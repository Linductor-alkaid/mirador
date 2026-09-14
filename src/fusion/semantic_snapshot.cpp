#include <mirador/semantic_snapshot.hpp>

namespace mirador {

const VisualRegion* find_region(const SemanticSnapshot& snapshot, uint64_t stable_id) noexcept {
    for (const VisualRegion& region : snapshot.regions) {
        if (region.stable_id == stable_id) {
            return &region;
        }
    }
    return nullptr;
}

bool is_generation_current(const SemanticSnapshot& snapshot, uint64_t generation) noexcept {
    return snapshot.generation == generation;
}

}  // namespace mirador
