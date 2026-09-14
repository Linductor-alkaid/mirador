#include "connected_components.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mirador/pixel_format.hpp>

#include <queue>
#include <utility>
#include <vector>
#include "mirador/image_view.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"

namespace mirador::image_internal {
namespace {

uint8_t pixel_at(const ImageView& view, int32_t x, int32_t y) noexcept {
    return std::to_integer<uint8_t>(*(view.data + static_cast<int64_t>(y) * view.row_stride_bytes + x));
}

/// One-bit-per-pixel visited map over the view; size budget-checked up front.
class VisitedMap {
public:
    VisitedMap(const ImageView& view, std::vector<uint8_t>& storage) : view_(view), storage_(storage) {}

    void mark(int32_t x, int32_t y) noexcept {
        const size_t bit = bit_index(x, y);
        storage_[bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));
    }

    [[nodiscard]] bool is_marked(int32_t x, int32_t y) const noexcept {
        const size_t bit = bit_index(x, y);
        return (storage_[bit / 8] & static_cast<uint8_t>(1U << (bit % 8))) != 0;
    }

private:
    [[nodiscard]] size_t bit_index(int32_t x, int32_t y) const noexcept {
        return static_cast<size_t>(y) * static_cast<size_t>(view_.width) + static_cast<size_t>(x);
    }

    const ImageView& view_;
    std::vector<uint8_t>& storage_;
};

/// Flood-fills one component from its first-scanned pixel; `queue` must be
/// empty on entry and stays empty on exit.
void collect_component(const ImageView& gray, uint8_t threshold, int32_t seed_x, int32_t seed_y, VisitedMap& visited,
                       std::queue<std::pair<int32_t, int32_t>>& queue, Component& component) {
    component.min_x = seed_x;
    component.min_y = seed_y;
    queue.emplace(seed_x, seed_y);
    visited.mark(seed_x, seed_y);
    while (!queue.empty()) {
        const auto [cx, cy] = queue.front();
        queue.pop();
        component.max_x = std::max(component.max_x, cx);
        component.max_y = std::max(component.max_y, cy);
        ++component.pixel_count;
        component.value_sum += pixel_at(gray, cx, cy);
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const int32_t nx = cx + dx;
                const int32_t ny = cy + dy;
                if (nx < 0 || nx >= gray.width || ny < 0 || ny >= gray.height || visited.is_marked(nx, ny)) {
                    continue;
                }
                if (pixel_at(gray, nx, ny) < threshold) {
                    continue;
                }
                visited.mark(nx, ny);
                queue.emplace(nx, ny);
            }
        }
    }
}

}  // namespace

Result<std::vector<Component>> find_components(const ImageView& gray, uint8_t threshold,
                                               int64_t work_budget_bytes) noexcept {
    if (!validate(gray).ok()) {
        return Status(ErrorCode::kInvalidArgument, "invalid view");
    }
    if (gray.format != PixelFormat::kGray8) {
        return Status(ErrorCode::kUnsupportedFormat, "connected components require kGray8");
    }
    const int64_t bitmap_bytes = (static_cast<int64_t>(gray.width) * gray.height + 7) / 8;
    if (bitmap_bytes > work_budget_bytes) {
        return Status(ErrorCode::kBudgetExceeded, "visited bitmap exceeds the work budget");
    }
    std::vector<uint8_t> storage(static_cast<size_t>(bitmap_bytes), 0);
    VisitedMap visited(gray, storage);

    std::vector<Component> components;
    std::queue<std::pair<int32_t, int32_t>> queue;
    for (int32_t y = 0; y < gray.height; ++y) {
        for (int32_t x = 0; x < gray.width; ++x) {
            if (visited.is_marked(x, y) || pixel_at(gray, x, y) < threshold) {
                continue;
            }
            Component component;
            collect_component(gray, threshold, x, y, visited, queue, component);
            components.push_back(component);
        }
    }
    return components;
}

}  // namespace mirador::image_internal
