#ifndef MIRADOR_GEOMETRY_HPP
#define MIRADOR_GEOMETRY_HPP

#include <array>
#include <cstdint>
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

/// Axis-aligned rectangle in integer pixel coordinates: [x, x + width) x [y, y + height).
/// Used for pixel-exact ROIs on ImageView data.
struct RectI {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

/// True when the rectangle has a non-negative size and its right/bottom edges
/// (`x + width`, `y + height`) stay representable in int32.
[[nodiscard]] bool is_valid(const RectI& rect) noexcept;

/// True when the point lies inside [x, x + width) x [y, y + height); edge comparison
/// happens in double precision.
[[nodiscard]] bool contains(const RectI& rect, const PointF& point) noexcept;

/// True when `inner` lies fully inside `outer` (empty rects are contained only in
/// the matching degenerate sense); edge arithmetic happens in int64.
[[nodiscard]] bool contains(const RectI& outer, const RectI& inner) noexcept;

/// Overlap of two rectangles in pixel coordinates. When the rectangles are disjoint
/// the result has zero width or height and the clamped origin; component arithmetic
/// happens in int64 and saturates to the int32 range.
[[nodiscard]] RectI intersect(const RectI& first, const RectI& second) noexcept;

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
