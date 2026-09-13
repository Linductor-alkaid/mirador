#ifndef MIRADOR_TRANSFORM_HPP
#define MIRADOR_TRANSFORM_HPP

#include <mirador/geometry.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace mirador {

/// Identifier of a coordinate space attached to every region output (design section 7).
/// Well-known spaces cover the standard perception pipeline; callers define private
/// spaces starting at kUserBase.
enum class CoordinateSpaceId : uint32_t {
    kFrame = 0,              ///< raw capture pixels, before orientation
    kOriented = 1,           ///< after applying ImageView::rotation
    kCropped = 2,            ///< ROI-local space
    kModelInput = 3,         ///< after resize/letterbox into a backend input
    kDisplay = 4,            ///< final display space
    kUserBase = 0x00010000,  ///< first caller-defined space
};

/// Composable 2D affine transform between two coordinate spaces. The matrix is
/// row-major 3x3 and acts on homogeneous column vectors [x, y, 1]^T.
///
/// Coordinate convention: continuous pixel-area coordinates. A W x H image covers
/// [0, W) x [0, H); integer coordinates (x, y) refer to the unit square
/// [x, x + 1) x [y, y + 1). Reflections reverse interval endpoints; RectF mapping
/// returns the bounding box of the mapped corners, which keeps the numerically exact
/// interval. All factories produce the forward preprocessing direction (raw frame
/// towards model input); recovery composes inverses.
///
/// Mapping is exposed through transform_* free functions (deliberately not named
/// `apply`, which would be prone to ambiguous lookup against std::apply via
/// argument-dependent lookup when std containers are passed unqualified).
struct Transform2D {
    std::array<double, 9> matrix{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    CoordinateSpaceId from = CoordinateSpaceId::kFrame;
    CoordinateSpaceId to = CoordinateSpaceId::kOriented;
};

/// Maps raw frame coordinates to the rotated view (the rotation recorded by
/// ImageView::rotation). For k90/k270 the oriented view is raw_height x raw_width.
[[nodiscard]] Transform2D make_rotation(Rotation rotation, int32_t raw_width, int32_t raw_height,
                                        CoordinateSpaceId from, CoordinateSpaceId to);

/// Oriented view dimensions (width, height) for a rotated raw frame.
[[nodiscard]] std::pair<int32_t, int32_t> oriented_size(Rotation rotation, int32_t raw_width,
                                                        int32_t raw_height) noexcept;

[[nodiscard]] Transform2D make_identity(CoordinateSpaceId from, CoordinateSpaceId to) noexcept;

[[nodiscard]] Transform2D make_scale(double scale_x, double scale_y, CoordinateSpaceId from,
                                     CoordinateSpaceId to) noexcept;

/// Maps source coordinates into a crop-local space: p -> p - crop.origin.
[[nodiscard]] Transform2D make_crop(const RectF& crop, CoordinateSpaceId from, CoordinateSpaceId to) noexcept;

/// Mirrors within an image of the given size: horizontal flips x around the vertical
/// center line, vertical flips y.
[[nodiscard]] Transform2D make_mirror(bool horizontal, bool vertical, int32_t image_width, int32_t image_height,
                                      CoordinateSpaceId from, CoordinateSpaceId to) noexcept;

/// Uniform-scale letterbox mapping src into dst with centering padding; scale is
/// min(dst_w / src_w, dst_h / src_h). Returns kInvalidArgument for non-positive sizes.
[[nodiscard]] Result<Transform2D> make_letterbox(int32_t src_width, int32_t src_height, int32_t dst_width,
                                                 int32_t dst_height, CoordinateSpaceId from, CoordinateSpaceId to);

/// Applies `first`, then `second`. Returns kCoordinateTransform when the spaces do not
/// chain (first.to != second.from).
[[nodiscard]] Result<Transform2D> compose(const Transform2D& first, const Transform2D& second);

/// Inverts an affine transform, swapping from/to. Returns kCoordinateTransform when
/// the matrix is (numerically) singular.
[[nodiscard]] Result<Transform2D> inverse(const Transform2D& transform);

/// Applies the transform to a point. Always numerically defined; bounds are the
/// caller's concern (property tests assert them for legal preprocessing chains).
[[nodiscard]] PointF transform_point(const Transform2D& transform, PointF point) noexcept;

/// Applies the transform to a rectangle: the bounding box of the mapped corners.
[[nodiscard]] RectF transform_rect(const Transform2D& transform, const RectF& rect) noexcept;

/// Applies the transform to every point.
[[nodiscard]] std::vector<PointF> transform_points(const Transform2D& transform, std::span<const PointF> points);

/// Applies the transform to both endpoints; confidence is carried over unchanged.
[[nodiscard]] LineSegment transform_segment(const Transform2D& transform, const LineSegment& segment) noexcept;

}  // namespace mirador

#endif  // MIRADOR_TRANSFORM_HPP
