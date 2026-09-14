#include <mirador/segment_growing_line_detector.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>
#include "mirador/backend_info.hpp"
#include "mirador/execution_context.hpp"
#include "mirador/geometry.hpp"
#include "mirador/image_view.hpp"
#include "mirador/line_detector.hpp"
#include "mirador/pixel_format.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"
#include "mirador/transform.hpp"

namespace mirador {
namespace {

/// Interior pixel of the view; borders lack the central-difference neighbors.
[[nodiscard]] uint8_t pixel_at(const ImageView& view, int32_t x, int32_t y) noexcept {
    return std::to_integer<uint8_t>(*(view.data + static_cast<int64_t>(y) * view.row_stride_bytes + x));
}

struct Gradient {
    int32_t gx = 0;
    int32_t gy = 0;
    int64_t square = 0;  // gx*gxx + gy*gy
    int64_t l1 = 0;      // |gx| + |gy|
};

[[nodiscard]] Gradient gradient_at(const ImageView& view, int32_t x, int32_t y) noexcept {
    const int32_t gx = static_cast<int32_t>(pixel_at(view, x + 1, y)) - static_cast<int32_t>(pixel_at(view, x - 1, y));
    const int32_t gy = static_cast<int32_t>(pixel_at(view, x, y + 1)) - static_cast<int32_t>(pixel_at(view, x, y - 1));
    const int64_t sq = static_cast<int64_t>(gx) * gx + static_cast<int64_t>(gy) * gy;
    return Gradient{gx, gy, sq, std::abs(static_cast<int64_t>(gx)) + std::abs(static_cast<int64_t>(gy))};
}

[[nodiscard]] bool is_valid(const SegmentGrowingParams& params) noexcept {
    if (params.gradient_threshold < 1 || params.gradient_threshold > 510) {
        return false;
    }
    if (!(params.angle_tolerance_deg > 0.0) || params.angle_tolerance_deg > 90.0 ||
        !std::isfinite(params.angle_tolerance_deg)) {
        return false;
    }
    if (!(params.deviation_tolerance > 0.0) || params.deviation_tolerance > 1e4 ||
        !std::isfinite(params.deviation_tolerance)) {
        return false;
    }
    if (!(params.min_length >= 0.0) || params.min_length > 1e6 || !std::isfinite(params.min_length)) {
        return false;
    }
    if (params.max_segments < 1 || params.max_segments > 65535) {
        return false;
    }
    return params.work_budget_bytes >= 1024;
}

/// Pixel coordinate collected during region growth.
struct SeedPoint {
    int32_t x = 0;
    int32_t y = 0;
};

/// A grown region's pixels, kept in discovery order (deterministic scan).
using Region = std::vector<SeedPoint>;

/// Closed-form PCA direction of the point cloud (unit length or an axis).
struct FitDirection {
    double dx = 1.0;
    double dy = 0.0;
    double mx = 0.0;  // centroid
    double my = 0.0;
};

[[nodiscard]] FitDirection fit_direction(const Region& pixels) noexcept {
    FitDirection fit;
    const auto n = static_cast<double>(pixels.size());
    double sx = 0.0;
    double sy = 0.0;
    for (const SeedPoint& p : pixels) {
        sx += p.x;
        sy += p.y;
    }
    fit.mx = sx / n;
    fit.my = sy / n;
    double cxx = 0.0;
    double cyy = 0.0;
    double cxy = 0.0;
    for (const SeedPoint& p : pixels) {
        const double dx = p.x - fit.mx;
        const double dy = p.y - fit.my;
        cxx += dx * dx;
        cyy += dy * dy;
        cxy += dx * dy;
    }
    cxx /= n;
    cyy /= n;
    cxy /= n;
    // Largest eigenvalue of the 2x2 covariance; eigenvector (lambda - cyy, cxy)
    // for the symmetric matrix [[cxx, cxy], [cxy, cyy]].
    const double lambda = (cxx + cyy) / 2.0 + std::sqrt((cxx - cyy) * (cxx - cyy) / 4.0 + cxy * cxy);
    double dx = lambda - cyy;
    double dy = cxy;
    if (std::fabs(dx) < 1e-12 && std::fabs(dy) < 1e-12) {
        dx = cxx >= cyy ? 1.0 : 0.0;
        dy = cxx >= cyy ? 0.0 : 1.0;
    }
    const double norm = std::sqrt(dx * dx + dy * dy);
    fit.dx = dx / norm;
    fit.dy = dy / norm;
    return fit;
}

struct Projected {
    double t = 0.0;
    double residual = 0.0;
    size_t index = 0;
};

/// Projects the region onto the fit direction; indices refer to `pixels`.
[[nodiscard]] std::vector<Projected> project(const Region& pixels, const FitDirection& fit) noexcept {
    std::vector<Projected> projected;
    projected.reserve(pixels.size());
    for (size_t i = 0; i < pixels.size(); ++i) {
        const double dx = pixels[i].x - fit.mx;
        const double dy = pixels[i].y - fit.my;
        Projected p;
        p.t = dx * fit.dx + dy * fit.dy;
        p.residual = std::fabs(dx * fit.dy - dy * fit.dx);
        p.index = i;
        projected.push_back(p);
    }
    std::sort(projected.begin(), projected.end(), [&pixels](const Projected& a, const Projected& b) {
        if (a.t != b.t) {
            return a.t < b.t;
        }
        const SeedPoint& pa = pixels[a.index];
        const SeedPoint& pb = pixels[b.index];
        if (pa.x != pb.x) {
            return pa.x < pb.x;
        }
        return pa.y < pb.y;
    });
    return projected;
}

/// Maximum t-gap between neighboring inliers of one run.
constexpr double kRunGapTolerance = 1.5;

/// One-bit-per-pixel visited map; the size was budget-checked by the caller.
class VisitedMap {
public:
    VisitedMap(const ImageView& view, std::vector<uint8_t>& storage) : view_(view), storage_(storage) {}

