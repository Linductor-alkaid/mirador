#include "connected_components.h"

#include <mirador/pixel_format.hpp>

#include <queue>
#include <vector>

namespace mirador::image_internal {
namespace {

uint8_t pixel_at(const ImageView& view, int32_t x, int32_t y) noexcept {
    return std::to_integer<uint8_t>(*(view.data + static_cast<int64_t>(y) * view.row_stride_bytes + x));
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
    std::vector<uint8_t> visited(static_cast<size_t>(bitmap_bytes), 0);
    const auto mark = [&visited, gray](int32_t x, int32_t y) {
        const size_t bit = static_cast<size_t>(y) * static_cast<size_t>(gray.width) + static_cast<size_t>(x);
        visited[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    };
    const auto is_marked = [&visited, gray](int32_t x, int32_t y) {
        const size_t bit = static_cast<size_t>(y) * static_cast<size_t>(gray.width) + static_cast<size_t>(x);
        return (visited[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) != 0;
    };

    std::vector<Component> components;
    std::queue<std::pair<int32_t, int32_t>> queue;
    for (int32_t y = 0; y < gray.height; ++y) {
        for (int32_t x = 0; x < gray.width; ++x) {
            if (is_marked(x, y) || pixel_at(gray, x, y) < threshold) {
                continue;
            }
            Component component;
            component.min_x = x;
            component.min_y = y;
            queue.emplace(x, y);
            mark(x, y);
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
                        if (nx < 0 || nx >= gray.width || ny < 0 || ny >= gray.height || is_marked(nx, ny)) {
                            continue;
                        }
                        if (pixel_at(gray, nx, ny) < threshold) {
                            continue;
                        }
                        mark(nx, ny);
                        queue.emplace(nx, ny);
                    }
                }
            }
            components.push_back(component);
        }
    }
    return components;
}

}  // namespace mirador::image_internal
