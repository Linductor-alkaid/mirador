#include <cstddef>
#include <mirador/geometric_proposal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>
#include "mirador/execution_context.hpp"
#include "mirador/geometry.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"

namespace mirador {
namespace {

constexpr int kCancelPollRows = 64;

bool is_finite(double value) noexcept {
    return std::isfinite(value);
}

bool is_finite(const LineSegment& segment) noexcept {
    return is_finite(static_cast<double>(segment.begin.x)) && is_finite(static_cast<double>(segment.begin.y)) &&
           is_finite(static_cast<double>(segment.end.x)) && is_finite(static_cast<double>(segment.end.y)) &&
           is_finite(static_cast<double>(segment.confidence));
}

double squared_distance(const PointF& a, const PointF& b) noexcept {
    const double dx = static_cast<double>(a.x) - b.x;
    const double dy = static_cast<double>(a.y) - b.y;
    return dx * dx + dy * dy;
}

/// Disjoint-set with path halving; the partition is independent of union order.
class DisjointSet {
public:
    explicit DisjointSet(size_t size) : parent_(size) {
        for (size_t i = 0; i < size; ++i) {
            parent_[i] = i;
        }
    }

    size_t find(size_t node) noexcept {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }

    void unite(size_t a, size_t b) noexcept {
        const size_t ra = find(a);
        const size_t rb = find(b);
        if (ra != rb) {
            parent_[rb] = ra;
        }
    }

private:
    std::vector<size_t> parent_;
};

/// Cancellation/deadline check for the quadratic passes; polled per row-block.
[[nodiscard]] Status poll_context(const ExecutionContext& context, size_t row) noexcept {
    if (row % kCancelPollRows != 0) {
        return Status::success();
    }
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "geometric proposal cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "geometric proposal deadline exceeded"};
    }
    return Status::success();
}

[[nodiscard]] Status validate_params(const GeometricProposalParams& params) noexcept {
    if (!is_finite(params.endpoint_radius_px) || params.endpoint_radius_px <= 0.0) {
        return {ErrorCode::kInvalidArgument, "endpoint_radius_px must be finite and > 0"};
    }
    if (!is_finite(params.angle_tolerance_deg) || params.angle_tolerance_deg <= 0.0 ||
        params.angle_tolerance_deg > 90.0) {
        return {ErrorCode::kInvalidArgument, "angle_tolerance_deg must be in (0, 90]"};
    }
    if (!(params.min_closure_score >= 0.0F) || !(params.min_closure_score <= 1.0F)) {
        return {ErrorCode::kInvalidArgument, "min_closure_score must be in [0, 1]"};
    }
    if (params.min_segments < 1) {
        return {ErrorCode::kInvalidArgument, "min_segments must be >= 1"};
    }
    if (params.max_segments < 1 || params.max_proposals < 1) {
        return {ErrorCode::kInvalidArgument, "max_segments and max_proposals must be >= 1"};
    }
    if (!is_finite(params.context_ratio) || params.context_ratio < 0.0) {
        return {ErrorCode::kInvalidArgument, "context_ratio must be finite and >= 0"};
    }
    if (!is_finite(params.min_context_padding_px) || params.min_context_padding_px < 0.0) {
        return {ErrorCode::kInvalidArgument, "min_context_padding_px must be finite and >= 0"};
    }
    if (!is_finite(params.max_context_padding_px) || params.max_context_padding_px < params.min_context_padding_px) {
        return {ErrorCode::kInvalidArgument, "max_context_padding_px must be >= min_context_padding_px"};
    }
    return Status::success();
}

struct SegmentData {
    LineSegment segment;
    double length = 0.0;
    double angle = 0.0;  ///< orientation in [0, pi)
};

/// Drops zero-length segments (their orientation is undefined); keeps input
/// order. Fails on non-finite data.
[[nodiscard]] Status prepare_segments(std::span<const LineSegment> segments, std::vector<SegmentData>& out) {
    out.reserve(segments.size());
    for (const LineSegment& segment : segments) {
        if (!is_finite(segment)) {
            return {ErrorCode::kInvalidArgument, "segment carries non-finite coordinates or confidence"};
        }
        const double dx = static_cast<double>(segment.end.x) - segment.begin.x;
        const double dy = static_cast<double>(segment.end.y) - segment.begin.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length == 0.0) {
            continue;
        }
        double angle = std::atan2(dy, dx);
        if (angle < 0.0) {
            angle += std::numbers::pi;
        }
        if (angle >= std::numbers::pi) {
            angle -= std::numbers::pi;
        }
        out.push_back(SegmentData{segment, length, angle});
    }
    return Status::success();
}