    void mark(int32_t x, int32_t y) noexcept {
        const size_t bit = bit_index(x, y);
        storage_[bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));
    }

    [[nodiscard]] bool is_marked(int32_t x, int32_t y) const noexcept {
        const size_t bit = bit_index(x, y);
        return (storage_[bit / 8] & static_cast<uint8_t>(1U << (bit % 8))) != 0;
    }

private:
    [[nodiscard]] size_t bit_index(int32_t x, int32_t y) const noexcept {
        return static_cast<size_t>(y) * static_cast<size_t>(view_.width) + static_cast<size_t>(x);
    }

    const ImageView& view_;
    std::vector<uint8_t>& storage_;
};

/// The inclusive interior of `scan` inside the view: central differences need
/// both neighbors, so the outermost pixel ring never seeds or grows.
[[nodiscard]] RectI interior_of(const RectI& scan, const ImageView& view) noexcept {
    const int32_t left = std::max(scan.x, 1);
    const int32_t top = std::max(scan.y, 1);
    const int32_t right = std::min(scan.x + scan.width, view.width - 1);
    const int32_t bottom = std::min(scan.y + scan.height, view.height - 1);
    return RectI{left, top, right - left, bottom - top};
}

/// Grows one 8-connected, seed-gradient-aligned region from `seed`
/// (deterministic DFS with an explicit stack), recording pixels in
/// `region` in discovery order.
void grow_region(const ImageView& view, const RectI& interior, const SeedPoint& seed, const Gradient& seed_gradient,
                 double sin_sq, const SegmentGrowingParams& params, VisitedMap& visited, Region& region) {
    region.clear();
    region.push_back(seed);
    visited.mark(seed.x, seed.y);
    Region stack;
    stack.push_back(seed);
    while (!stack.empty()) {
        const SeedPoint current = stack.back();
        stack.pop_back();
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const int32_t nx = current.x + dx;
                const int32_t ny = current.y + dy;
                if (nx < interior.x || nx >= interior.x + interior.width || ny < interior.y ||
                    ny >= interior.y + interior.height || visited.is_marked(nx, ny)) {
                    continue;
                }
                const Gradient g = gradient_at(view, nx, ny);
                if (g.l1 < params.gradient_threshold) {
                    continue;
                }
                // |sin(angle between gradients)| <= sin(tolerance):
                // cross^2 <= |a|^2 |b|^2 sin^2(tolerance).
                const int64_t cross =
                    static_cast<int64_t>(seed_gradient.gx) * g.gy - static_cast<int64_t>(seed_gradient.gy) * g.gx;
                if (static_cast<double>(cross * cross) >
                    static_cast<double>(seed_gradient.square) * static_cast<double>(g.square) * sin_sq) {
                    continue;
                }
                visited.mark(nx, ny);
                stack.push_back(SeedPoint{nx, ny});
                region.push_back(SeedPoint{nx, ny});
            }
        }
    }
}

