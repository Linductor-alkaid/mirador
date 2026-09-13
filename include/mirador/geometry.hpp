#ifndef MIRADOR_GEOMETRY_HPP
#define MIRADOR_GEOMETRY_HPP

#include <array>
#include <cstddef>
#include <span>

namespace mirador {

/// 2D point in continuous pixel-area coordinates (see Transform2D for the convention).
struct PointF {
    float x = 0.0F;
    float y = 0.0F;
};

/// Axis-aligned rectangle in pixel-area coordinates: [x, x + width) x [y, y + height).
struct RectF {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

/// Straight segment in continuous coordinates, with an optional confidence.
struct LineSegment {
    PointF begin;
    PointF end;
    float confidence = 0.0F;
};

/// The four corners of `rect` in order: top-left, top-right, bottom-right, bottom-left.
[[nodiscard]] std::array<PointF, 4> corners(const RectF& rect) noexcept;

/// Tight axis-aligned bounding rectangle of the points. Returns a zero rect for an
/// empty span.
[[nodiscard]] RectF bounding_rect(std::span<const PointF> points) noexcept;

}  // namespace mirador

#endif  // MIRADOR_GEOMETRY_HPP