/// True when any endpoint pair of the two segments lies within `radius_sq`.
[[nodiscard]] bool endpoints_within_radius(const std::array<PointF, 2>& a, const std::array<PointF, 2>& b,
                                           double radius_sq) noexcept {
    for (const PointF& pa : a) {
        for (const PointF& pb : b) {
            if (squared_distance(pa, pb) <= radius_sq) {
                return true;
            }
        }
    }
    return false;
}

/// Unions segments whose endpoint pairs lie within the radius; polls `context`
/// per row-block.
[[nodiscard]] Status link_by_endpoint_proximity(std::span<const SegmentData> segments, double radius_sq,
                                                const ExecutionContext& context, DisjointSet& sets) {
    for (size_t i = 0; i < segments.size(); ++i) {
        if (const Status poll = poll_context(context, i); !poll.ok()) {
            return poll;
        }
        const std::array<PointF, 2> ends_i{segments[i].segment.begin, segments[i].segment.end};
        for (size_t j = i + 1; j < segments.size(); ++j) {
            const std::array<PointF, 2> ends_j{segments[j].segment.begin, segments[j].segment.end};
            if (endpoints_within_radius(ends_i, ends_j, radius_sq)) {
                sets.unite(i, j);
            }
        }
    }
    return Status::success();
}

/// Collects union-find members into clusters ordered by smallest member index;
/// members ascend.
void ordered_clusters(std::vector<std::vector<size_t>> members, std::vector<std::vector<size_t>>& clusters) {
    std::vector<size_t> roots;
    for (size_t root = 0; root < members.size(); ++root) {
        if (!members[root].empty()) {
            roots.push_back(root);
        }
    }
    std::sort(roots.begin(), roots.end(),
              [&members](size_t a, size_t b) { return members[a].front() < members[b].front(); });
    clusters.clear();
    clusters.reserve(roots.size());
    for (const size_t root : roots) {
        clusters.push_back(std::move(members[root]));
    }
}

/// Groups segments by endpoint proximity (any endpoint pair of two distinct
/// segments within the radius). Clusters are ordered by smallest member index;
/// members ascend.
[[nodiscard]] Status collect_clusters(std::span<const SegmentData> segments, double radius,
                                      const ExecutionContext& context, std::vector<std::vector<size_t>>& clusters) {
    const size_t count = segments.size();
    DisjointSet sets(count);
    if (const Status failed = link_by_endpoint_proximity(segments, radius * radius, context, sets); !failed.ok()) {
        return failed;
    }
    std::vector<std::vector<size_t>> members(count);
    for (size_t i = 0; i < count; ++i) {
        members[sets.find(i)].push_back(i);
    }
    ordered_clusters(std::move(members), clusters);
    return Status::success();
}

/// Endpoint-junction graph of one structure. Junctions merge endpoints within
/// the radius; the representative point is the lowest endpoint slot of the
/// union. A segment is "shared" when one of its endpoints sits in a junction
/// holding endpoints of at least two distinct segments. Sets `error` and
/// returns false when `context` reports cancellation or timeout.
struct JunctionGraph {
    size_t junction_count = 0;
    size_t straight_edge_count = 0;  ///< segments whose endpoints sit in distinct junctions
    size_t dangling_count = 0;
    PointF dangling_a;
    PointF dangling_b;
    size_t shared_segments = 0;
};

/// Merges endpoint slots within `radius_sq`; polls `context` and returns false
/// with `error` set on cancellation/timeout.
[[nodiscard]] bool merge_endpoint_slots(std::span<const PointF> points, double radius_sq,
                                        const ExecutionContext& context, DisjointSet& sets, Status& error) {
    for (size_t p = 0; p < points.size(); ++p) {
        if (const Status poll = poll_context(context, p); !poll.ok()) {
            error = poll;
            return false;
        }
        for (size_t q = p + 1; q < points.size(); ++q) {
            if (squared_distance(points[p], points[q]) <= radius_sq) {
                sets.unite(p, q);
            }
        }
    }
    return true;
}

