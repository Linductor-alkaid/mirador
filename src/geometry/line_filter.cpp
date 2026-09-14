#include <cstddef>
#include <mirador/line_detector.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>
#include "mirador/geometry.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"

namespace mirador {
namespace {

constexpr double kPi = std::numbers::pi;

/// True when the value is a finite number (filters and merges reject NaN/inf
/// coordinates as invalid input rather than propagating them silently).
bool is_finite(double value) noexcept {
    return std::isfinite(value);
}

bool is_finite(const LineSegment& segment) noexcept {
    return is_finite(static_cast<double>(segment.begin.x)) && is_finite(static_cast<double>(segment.begin.y)) &&
           is_finite(static_cast<double>(segment.end.x)) && is_finite(static_cast<double>(segment.end.y)) &&
           is_finite(static_cast<double>(segment.confidence));
}

/// Direction of the segment normalized to unit length. Precondition: length > 0.
struct Direction {
    double dx = 1.0;
    double dy = 0.0;
};

[[nodiscard]] Direction unit_direction(const LineSegment& segment) noexcept {
    const double dx = static_cast<double>(segment.end.x) - segment.begin.x;
    const double dy = static_cast<double>(segment.end.y) - segment.begin.y;
    const double length = std::sqrt(dx * dx + dy * dy);
    return Direction{dx / length, dy / length};
}

/// Squared perpendicular distance of `point` from the infinite line through
/// `origin` with unit direction `d`.
[[nodiscard]] double perpendicular_distance_sq(const PointF& point, const PointF& origin, const Direction& d) noexcept {
    const double px = static_cast<double>(point.x) - origin.x;
    const double py = static_cast<double>(point.y) - origin.y;
    // Cross product magnitude is the orthogonal distance for a unit direction.
    const double cross = px * d.dy - py * d.dx;
    return cross * cross;
}

/// Projection parameter of `point` on the line through `origin` with unit
/// direction `d` (origin at t=0, increasing along d).
[[nodiscard]] double projection(const PointF& point, const PointF& origin, const Direction& d) noexcept {
    const double px = static_cast<double>(point.x) - origin.x;
    const double py = static_cast<double>(point.y) - origin.y;
    return px * d.dx + py * d.dy;
}

struct Interval {
    double begin = 0.0;
    double end = 0.0;
};

[[nodiscard]] Interval projected_interval(const LineSegment& segment, const PointF& origin,
                                          const Direction& d) noexcept {
    const double t0 = projection(segment.begin, origin, d);
    const double t1 = projection(segment.end, origin, d);
    return t0 <= t1 ? Interval{t0, t1} : Interval{t1, t0};
}

/// Orientation difference of the two segments in degrees, in [0, 90].
[[nodiscard]] double orientation_difference_deg(const LineSegment& first, const LineSegment& second) noexcept {
    double diff = std::fabs(segment_angle_deg(first) - segment_angle_deg(second));
    if (diff > 90.0) {
        diff = 180.0 - diff;
    }
    return diff;
}

/// True when `candidate` is collinear and close enough to `reference` to merge.
[[nodiscard]] bool can_merge(const LineSegment& reference, const LineSegment& candidate,
                             const CollinearMergeParams& params) noexcept {
    if (orientation_difference_deg(reference, candidate) > params.angle_tolerance_deg) {
        return false;
    }
    const Direction d = unit_direction(reference);
    // Both lines must pass near each other: check each candidate endpoint
    // against the reference line and each reference endpoint against the
    // candidate line, so offset parallel segments are rejected.
    for (const PointF point : {candidate.begin, candidate.end}) {
        if (perpendicular_distance_sq(point, reference.begin, d) >
            params.distance_tolerance * params.distance_tolerance) {
            return false;
        }
    }
    const Direction dc = unit_direction(candidate);
    for (const PointF point : {reference.begin, reference.end}) {
        if (perpendicular_distance_sq(point, candidate.begin, dc) >
            params.distance_tolerance * params.distance_tolerance) {
            return false;
        }
    }
    // Overlapping projections always merge; separated ones need a gap within
    // the tolerance.
    const Interval a = projected_interval(reference, reference.begin, d);
    const Interval b = projected_interval(candidate, reference.begin, d);
    const double gap = std::max(b.begin - a.end, a.begin - b.end);
    return gap <= params.gap_tolerance;
}

/// Extends `reference` to cover `candidate`'s span along the reference
/// direction, keeping the strongest confidence.
[[nodiscard]] LineSegment absorb(const LineSegment& reference, const LineSegment& candidate) noexcept {
    const Direction d = unit_direction(reference);
    const Interval a = projected_interval(reference, reference.begin, d);
    const Interval b = projected_interval(candidate, reference.begin, d);
    LineSegment merged;
    merged.begin.x = static_cast<float>(reference.begin.x + d.dx * std::min(a.begin, b.begin));
    merged.begin.y = static_cast<float>(reference.begin.y + d.dy * std::min(a.begin, b.begin));
    merged.end.x = static_cast<float>(reference.begin.x + d.dx * std::max(a.end, b.end));
    merged.end.y = static_cast<float>(reference.begin.y + d.dy * std::max(a.end, b.end));
    merged.confidence = std::max(reference.confidence, candidate.confidence);
    return merged;
}

/// Angle window test with wrap-around through the 0/180 seam when the window
/// bounds are reversed.
[[nodiscard]] bool angle_in_window(double angle, const LineFilterParams& params) noexcept {
    if (params.min_angle_deg <= params.max_angle_deg) {
        return angle >= params.min_angle_deg && angle <= params.max_angle_deg;
    }
    return angle >= params.min_angle_deg || angle <= params.max_angle_deg;
}

/// Every active bound of `params` must hold for the segment to be kept.
[[nodiscard]] bool passes_filter(const LineSegment& segment, const LineFilterParams& params) noexcept {
    const double length = segment_length(segment);
    if (params.min_length > 0.0 && length < params.min_length) {
        return false;
    }
    if (params.max_length > 0.0 && length > params.max_length) {
        return false;
    }
    if (!angle_in_window(segment_angle_deg(segment), params)) {
        return false;
    }
    return params.min_confidence <= 0.0F || segment.confidence >= params.min_confidence;
}

}  // namespace

double segment_length(const LineSegment& segment) noexcept {
    const double dx = static_cast<double>(segment.end.x) - segment.begin.x;
    const double dy = static_cast<double>(segment.end.y) - segment.begin.y;
    return std::sqrt(dx * dx + dy * dy);
}

double segment_angle_deg(const LineSegment& segment) noexcept {
    const double dx = static_cast<double>(segment.end.x) - segment.begin.x;
    const double dy = static_cast<double>(segment.end.y) - segment.begin.y;
    double degrees = std::atan2(dy, dx) * 180.0 / kPi;
    if (degrees < 0.0) {
        degrees += 180.0;
    }
    if (degrees >= 180.0) {
        degrees -= 180.0;
    }
    return degrees;
}

namespace {

[[nodiscard]] Status validate_filter_params(const LineFilterParams& params) noexcept {
    if (!std::isfinite(params.min_length) || !std::isfinite(params.max_length) ||
        !std::isfinite(params.min_angle_deg) || !std::isfinite(params.max_angle_deg) ||
        !std::isfinite(static_cast<double>(params.min_confidence))) {
        return {ErrorCode::kInvalidArgument, "filter bounds must be finite"};
    }
    if (params.min_length < 0.0 || params.max_length < 0.0) {
        return {ErrorCode::kInvalidArgument, "length bounds must be non-negative"};
    }
    if (params.min_angle_deg < 0.0 || params.max_angle_deg > 180.0) {
        return {ErrorCode::kInvalidArgument, "angle bounds must lie in [0, 180]"};
    }
    if (params.min_confidence < 0.0F || params.min_confidence > 1.0F) {
        return {ErrorCode::kInvalidArgument, "min_confidence must lie in [0, 1]"};
    }
    return Status::success();
}

[[nodiscard]] Status validate_merge_params(const CollinearMergeParams& params) noexcept {
    if (!std::isfinite(params.angle_tolerance_deg) || !std::isfinite(params.distance_tolerance) ||
        !std::isfinite(params.gap_tolerance)) {
        return {ErrorCode::kInvalidArgument, "merge tolerances must be finite"};
    }
    if (params.angle_tolerance_deg < 0.0 || params.angle_tolerance_deg > 90.0 || params.distance_tolerance < 0.0 ||
        params.gap_tolerance < 0.0) {
        return {ErrorCode::kInvalidArgument, "merge tolerances must be non-negative (angle up to 90)"};
    }
    return Status::success();
}

[[nodiscard]] Status validate_segments(std::span<const LineSegment> segments) noexcept {
    for (const LineSegment& segment : segments) {
        if (!is_finite(segment)) {
            return {ErrorCode::kInvalidArgument, "segments must carry finite coordinates"};
        }
    }
    return Status::success();
}

/// Absorbs every collinear unconsumed segment into the seed until fixpoint.
/// Marks absorbed entries in `consumed`. Precondition: the seed is not
/// consumed and has positive length.
[[nodiscard]] LineSegment absorb_seed(std::span<const LineSegment> segments, size_t seed, std::vector<bool>& consumed,
                                      const CollinearMergeParams& params) {
    LineSegment current = segments[seed];
    bool grew = true;
    while (grew) {
        grew = false;
        for (size_t candidate = 0; candidate < segments.size(); ++candidate) {
            if (consumed[candidate] || segment_length(segments[candidate]) <= 0.0) {
                continue;
            }
            if (!can_merge(current, segments[candidate], params)) {
                continue;
            }
            current = absorb(current, segments[candidate]);
            consumed[candidate] = true;
            grew = true;
        }
    }
    return current;
}

}  // namespace

Result<std::vector<LineSegment>> filter_segments(std::span<const LineSegment> segments,
                                                 const LineFilterParams& params) {
    if (const Status invalid = validate_filter_params(params); !invalid.ok()) {
        return invalid;
    }
    if (const Status invalid = validate_segments(segments); !invalid.ok()) {
        return invalid;
    }

    std::vector<LineSegment> kept;
    kept.reserve(segments.size());
    for (const LineSegment& segment : segments) {
        if (passes_filter(segment, params)) {
            kept.push_back(segment);
        }
    }
    return kept;
}

Result<std::vector<LineSegment>> merge_collinear(std::span<const LineSegment> segments,
                                                 const CollinearMergeParams& params) {
    if (segments.size() > kMaxMergeSegments) {
        return Status(ErrorCode::kBudgetExceeded, "too many segments for collinear merge");
    }
    if (const Status invalid = validate_merge_params(params); !invalid.ok()) {
        return invalid;
    }
    if (const Status invalid = validate_segments(segments); !invalid.ok()) {
        return invalid;
    }

    std::vector<bool> consumed(segments.size(), false);
    std::vector<LineSegment> merged;
    for (size_t seed = 0; seed < segments.size(); ++seed) {
        if (consumed[seed]) {
            continue;
        }
        consumed[seed] = true;
        if (segment_length(segments[seed]) <= 0.0) {
            continue;  // zero-length segments are dropped, not merged
        }
        merged.push_back(absorb_seed(segments, seed, consumed, params));
    }
    return merged;
}

}  // namespace mirador
