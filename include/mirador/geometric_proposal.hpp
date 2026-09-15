#ifndef MIRADOR_GEOMETRIC_PROPOSAL_HPP
#define MIRADOR_GEOMETRIC_PROPOSAL_HPP

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mirador {

/// Experimental (M6): center, extents and edge angle of an oriented rectangle
/// (see `GeometricRegionProposal::oriented_bounds`).
struct OrientedRect {
    PointF center;
    float width = 0.0F;
    float height = 0.0F;
    /// Rotation of the width axis in degrees, normalized to [0, 180).
    float angle_deg = 0.0F;
};

/// Experimental (M6, DEC-017): the geometric region proposal contract is under
/// validation and MAY change without a compatibility notice; it is not covered
/// by the M0-M5 compatibility register. Promotion to a supported capability
/// requires a dedicated decision record (DEC-017 promotion gate).
///
/// A proposal is a closed or near-closed structure of line segments (issue #11
/// hypothesis) described by geometric-completeness evidence only: it never
/// carries semantic labels. Final semantics remain the job of OCR, Detector,
/// Accessibility and Evidence Fusion. Bounds are expressed in the same
/// coordinate space as the input segments; callers transform segments before
/// calling and map bounds onwards (RULE-05).
struct GeometricRegionProposal {
    /// Axis-aligned bounds covering the structure's endpoints exactly (no
    /// padding). Filled from the supporting segment geometry.
    RectI tight_bounds;
    /// `tight_bounds` expanded for downstream semantic consumers (OCR,
    /// detector refinement): each side grows by clamping
    /// `context_ratio * extent` into [min_context_padding_px,
    /// max_context_padding_px] (x from width, y from height). Not clamped to
    /// any frame: callers intersect with their frame ROI.
    RectI context_bounds;
    /// Degree of closure in [0, 1]. 1.0 when the structure's endpoint-junction
    /// graph contains at least one cycle; otherwise, when exactly two dangling
    /// junctions exist, `1 - gap / diagonal` (gap between the dangling
    /// junctions, diagonal of the structure's endpoint bounding box); 0
    /// otherwise (open chains, T/Y junctions).
    float closure_score = 0.0F;
    /// Fraction of total segment length whose orientation lies within
    /// `angle_tolerance_deg` of the structure's two principal directions
    /// (length-weighted circular mean over doubled angles). 1.0 for
    /// rectangular layouts, low for free-form bundles.
    float rectangularity = 0.0F;
    /// Fraction of supporting segments that share at least one junction with
    /// another segment (endpoint merged within `endpoint_radius_px`).
    float edge_support = 0.0F;
    /// Minimum-area oriented rectangle of the structure's endpoints (rotating
    /// calipers on the convex hull): preserves direction information for
    /// geometry-level matching; axis-aligned consumers use `tight_bounds`.
    /// Degenerate structures (collinear endpoints) collapse in extent.
    OrientedRect oriented_bounds;
    /// The segments forming the structure, in ascending input order.
    std::vector<LineSegment> supporting_segments;
};

/// Tuning and budgets for `propose_regions` (DEC-017: every limit is explicit;
/// exceeding a budget is a visible error, never silent truncation).
struct GeometricProposalParams {
    /// Endpoint pairs within this distance merge into one junction. > 0.
    double endpoint_radius_px = 4.0;
    /// Orientation tolerance for the principal-axis alignment test of
    /// `rectangularity`. In (0, 90].
    double angle_tolerance_deg = 20.0;
    /// Minimum `closure_score` for a structure to become a proposal. In [0, 1].
    float min_closure_score = 0.75F;
    /// Structures with fewer supporting segments are never proposed (>= 1; a
    /// closed structure needs at least 3).
    int32_t min_segments = 3;
    /// Input budget: more segments fail with kBudgetExceeded (the endpoint
    /// linking pass is quadratic, RULE-06). >= 1.
    int32_t max_segments = 1024;
    /// Output budget: more qualifying structures fail with kBudgetExceeded.
    /// >= 1.
    int32_t max_proposals = 64;
    /// Context padding ratio relative to the tight extent (x from width, y
    /// from height). >= 0.
    double context_ratio = 0.25;
    /// Context padding lower bound in pixels. >= 0.
    double min_context_padding_px = 8.0;
    /// Context padding upper bound in pixels; >= min_context_padding_px.
    double max_context_padding_px = 64.0;
};

/// Groups line segments into closed or near-closed structures and proposes one
/// region per structure (design section 24 M6, issue #11). Pure function:
/// stateless, deterministic for equal inputs (bit-stable output order by the
/// smallest supporting-segment input index), never modifies the input, no
/// internal concurrency. Non-finite segment coordinates fail the whole call;
/// zero-length segments are dropped (consistent with `merge_collinear`); no
/// partial results on error. Endpoint-based analysis only: contacts between
/// segment bodies (T junctions through a segment's interior) do not link
/// structures — boundary outlines meet at their endpoints.
///
/// Errors: kInvalidArgument for invalid params or non-finite segment data,
/// kBudgetExceeded when the input exceeds `max_segments` or the qualifying
/// structures exceed `max_proposals`, kCancelled/kTimeout when `context`
/// reports them (polled on the quadratic passes). Never throws.
[[nodiscard]] Result<std::vector<GeometricRegionProposal>> propose_regions(std::span<const LineSegment> segments,
                                                                           const GeometricProposalParams& params,
                                                                           const ExecutionContext& context);

}  // namespace mirador

#endif  // MIRADOR_GEOMETRIC_PROPOSAL_HPP