/// Dense junction ids ordered by first occurrence (lowest slot = canonical).
struct JunctionIds {
    std::vector<size_t> of_slot;
    std::vector<PointF> representative;
};

JunctionIds assign_junction_ids(DisjointSet& sets, std::span<const PointF> points) {
    const size_t slots = points.size();
    JunctionIds ids;
    ids.of_slot.assign(slots, slots);
    for (size_t p = 0; p < slots; ++p) {
        const size_t root = sets.find(p);
        if (ids.of_slot[root] == slots) {
            ids.of_slot[root] = ids.representative.size();
            ids.representative.push_back(points[root]);
        }
        ids.of_slot[p] = ids.of_slot[root];
    }
    return ids;
}

/// Marks all segments of a shared junction (holding endpoints of at least two
/// distinct segments) as supported. Member lists ascend, so distinct
/// membership is an adjacent comparison.
void mark_shared_junction(std::span<const size_t> at_junction, std::vector<bool>& supported) {
    for (size_t i = 1; i < at_junction.size(); ++i) {
        if (at_junction[i - 1] != at_junction[i]) {
            for (const size_t k : at_junction) {
                supported[k] = true;
            }
            return;
        }
    }
}

/// Records a dangling junction (degree 1) into the graph's dangling slots.
void record_dangling(std::span<const PointF> junction_points, size_t junction, JunctionGraph& graph) {
    if (graph.dangling_count == 0) {
        graph.dangling_a = junction_points[junction];
    } else if (graph.dangling_count == 1) {
        graph.dangling_b = junction_points[junction];
    }
    ++graph.dangling_count;
}

/// Fills the junction-graph statistics from dense junction ids.
void accumulate_junction_stats(const JunctionIds& ids, JunctionGraph& graph) {
    const size_t count = ids.of_slot.size() / 2;
    graph = JunctionGraph{};
    graph.junction_count = ids.representative.size();
    std::vector<size_t> degree(ids.representative.size(), 0);
    std::vector<std::vector<size_t>> junction_members(ids.representative.size());
    for (size_t k = 0; k < count; ++k) {
        const size_t a = ids.of_slot[2 * k];
        const size_t b = ids.of_slot[2 * k + 1];
        ++degree[a];
        junction_members[a].push_back(k);
        if (b != a) {
            ++degree[b];
            junction_members[b].push_back(k);
            ++graph.straight_edge_count;
        }
    }
    std::vector<bool> supported(count, false);
    for (size_t j = 0; j < ids.representative.size(); ++j) {
        mark_shared_junction(junction_members[j], supported);
        if (degree[j] == 1) {
            record_dangling(ids.representative, j, graph);
        }
    }
    graph.shared_segments = static_cast<size_t>(std::count(supported.begin(), supported.end(), true));
}

[[nodiscard]] bool build_junction_graph(std::span<const SegmentData> segments, std::span<const size_t> members,
                                        double radius, const ExecutionContext& context, JunctionGraph& graph,
                                        Status& error) {
    const size_t count = members.size();
    const size_t slots = 2 * count;
    std::vector<PointF> points(slots);
    for (size_t k = 0; k < count; ++k) {
        points[2 * k] = segments[members[k]].segment.begin;
        points[2 * k + 1] = segments[members[k]].segment.end;
    }

    DisjointSet sets(slots);
    if (!merge_endpoint_slots(points, radius * radius, context, sets, error)) {
        return false;
    }
    const JunctionIds ids = assign_junction_ids(sets, points);
    accumulate_junction_stats(ids, graph);
    return true;
}

