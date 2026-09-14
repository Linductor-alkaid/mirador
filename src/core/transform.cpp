#include <mirador/transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include <mirador/geometry.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

namespace mirador {
namespace {

constexpr double kSingularEpsilon = 1e-12;

std::array<double, 9> multiply(const std::array<double, 9>& second, const std::array<double, 9>& first) {
    // Row-major 3x3 product: apply `first`, then `second`.
    std::array<double, 9> out{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += second[static_cast<size_t>(row) * 3 + k] * first[static_cast<size_t>(k) * 3 + col];
            }
            out[static_cast<size_t>(row) * 3 + col] = sum;
        }
    }
    return out;
}

PointF apply_matrix(const std::array<double, 9>& m, PointF point) noexcept {
    const auto x = static_cast<double>(point.x);
    const auto y = static_cast<double>(point.y);
    return PointF{static_cast<float>(m[0] * x + m[1] * y + m[2]), static_cast<float>(m[3] * x + m[4] * y + m[5])};
}

Status transform_error(const char* reason) {
    return {ErrorCode::kCoordinateTransform, reason};
}

}  // namespace

Transform2D make_rotation(Rotation rotation, int32_t raw_width, int32_t raw_height, CoordinateSpaceId from,
                          CoordinateSpaceId to) {
    Transform2D transform;
    transform.from = from;
    transform.to = to;
    const auto w = static_cast<double>(raw_width);
    const auto h = static_cast<double>(raw_height);
    switch (rotation) {
        case Rotation::k0:
            break;  // identity
        case Rotation::k90:
            // x_o = h - y_r, y_o = x_r (clockwise).
            transform.matrix = {0.0, -1.0, h, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
            break;
        case Rotation::k180:
            transform.matrix = {-1.0, 0.0, w, 0.0, -1.0, h, 0.0, 0.0, 1.0};
            break;
        case Rotation::k270:
            // x_o = y_r, y_o = w - x_r.
            transform.matrix = {0.0, 1.0, 0.0, -1.0, 0.0, w, 0.0, 0.0, 1.0};
            break;
    }
    return transform;
}

std::pair<int32_t, int32_t> oriented_size(Rotation rotation, int32_t raw_width, int32_t raw_height) noexcept {
    if (rotation == Rotation::k90 || rotation == Rotation::k270) {
        return {raw_height, raw_width};
    }
    return {raw_width, raw_height};
}

Transform2D make_identity(CoordinateSpaceId from, CoordinateSpaceId to) noexcept {
    Transform2D transform;
    transform.from = from;
    transform.to = to;
    return transform;
}

Transform2D make_scale(double scale_x, double scale_y, CoordinateSpaceId from, CoordinateSpaceId to) noexcept {
    Transform2D transform;
    transform.from = from;
    transform.to = to;
    transform.matrix = {scale_x, 0.0, 0.0, 0.0, scale_y, 0.0, 0.0, 0.0, 1.0};
    return transform;
}

Transform2D make_translation(double dx, double dy, CoordinateSpaceId from, CoordinateSpaceId to) noexcept {
    Transform2D transform;
    transform.from = from;
    transform.to = to;
    transform.matrix = {1.0, 0.0, dx, 0.0, 1.0, dy, 0.0, 0.0, 1.0};
    return transform;
}

Transform2D make_crop(const RectF& crop, CoordinateSpaceId from, CoordinateSpaceId to) noexcept {
    Transform2D transform;
    transform.from = from;
    transform.to = to;
    transform.matrix = {1.0, 0.0, -static_cast<double>(crop.x), 0.0, 1.0, -static_cast<double>(crop.y), 0.0, 0.0, 1.0};
    return transform;
}

Transform2D make_mirror(bool horizontal, bool vertical, int32_t image_width, int32_t image_height,
                        CoordinateSpaceId from, CoordinateSpaceId to) noexcept {
    Transform2D transform;
    transform.from = from;
    transform.to = to;
    const auto w = static_cast<double>(image_width);
    const auto h = static_cast<double>(image_height);
    const double sx = horizontal ? -1.0 : 1.0;
    const double sy = vertical ? -1.0 : 1.0;
    const double tx = horizontal ? w : 0.0;
    const double ty = vertical ? h : 0.0;
    transform.matrix = {sx, 0.0, tx, 0.0, sy, ty, 0.0, 0.0, 1.0};
    return transform;
}

Result<Transform2D> make_letterbox(int32_t src_width, int32_t src_height, int32_t dst_width, int32_t dst_height,
                                   CoordinateSpaceId from, CoordinateSpaceId to) {
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
        return Status(ErrorCode::kInvalidArgument, "letterbox sizes must be positive");
    }
    const auto scale_x = static_cast<double>(dst_width) / static_cast<double>(src_width);
    const auto scale_y = static_cast<double>(dst_height) / static_cast<double>(src_height);
    const double scale = std::min(scale_x, scale_y);
    const auto pad_x = (static_cast<double>(dst_width) - scale * src_width) / 2.0;
    const auto pad_y = (static_cast<double>(dst_height) - scale * src_height) / 2.0;
    Transform2D transform;
    transform.from = from;
    transform.to = to;
    transform.matrix = {scale, 0.0, pad_x, 0.0, scale, pad_y, 0.0, 0.0, 1.0};
    return transform;
}

Result<Transform2D> compose(const Transform2D& first, const Transform2D& second) {
    if (first.to != second.from) {
        return transform_error("compose requires first.to == second.from");
    }
    Transform2D chained;
    chained.from = first.from;
    chained.to = second.to;
    chained.matrix = multiply(second.matrix, first.matrix);
    return chained;
}

Result<Transform2D> inverse(const Transform2D& transform) {
    const std::array<double, 9>& m = transform.matrix;
    const double det = m[0] * m[4] - m[1] * m[3];
    if (std::fabs(det) < kSingularEpsilon) {
        return transform_error("transform is singular and cannot be inverted");
    }
    const double inv_det = 1.0 / det;
    Transform2D inverted;
    inverted.from = transform.to;
    inverted.to = transform.from;
    inverted.matrix = {
        m[4] * inv_det,
        -m[1] * inv_det,
        (m[1] * m[5] - m[2] * m[4]) * inv_det,
        -m[3] * inv_det,
        m[0] * inv_det,
        (m[2] * m[3] - m[0] * m[5]) * inv_det,
        0.0,
        0.0,
        1.0,
    };
    return inverted;
}

PointF transform_point(const Transform2D& transform, PointF point) noexcept {
    return apply_matrix(transform.matrix, point);
}

RectF transform_rect(const Transform2D& transform, const RectF& rect) noexcept {
    const std::array<PointF, 4> rect_corners = corners(rect);
    std::array<PointF, 4> mapped{};
    std::transform(rect_corners.begin(), rect_corners.end(), mapped.begin(),
                   [&transform](PointF point) { return apply_matrix(transform.matrix, point); });
    return bounding_rect(mapped);
}

std::vector<PointF> transform_points(const Transform2D& transform, std::span<const PointF> points) {
    std::vector<PointF> mapped;
    mapped.reserve(points.size());
    std::transform(points.begin(), points.end(), std::back_inserter(mapped),
                   [&transform](PointF point) { return apply_matrix(transform.matrix, point); });
    return mapped;
}

LineSegment transform_segment(const Transform2D& transform, const LineSegment& segment) noexcept {
    return LineSegment{apply_matrix(transform.matrix, segment.begin), apply_matrix(transform.matrix, segment.end),
                       segment.confidence};
}

}  // namespace mirador
