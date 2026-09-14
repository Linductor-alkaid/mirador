#ifndef MIRADOR_SEGMENT_GROWING_LINE_DETECTOR_HPP
#define MIRADOR_SEGMENT_GROWING_LINE_DETECTOR_HPP

#include <mirador/line_detector.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador {

/// Tunables of the first-party segment-growing line detector (DEC-009, design
/// section 15). The detector extracts gradient edge ridges (central
/// differences, L1 magnitude), grows direction-aligned regions from scan-order
/// seeds (8-connectivity, alignment to the seed gradient mod 180), fits one
/// line per region by closed-form PCA, and splits regions whose orthogonal
/// residuals exceed `deviation_tolerance` into contiguous inlier runs. A thin
/// bright line therefore produces one ridge segment per side; callers unify
/// them with `merge_collinear` when needed.
struct SegmentGrowingParams {
    /// L1 gradient magnitude (|gx| + |gy|, values 0..510) at or above which a
    /// pixel counts as an edge pixel, in [1, 510].
    int32_t gradient_threshold = 32;
    /// Maximum gradient-direction difference (mod 180) inside one region, in
    /// degrees, in (0, 90].
    double angle_tolerance_deg = 15.0;
    /// Maximum orthogonal residual (pixels) of an inlier after the line fit,
    /// in (0, 1e4].
    double deviation_tolerance = 1.5;
    /// Minimum accepted segment length (pixels); shorter runs are dropped.
    /// In [0, 1e6].
    double min_length = 8.0;
    /// Maximum number of returned segments; scanning past the cap fails with
    /// kBudgetExceeded (RULE-06), in [1, 65535].
    int32_t max_segments = 1024;
    /// Budget for the detector's own working memory (the visited bitmap, one
    /// bit per scanned pixel); must leave room for the ROI area, in
    /// [1024, INT64_MAX].
    int64_t work_budget_bytes = int64_t{4} * 1024 * 1024;
};

/// First-party deterministic line detector (design sections 15, 24 M3; the
/// "equivalent implementation" of SCOPE-04 per DEC-009). Pure C++20: integer
/// gradients and growth, closed-form double-precision PCA fits (sqrt only, no
/// iterative optimization on the hot path). Output segments are deterministic
/// for equal inputs and reported in presented view pixel space regardless of
/// the request ROI. The detector is stateless: `detect` keeps no state between
/// calls, so instances may be shared across threads.
class SegmentGrowingLineDetector final : public LineDetector {
public:
    SegmentGrowingLineDetector() noexcept = default;
    explicit SegmentGrowingLineDetector(SegmentGrowingParams params) noexcept;

    [[nodiscard]] BackendInfo info() const override;

    /// Detects segments on a grayscale view. Errors: kInvalidArgument (invalid
    /// view, out-of-range parameters or ROI outside the view),
    /// kUnsupportedFormat (non-gray view), kCancelled/kTimeout (context),
    /// kBudgetExceeded (more than `max_segments` segments found, or the
    /// visited bitmap exceeding `work_budget_bytes`). Never throws.
    Result<LineSegmentSet> detect(const ImageView& gray, const LineDetectRequest& request,
                                  const ExecutionContext& context) override;

private:
    SegmentGrowingParams params_;
};

}  // namespace mirador

#endif  // MIRADOR_SEGMENT_GROWING_LINE_DETECTOR_HPP