/// Closure degree in [0, 1]: a junction-graph cycle means fully closed;
/// otherwise exactly two dangling junctions with a small gap (relative to the
/// structure diagonal) mean near-closed.
[[nodiscard]] float closure_degree(const JunctionGraph& graph, std::span<const PointF> points) noexcept {
    if (graph.junction_count == 0) {
        return 0.0F;
    }
    const long long rank =
        static_cast<long long>(graph.straight_edge_count) - static_cast<long long>(graph.junction_count) + 1;
    if (rank >= 1) {
        return 1.0F;
    }
    if (graph.dangling_count != 2) {
        return 0.0F;
    }
    auto min_x = static_cast<double>(points[0].x);
    double max_x = min_x;
    auto min_y = static_cast<double>(points[0].y);
    double max_y = min_y;
    for (const PointF& point : points) {
        min_x = std::min(min_x, static_cast<double>(point.x));
        max_x = std::max(max_x, static_cast<double>(point.x));
        min_y = std::min(min_y, static_cast<double>(point.y));
        max_y = std::max(max_y, static_cast<double>(point.y));
    }
    const double diagonal = std::sqrt((max_x - min_x) * (max_x - min_x) + (max_y - min_y) * (max_y - min_y));
    if (diagonal == 0.0) {
        return 0.0F;
    }
    const double gap = std::sqrt(squared_distance(graph.dangling_a, graph.dangling_b));
    return static_cast<float>(std::max(0.0, 1.0 - gap / diagonal));
}

/// Length-weighted fraction of segment length within `angle_tolerance_deg` of
/// the two principal directions (circular mean over doubled angles).
[[nodiscard]] float rectangularity_degree(std::span<const SegmentData> segments, std::span<const size_t> members,
                                          double angle_tolerance_deg) noexcept {
    double cos_sum = 0.0;
    double sin_sum = 0.0;
    double total = 0.0;
    for (const size_t k : members) {
        const double two_angle = 2.0 * segments[k].angle;
        cos_sum += segments[k].length * std::cos(two_angle);
        sin_sum += segments[k].length * std::sin(two_angle);
        total += segments[k].length;
    }
    if (total == 0.0) {
        return 0.0F;
    }
    const double principal = 0.5 * std::atan2(sin_sum, cos_sum);
    const double tolerance = angle_tolerance_deg * std::numbers::pi / 180.0;
    double aligned = 0.0;
    for (const size_t k : members) {
        double delta = std::fabs(segments[k].angle - principal);
        if (delta > std::numbers::pi / 2.0) {
            delta = std::numbers::pi - delta;
        }
        const double delta_perp = std::fabs(std::numbers::pi / 2.0 - delta);
        if (delta <= tolerance || delta_perp <= tolerance) {
            aligned += segments[k].length;
        }
    }
    return static_cast<float>(aligned / total);
}

/// Counter-clockwise convex hull without collinear points (Andrew monotone
/// chain on a lexicographic sort; deterministic in double arithmetic). Empty
/// for fewer than two input points.
[[nodiscard]] std::vector<PointF> convex_hull(std::span<const PointF> points) {
    if (points.size() < 3) {
        return {points.begin(), points.end()};
    }
    std::vector<PointF> sorted(points.begin(), points.end());
    std::sort(sorted.begin(), sorted.end(), [](const PointF& a, const PointF& b) {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.y < b.y;
    });
    auto cross = [](const PointF& o, const PointF& a, const PointF& b) {
        const double ax = static_cast<double>(a.x) - o.x;
        const double ay = static_cast<double>(a.y) - o.y;
        const double bx = static_cast<double>(b.x) - o.x;
        const double by = static_cast<double>(b.y) - o.y;
        return ax * by - ay * bx;
    };
    std::vector<PointF> hull;
    hull.reserve(2 * sorted.size());
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<PointF> chain;
        for (size_t i = 0; i < sorted.size(); ++i) {
            const PointF& point = pass == 0 ? sorted[i] : sorted[sorted.size() - 1 - i];
            while (chain.size() >= 2 && cross(chain[chain.size() - 2], chain.back(), point) <= 0.0) {
                chain.pop_back();
            }
            chain.push_back(point);
        }
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            hull.push_back(chain[i]);
        }
    }
    return hull;
}

