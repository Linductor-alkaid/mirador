#ifndef MIRADOR_SRC_FUSION_RECT_MATH_H_
#define MIRADOR_SRC_FUSION_RECT_MATH_H_

// Internal rect math shared by the fusion engine and the stable-id tracker
// (double precision, continuous pixel-area coordinates). Not public API.

#include <mirador/geometry.hpp>

#include <algorithm>
#include <cmath>

namespace mirador::fusion_internal {

[[nodiscard]] inline double rect_area(const RectF& rect) noexcept {
    return static_cast<double>(rect.width) * static_cast<double>(rect.height);
}

[[nodiscard]] inline double intersection_area(const RectF& a, const RectF& b) noexcept {
    const double x0 = static_cast<double>(std::max(a.x, b.x));
    const double y0 = static_cast<double>(std::max(a.y, b.y));
    const double x1 = static_cast<double>(std::min(a.x + a.width, b.x + b.width));
    const double y1 = static_cast<double>(std::min(a.y + a.height, b.y + b.height));
    if (x1 <= x0 || y1 <= y0) {
        return 0.0;
    }
    return (x1 - x0) * (y1 - y0);
}

/// Intersection-over-union; 0 when both areas are zero.
[[nodiscard]] inline double rect_iou(const RectF& a, const RectF& b) noexcept {
    const double inter = intersection_area(a, b);
    const double denominator = rect_area(a) + rect_area(b) - inter;
    return denominator > 0.0 ? inter / denominator : 0.0;
}

[[nodiscard]] inline PointF rect_center(const RectF& rect) noexcept {
    return PointF{rect.x + rect.width / 2.0F, rect.y + rect.height / 2.0F};
}

/// True when the center of `inner` lies within `outer` (continuous coords).
[[nodiscard]] inline bool center_inside(const RectF& outer, const RectF& inner) noexcept {
    const PointF center = rect_center(inner);
    return center.x >= outer.x && center.x < outer.x + outer.width && center.y >= outer.y &&
           center.y < outer.y + outer.height;
}

[[nodiscard]] inline double center_distance(const RectF& a, const RectF& b) noexcept {
    const PointF ca = rect_center(a);
    const PointF cb = rect_center(b);
    return std::hypot(static_cast<double>(ca.x) - static_cast<double>(cb.x),
                      static_cast<double>(ca.y) - static_cast<double>(cb.y));
}

}  // namespace mirador::fusion_internal

#endif  // MIRADOR_SRC_FUSION_RECT_MATH_H_
