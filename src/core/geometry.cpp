#include <mirador/geometry.hpp>

#include <algorithm>
#include <array>
#include <span>

namespace mirador {

std::array<PointF, 4> corners(const RectF& rect) noexcept {
    const float right = rect.x + rect.width;
    const float bottom = rect.y + rect.height;
    return {PointF{rect.x, rect.y}, PointF{right, rect.y}, PointF{right, bottom}, PointF{rect.x, bottom}};
}

RectF bounding_rect(std::span<const PointF> points) noexcept {
    if (points.empty()) {
        return RectF{};
    }
    const auto [min_x, max_x] =
        std::minmax_element(points.begin(), points.end(), [](const PointF& a, const PointF& b) { return a.x < b.x; });
    const auto [min_y, max_y] =
        std::minmax_element(points.begin(), points.end(), [](const PointF& a, const PointF& b) { return a.y < b.y; });
    return RectF{min_x->x, min_y->y, max_x->x - min_x->x, max_y->y - min_y->y};
}

}  // namespace mirador