/// Minimum-area oriented rectangle of the hull (rotating calipers; ties keep
/// the lowest hull edge, so the result is deterministic).
[[nodiscard]] OrientedRect minimum_oriented_rect(std::span<const PointF> hull) noexcept {
    if (hull.empty()) {
        return OrientedRect{};
    }
    if (hull.size() == 1) {
        return OrientedRect{hull[0], 0.0F, 0.0F, 0.0F};
    }
    if (hull.size() == 2) {
        const double dx = static_cast<double>(hull[1].x) - hull[0].x;
        const double dy = static_cast<double>(hull[1].y) - hull[0].y;
        double angle = std::atan2(dy, dx);
        if (angle < 0.0) {
            angle += std::numbers::pi;
        }
        const PointF center{static_cast<float>((hull[0].x + hull[1].x) / 2.0),
                            static_cast<float>((hull[0].y + hull[1].y) / 2.0)};
        return OrientedRect{center, static_cast<float>(std::sqrt(dx * dx + dy * dy)), 0.0F,
                            static_cast<float>(angle * 180.0 / std::numbers::pi)};
    }
    double best_area = std::numeric_limits<double>::infinity();
    OrientedRect best;
    for (size_t i = 0; i < hull.size(); ++i) {
        const PointF& origin = hull[i];
        const PointF& next = hull[(i + 1) % hull.size()];
        const double dx = static_cast<double>(next.x) - origin.x;
        const double dy = static_cast<double>(next.y) - origin.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length == 0.0) {
            continue;
        }
        const double ux = dx / length;
        const double uy = dy / length;
        double min_u = 0.0;
        double max_u = 0.0;
        double min_v = 0.0;
        double max_v = 0.0;
        for (const PointF& point : hull) {
            const double rx = static_cast<double>(point.x) - origin.x;
            const double ry = static_cast<double>(point.y) - origin.y;
            const double u = rx * ux + ry * uy;
            const double v = -rx * uy + ry * ux;
            min_u = std::min(min_u, u);
            max_u = std::max(max_u, u);
            min_v = std::min(min_v, v);
            max_v = std::max(max_v, v);
        }
        const double area = (max_u - min_u) * (max_v - min_v);
        if (area < best_area) {
            best_area = area;
            double angle = std::atan2(uy, ux);
            if (angle < 0.0) {
                angle += std::numbers::pi;
            }
            const double mid_u = (min_u + max_u) / 2.0;
            const double mid_v = (min_v + max_v) / 2.0;
            best.center = PointF{static_cast<float>(origin.x + ux * mid_u - uy * mid_v),
                                 static_cast<float>(origin.y + uy * mid_u + ux * mid_v)};
            best.width = static_cast<float>(max_u - min_u);
            best.height = static_cast<float>(max_v - min_v);
            best.angle_deg = static_cast<float>(angle * 180.0 / std::numbers::pi);
        }
    }
    return best;
}

[[nodiscard]] bool fits_int32_rect(int64_t x, int64_t y, int64_t width, int64_t height) noexcept {
    constexpr int64_t kInt32Min = std::numeric_limits<int32_t>::min();
    constexpr int64_t kInt32Max = std::numeric_limits<int32_t>::max();
    return x >= kInt32Min && y >= kInt32Min && width >= 0 && height >= 0 && width <= kInt32Max && height <= kInt32Max &&
           x + width <= kInt32Max && y + height <= kInt32Max;
}

/// Tight integer bounds of the points: [floor(min), ceil(max)) covers every
/// endpoint's pixel area. Fails when the extent leaves int32 range.
[[nodiscard]] Status make_tight_bounds(std::span<const PointF> points, RectI& out) {
    auto min_x = static_cast<double>(points[0].x);
    double max_x = min_x;
    auto min_y = static_cast<double>(points[0].y);
    double max_y = min_y;
    for (const PointF& point : points) {
        min_x = std::min(min_x, static_cast<double>(point.x));
        max_x = std::max(max_x, static_cast<double>(point.x));
        min_y = std::min(min_y, static_cast<double>(point.y));
        max_y = std::max(max_y, static_cast<double>(point.y));
    }
    const auto x0 = static_cast<int64_t>(std::floor(min_x));
    const auto y0 = static_cast<int64_t>(std::floor(min_y));
    const auto x1 = static_cast<int64_t>(std::ceil(max_x));
    const auto y1 = static_cast<int64_t>(std::ceil(max_y));
    if (!fits_int32_rect(x0, y0, x1 - x0, y1 - y0)) {
        return {ErrorCode::kInvalidArgument, "proposal bounds leave the int32 coordinate range"};
    }
    out = RectI{static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x1 - x0),
                static_cast<int32_t>(y1 - y0)};
    return Status::success();
}