/// Fits one contiguous inlier run and appends the segment when it passes the
/// length and confidence gates. Returns kBudgetExceeded past the segment cap.
[[nodiscard]] Status emit_run(const Region& run, const LineDetectRequest& request, const SegmentGrowingParams& params,
                              LineSegmentSet& result) {
    const FitDirection run_fit = fit_direction(run);
    const std::vector<Projected> run_projected = project(run, run_fit);
    const double length = run_projected.back().t - run_projected.front().t;
    double residual_sum = 0.0;
    for (const Projected& p : run_projected) {
        residual_sum += p.residual;
    }
    const double mean_residual = residual_sum / static_cast<double>(run_projected.size());
    const double confidence = std::max(0.0, std::min(1.0, 1.0 - mean_residual / params.deviation_tolerance));
    if (length < params.min_length || confidence < static_cast<double>(request.min_confidence)) {
        return Status::success();
    }
    if (result.segments.size() >= static_cast<size_t>(params.max_segments)) {
        return {ErrorCode::kBudgetExceeded, "segment cap exceeded"};
    }
    LineSegment segment;
    segment.begin.x = static_cast<float>(run_fit.mx + run_fit.dx * run_projected.front().t);
    segment.begin.y = static_cast<float>(run_fit.my + run_fit.dy * run_projected.front().t);
    segment.end.x = static_cast<float>(run_fit.mx + run_fit.dx * run_projected.back().t);
    segment.end.y = static_cast<float>(run_fit.my + run_fit.dy * run_projected.back().t);
    segment.confidence = static_cast<float>(confidence);
    result.segments.push_back(segment);
    return Status::success();
}

/// Fits `region` and splits it into contiguous inlier runs; every run is
/// refit and gated independently.
[[nodiscard]] Status process_region(const Region& region, const LineDetectRequest& request,
                                    const SegmentGrowingParams& params, LineSegmentSet& result) {
    const FitDirection fit = fit_direction(region);
    const std::vector<Projected> projected = project(region, fit);
    const auto inlier = [&params](const Projected& p) { return p.residual <= params.deviation_tolerance; };

    size_t run_start = 0;
    while (run_start < projected.size()) {
        if (!inlier(projected[run_start])) {
            ++run_start;
            continue;
        }
        size_t run_end = run_start + 1;  // exclusive
        while (run_end < projected.size() && inlier(projected[run_end]) &&
               projected[run_end].t - projected[run_end - 1].t <= kRunGapTolerance) {
            ++run_end;
        }
        if (run_end - run_start >= 2) {
            Region run;
            run.reserve(run_end - run_start);
            for (size_t i = run_start; i < run_end; ++i) {
                run.push_back(region[projected[i].index]);
            }
            if (const Status status = emit_run(run, request, params, result); !status.ok()) {
                return status;
            }
        }
        run_start = run_end;
    }
    return Status::success();
}

