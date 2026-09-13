#include <mirador/geometry.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace mirador {
namespace {

/// Saturates an int64 difference to the int32 range (see intersect).
int32_t saturated_delta(int64_t begin, int64_t end) noexcept {
    const int64_t delta = end > begin ? end - begin : 0;
    constexpr int64_t kInt32Max = INT32_MAX;
    return static_cast<int32_t>(delta > kInt32Max ? kInt32Max : delta);
}

}  // namespace

bool is_valid(const RectI& rect) noexcept {
    return rect.width >= 0 && rect.height >= 0 && rect.x <= INT32_MAX - rect.width && rect.y <= INT32_MAX - rect.height;
}

bool contains(const RectI& rect, const PointF& point) noexcept {
    if (rect.width <= 0 || rect.height <= 0) {
        return false;
    }
    const double right = static_cast<double>(rect.x) + rect.width;
    const double bottom = static_cast<double>(rect.y) + rect.height;
    return point.x >= static_cast<double>(rect.x) && point.x < right && point.y >= static_cast<double>(rect.y) &&
           point.y < bottom;
}

bool contains(const RectI& outer, const RectI& inner) noexcept {
    const int64_t outer_right = static_cast<int64_t>(outer.x) + outer.width;
    const int64_t outer_bottom = static_cast<int64_t>(outer.y) + outer.height;
    const int64_t inner_right = static_cast<int64_t>(inner.x) + inner.width;
    const int64_t inner_bottom = static_cast<int64_t>(inner.y) + inner.height;
    return inner.x >= outer.x && inner.y >= outer.y && inner_right <= outer_right && inner_bottom <= outer_bottom;
}

RectI intersect(const RectI& first, const RectI& second) noexcept {
    const int64_t left = std::max<int64_t>(first.x, second.x);
    const int64_t top = std::max<int64_t>(first.y, second.y);
    const int64_t right =
        std::min<int64_t>(static_cast<int64_t>(first.x) + first.width, static_cast<int64_t>(second.x) + second.width);
    const int64_t bottom =
        std::min<int64_t>(static_cast<int64_t>(first.y) + first.height, static_cast<int64_t>(second.y) + second.height);
    return RectI{static_cast<int32_t>(std::clamp<int64_t>(left, INT32_MIN, INT32_MAX)),
                 static_cast<int32_t>(std::clamp<int64_t>(top, INT32_MIN, INT32_MAX)), saturated_delta(left, right),
                 saturated_delta(top, bottom)};
}

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
