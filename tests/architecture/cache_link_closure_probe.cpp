// M1-07: probe executable that forces the mirador-cache translation units into
// the link closure so the NEEDED-entry check covers the whole cache module.
#include <mirador/frame_cache.hpp>

#include <array>
#include <cstddef>

int main() {
    auto cache_result = mirador::FrameCache::create(4096);
    if (!cache_result.ok()) {
        return 1;
    }
    mirador::FrameCache cache = cache_result.take_value();
    static const std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    if (!cache.insert(7, bytes).ok()) {
        return 2;
    }
    const auto hit = cache.lookup(7);
    return hit.ok() && hit.value() != nullptr ? 0 : 3;
}