/// Context bounds: each axis pads the tight rect by `context_ratio * extent`
/// clamped into [min, max] pixels. Fails when the extent leaves int32 range.
[[nodiscard]] Status make_context_bounds(const RectI& tight, const GeometricProposalParams& params, RectI& out) {
    const double pad_x = std::clamp(params.context_ratio * static_cast<double>(tight.width),
                                    params.min_context_padding_px, params.max_context_padding_px);
    const double pad_y = std::clamp(params.context_ratio * static_cast<double>(tight.height),
                                    params.min_context_padding_px, params.max_context_padding_px);
    const auto pad_x_px = static_cast<int64_t>(std::floor(pad_x));
    const auto pad_y_px = static_cast<int64_t>(std::floor(pad_y));
    const int64_t x0 = static_cast<int64_t>(tight.x) - pad_x_px;
    const int64_t y0 = static_cast<int64_t>(tight.y) - pad_y_px;
    const int64_t width = static_cast<int64_t>(tight.width) + 2 * pad_x_px;
    const int64_t height = static_cast<int64_t>(tight.height) + 2 * pad_y_px;
    if (!fits_int32_rect(x0, y0, width, height)) {
        return {ErrorCode::kInvalidArgument, "context bounds leave the int32 coordinate range"};
    }
    out = RectI{static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(width),
                static_cast<int32_t>(height)};
    return Status::success();
}

}  // namespace

Result<std::vector<GeometricRegionProposal>> propose_regions(std::span<const LineSegment> segments,
                                                             const GeometricProposalParams& params,
                                                             const ExecutionContext& context) {
    if (const Status invalid = validate_params(params); !invalid.ok()) {
        return invalid;
    }
    if (segments.size() > static_cast<size_t>(params.max_segments)) {
        return Status{ErrorCode::kBudgetExceeded, "too many input segments for geometric proposal"};
    }
    std::vector<SegmentData> prepared;
    if (const Status invalid = prepare_segments(segments, prepared); !invalid.ok()) {
        return invalid;
    }

    std::vector<std::vector<size_t>> clusters;
    if (const Status failed = collect_clusters(prepared, params.endpoint_radius_px, context, clusters); !failed.ok()) {
        return failed;
    }

    std::vector<GeometricRegionProposal> proposals;
    for (const std::vector<size_t>& members : clusters) {
        if (members.size() < static_cast<size_t>(params.min_segments)) {
            continue;
        }
        const size_t count = members.size();
        std::vector<PointF> endpoints(2 * count);
        for (size_t k = 0; k < count; ++k) {
            endpoints[2 * k] = prepared[members[k]].segment.begin;
            endpoints[2 * k + 1] = prepared[members[k]].segment.end;
        }

        JunctionGraph graph;
        Status error;
        if (!build_junction_graph(prepared, members, params.endpoint_radius_px, context, graph, error)) {
            return error;
        }
        if (graph.junction_count < 3) {
            continue;  // fewer than three junctions cannot bound an area
        }
        const float closure = closure_degree(graph, endpoints);
        if (closure < params.min_closure_score) {
            continue;
        }
        if (proposals.size() >= static_cast<size_t>(params.max_proposals)) {
            return Status{ErrorCode::kBudgetExceeded, "too many qualifying structures for geometric proposal"};
        }

        GeometricRegionProposal proposal;
        proposal.closure_score = closure;
        proposal.rectangularity = rectangularity_degree(prepared, members, params.angle_tolerance_deg);
        proposal.edge_support =
            graph.junction_count == 0 ? 0.0F : static_cast<float>(graph.shared_segments) / static_cast<float>(count);
        if (const Status failed = make_tight_bounds(endpoints, proposal.tight_bounds); !failed.ok()) {
            return failed;
        }
        if (const Status failed = make_context_bounds(proposal.tight_bounds, params, proposal.context_bounds);
            !failed.ok()) {
            return failed;
        }
        proposal.oriented_bounds = minimum_oriented_rect(convex_hull(endpoints));
        proposal.supporting_segments.reserve(count);
        for (const size_t k : members) {
            proposal.supporting_segments.push_back(prepared[k].segment);
        }
        proposals.push_back(std::move(proposal));
    }
    return proposals;
}

}  // namespace mirador