/// Context polls for pre-checks and stage boundaries; messages are generic
/// because callers act on the Status code.
[[nodiscard]] Status context_status(const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "deadline reached"};
    }
    return Status::success();
}

/// Validates the request, view and ROI; returns the scan rectangle.
[[nodiscard]] Result<RectI> resolve_scan(const SegmentGrowingParams& params, const ImageView& gray,
                                         const LineDetectRequest& request) noexcept {
    if (!is_valid(params)) {
        return Status(ErrorCode::kInvalidArgument, "detector parameters out of range");
    }
    if (request.min_confidence < 0.0F || request.min_confidence > 1.0F) {
        return Status(ErrorCode::kInvalidArgument, "min_confidence must lie in [0, 1]");
    }
    if (!validate(gray).ok()) {
        return Status(ErrorCode::kInvalidArgument, "invalid input view");
    }
    if (gray.format != PixelFormat::kGray8) {
        return Status(ErrorCode::kUnsupportedFormat, "line detection requires kGray8");
    }
    if (!request.roi.has_value()) {
        return RectI{0, 0, gray.width, gray.height};
    }
    const RectI scan = *request.roi;
    if (!is_valid(scan)) {
        return Status(ErrorCode::kInvalidArgument, "invalid ROI rectangle");
    }
    if (!contains(RectI{0, 0, gray.width, gray.height}, scan)) {
        return Status(ErrorCode::kInvalidArgument, "ROI outside the view");
    }
    return scan;
}

}  // namespace

SegmentGrowingLineDetector::SegmentGrowingLineDetector(SegmentGrowingParams params) noexcept : params_(params) {}

BackendInfo SegmentGrowingLineDetector::info() const {
    BackendInfo info;
    info.name = "mirador-segment-growing";
    info.implementation_version = "1.0.0";
    info.model_id.clear();  // model-free traditional detector
    info.model_revision.clear();
    info.accepted_formats = {PixelFormat::kGray8};
    info.thread_safe = true;  // detect() is stateless
    return info;
}

Result<LineSegmentSet> SegmentGrowingLineDetector::detect(const ImageView& gray, const LineDetectRequest& request,
                                                          const ExecutionContext& context) {
    if (const Status stopped = context_status(context); !stopped.ok()) {
        return stopped;
    }
    auto scan = resolve_scan(params_, gray, request);
    if (!scan.ok()) {
        return Status(scan.status().code(), scan.status().message());
    }
    const int64_t bit_bytes = (static_cast<int64_t>(gray.width) * gray.height + 7) / 8;
    if (bit_bytes > params_.work_budget_bytes) {
        return Status(ErrorCode::kBudgetExceeded, "visited bitmap exceeds the work budget");
    }

    std::vector<uint8_t> storage(static_cast<size_t>(bit_bytes), 0);
    VisitedMap visited(gray, storage);
    const RectI interior = interior_of(scan.value(), gray);
    const double sin_tolerance = std::sin(params_.angle_tolerance_deg * std::numbers::pi / 180.0);
    const double sin_sq = sin_tolerance * sin_tolerance;

    LineSegmentSet result;
    result.space = CoordinateSpaceId::kOriented;
    Region region;
    for (int32_t y = interior.y; y < interior.y + interior.height; ++y) {
        if (const Status stopped = context_status(context); !stopped.ok()) {
            return stopped;
        }
        for (int32_t x = interior.x; x < interior.x + interior.width; ++x) {
            if (visited.is_marked(x, y)) {
                continue;
            }
            const Gradient seed_gradient = gradient_at(gray, x, y);
            if (seed_gradient.l1 < params_.gradient_threshold) {
                continue;
            }
            grow_region(gray, interior, SeedPoint{x, y}, seed_gradient, sin_sq, params_, visited, region);
            if (region.size() < 2) {
                continue;
            }
            if (const Status status = process_region(region, request, params_, result); !status.ok()) {
                return status;
            }
        }
    }
    return result;
}

}  // namespace mirador
