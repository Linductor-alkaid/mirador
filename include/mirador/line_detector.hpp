#ifndef MIRADOR_LINE_DETECTOR_HPP
#define MIRADOR_LINE_DETECTOR_HPP

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/transform.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mirador {

/// Unified line-segment output (design section 15): a set of straight segments
/// in one declared coordinate space. Concrete detectors (first-party growing
/// detector, optional ELSED adapter) all produce this type; consumers stay
/// detector-agnostic.
struct LineSegmentSet {
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;
    std::vector<LineSegment> segments;
};

/// Line detection request (design section 15). The view-space ROI narrows the
/// scanned pixels; `min_confidence` gates returned segments; `backend_params`
/// carries opaque implementation-private parameters, mirroring the Backend SPI
/// request layering (DEC-012). Mirador never interprets `backend_params`.
struct LineDetectRequest {
    /// Pixel-space ROI inside the presented view; nullopt scans the whole view.
    std::optional<RectI> roi;
    float min_confidence = 0.0F;
    std::string backend_params;
};

/// Synchronous line detection SPI (design sections 9, 15). Implementations own
/// the algorithm; Mirador owns the unified output model, filtering utilities and
/// coordinate transformation. The same execution contract as OcrBackend applies:
/// implementations must honor `request.min_confidence`, poll `context` and
/// return kCancelled/kTimeout explicitly, stay deterministic for equal inputs,
/// and never modify the input pixels. Segments are reported in presented view
/// pixel space; callers recover them with `transform_segment` (RULE-05).
class LineDetector {
public:
    virtual ~LineDetector() = default;

    /// Capability and identity query, mirroring BackendInfo (DEC-012). The
    /// accepted_formats list must contain exactly kGray8: the SPI input is a
    /// grayscale view.
    [[nodiscard]] virtual BackendInfo info() const = 0;

    /// Detects line segments on a grayscale view (kGray8 required; any other
    /// format returns kUnsupportedFormat — color conversion belongs to
    /// mirador::image on the caller side).
    virtual Result<LineSegmentSet> detect(const ImageView& gray, const LineDetectRequest& request,
                                          const ExecutionContext& context) = 0;
};

/// Euclidean length of a segment (double arithmetic; never negative).
[[nodiscard]] double segment_length(const LineSegment& segment) noexcept;

/// Segment orientation in degrees, normalized to [0, 180): direction-independent
/// (a segment and its reversal share one orientation). Computed with
/// std::atan2 in double precision; angles within the last ulp of a caller
/// threshold may classify differently across libm implementations, so callers
/// should keep tolerances away from exact boundary values.
[[nodiscard]] double segment_angle_deg(const LineSegment& segment) noexcept;

/// Inclusive linear filters for a set of segments (design section 15). A segment
/// is kept when every active bound holds:
/// - `min_length` <= length (active when > 0);
/// - length <= `max_length` (active when > 0);
/// - orientation inside the [min_angle_deg, max_angle_deg] window; a window with
///   min > max wraps around 0/180 (for example 170..10 keeps near-horizontal
///   segments through the 180-degree seam);
/// - confidence >= `min_confidence` (active when > 0).
/// Output preserves input order.
///
/// Errors: kInvalidArgument for negative bounds, angle bounds outside [0, 180],
/// NaN values or a confidence bound outside [0, 1]. Never throws.
struct LineFilterParams {
    double min_length = 0.0;
    double max_length = 0.0;  ///< 0 = unbounded
    double min_angle_deg = 0.0;
    double max_angle_deg = 180.0;
    float min_confidence = 0.0F;
};

[[nodiscard]] Result<std::vector<LineSegment>> filter_segments(std::span<const LineSegment> segments,
                                                               const LineFilterParams& params);

/// Merges collinear segments (design section 15): two segments merge when their
/// orientations differ by at most `angle_tolerance_deg` (mod 180), each is
/// within `distance_tolerance` pixels of the other's infinite line, and the gap
/// between their projections on the merged direction is at most
/// `gap_tolerance` (overlaps merge regardless of `gap_tolerance`). Merging is
/// transitive: the result absorbs repeatedly until fixpoint. Merged endpoints
/// span the union interval projected on the seed direction; confidence is the
/// maximum of the absorbed segments. Output order follows the first (lowest
/// index) absorbed segment; input is never modified. Segments with zero length
/// are dropped from the output.
///
/// Errors: kBudgetExceeded when `segments` exceeds kMaxMergeSegments (the
/// quadratic fixpoint needs a bounded input, RULE-06). Never throws.
struct CollinearMergeParams {
    double angle_tolerance_deg = 2.0;
    double distance_tolerance = 1.5;
    double gap_tolerance = 4.0;
};

inline constexpr size_t kMaxMergeSegments = 4096;

[[nodiscard]] Result<std::vector<LineSegment>> merge_collinear(std::span<const LineSegment> segments,
                                                               const CollinearMergeParams& params);

}  // namespace mirador

#endif  // MIRADOR_LINE_DETECTOR_HPP
