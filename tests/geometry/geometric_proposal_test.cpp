// M6 (DEC-017): unit tests for the experimental geometric region proposal
// contract (design section 24, issue #11). The header
// <mirador/geometric_proposal.hpp> is the authoritative contract.

#include <mirador/execution_context.hpp>
#include <mirador/geometric_proposal.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <vector>
#include "mirador/geometry.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"

namespace {

using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::GeometricProposalParams;
using mirador::GeometricRegionProposal;
using mirador::LineSegment;
using mirador::OrientedRect;
using mirador::PointF;
using mirador::RectI;
using mirador::Result;

/// Four-segment closed rectangle outline with unit confidence.
std::vector<LineSegment> rectangle_chain(float x, float y, float width, float height) {
    const PointF a{x, y};
    const PointF b{x + width, y};
    const PointF c{x + width, y + height};
    const PointF d{x, y + height};
    return {LineSegment{a, b, 1.0F}, LineSegment{b, c, 1.0F}, LineSegment{c, d, 1.0F}, LineSegment{d, a, 1.0F}};
}

constexpr double nan_double() {
    return std::numeric_limits<double>::quiet_NaN();
}

constexpr float nan_float() {
    return std::numeric_limits<float>::quiet_NaN();
}

void expect_rejected(ErrorCode code, const std::vector<LineSegment>& segments, const GeometricProposalParams& params,
                     const ExecutionContext& context) {
    const Result<std::vector<GeometricRegionProposal>> result = propose_regions(segments, params, context);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), code);
}

/// Bit-stable contract: equal inputs must produce identical proposals, so all
/// floating point fields compare with exact equality.
bool identical_proposals(const std::vector<GeometricRegionProposal>& left,
                         const std::vector<GeometricRegionProposal>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].tight_bounds != right[i].tight_bounds || left[i].context_bounds != right[i].context_bounds ||
            left[i].closure_score != right[i].closure_score || left[i].rectangularity != right[i].rectangularity ||
            left[i].edge_support != right[i].edge_support ||
            left[i].oriented_bounds.center != right[i].oriented_bounds.center ||
            left[i].oriented_bounds.width != right[i].oriented_bounds.width ||
            left[i].oriented_bounds.height != right[i].oriented_bounds.height ||
            left[i].oriented_bounds.angle_deg != right[i].oriented_bounds.angle_deg ||
            left[i].supporting_segments != right[i].supporting_segments) {
            return false;
        }
    }
    return true;
}

/// Tight bounds are [floor(min), ceil(max)), so an endpoint exactly on an
/// integral right/bottom edge lies on the exclusive boundary but is still
/// covered by the half-open pixel-area convention.
bool endpoint_covered_by(const RectI& rect, const PointF& point) {
    if (mirador::contains(rect, point)) {
        return true;
    }
    const double right = static_cast<double>(rect.x) + static_cast<double>(rect.width);
    const double bottom = static_cast<double>(rect.y) + static_cast<double>(rect.height);
    const auto px = static_cast<double>(point.x);
    const auto py = static_cast<double>(point.y);
    return px >= static_cast<double>(rect.x) && px <= right && py >= static_cast<double>(rect.y) && py <= bottom;
}

/// Star-shaped closed ring: `sides` vertices at strictly ordered jittered
/// angles (radii >= 30 px) chained with exact shared endpoints and closed back
/// onto the first vertex. Adjacent vertices stay farther apart than the
/// default 4 px junction radius.
std::vector<LineSegment> closed_ring(uint32_t seed, int sides) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> radius(30.0F, 120.0F);
    std::uniform_real_distribution<double> jitter(0.3, 0.7);
    const double slot = 2.0 * std::numbers::pi / static_cast<double>(sides);
    std::vector<PointF> vertices;
    vertices.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        const double angle = slot * (static_cast<double>(i) + jitter(rng));
        const float r = radius(rng);
        vertices.push_back(PointF{static_cast<float>(300.0 + (static_cast<double>(r) * std::cos(angle))),
                                  static_cast<float>(250.0 + (static_cast<double>(r) * std::sin(angle)))});
    }
    std::vector<LineSegment> segments;
    segments.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        segments.push_back(LineSegment{vertices[static_cast<std::size_t>(i)],
                                       vertices[static_cast<std::size_t>((i + 1) % sides)], 1.0F});
    }
    return segments;
}

PointF quarter_turn(PointF point, int quarter_turns) {
    switch (quarter_turns % 4) {
        case 1:
            return PointF{-point.y, point.x};
        case 2:
            return PointF{-point.x, -point.y};
        case 3:
            return PointF{point.y, -point.x};
        default:
            return point;
    }
}

/// Contract shape check shared by the rectangle tests: a rectangle has two
/// minimum-area orientations (the calipers tie keeps the first hull edge), so
/// either width-axis assignment is valid, but the dimensions, area and center
/// must match within tolerance and the angle must equal one of the two edge
/// orientations.
void expect_oriented_rectangle(const OrientedRect& oriented, float edge_a_deg, float edge_b_deg, float long_side,
                               float short_side, float center_x, float center_y) {
    const bool dims_match =
        (std::fabs(oriented.width - long_side) <= 0.5F && std::fabs(oriented.height - short_side) <= 0.5F) ||
        (std::fabs(oriented.width - short_side) <= 0.5F && std::fabs(oriented.height - long_side) <= 0.5F);
    EXPECT_TRUE(dims_match) << oriented.width << " x " << oriented.height;
    const bool angle_matches =
        std::fabs(oriented.angle_deg - edge_a_deg) <= 0.5F || std::fabs(oriented.angle_deg - edge_b_deg) <= 0.5F;
    EXPECT_TRUE(angle_matches) << oriented.angle_deg;
    EXPECT_NEAR(oriented.width * oriented.height, long_side * short_side, 2.0F);
    EXPECT_NEAR(oriented.center.x, center_x, 0.5F);
    EXPECT_NEAR(oriented.center.y, center_y, 0.5F);
}

/// One rotation step of the DOD-03 matrix: builds the quarter-turned odd-sized
/// rectangle chain and asserts the hand-computed tight box.
void expect_quarter_turn_tight_bounds(const std::array<PointF, 4>& base, int turns, const RectI& expected) {
    SCOPED_TRACE(turns);
    std::vector<LineSegment> segments;
    segments.reserve(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        const PointF a = quarter_turn(base[i], turns);
        const PointF b = quarter_turn(base[(i + 1) % base.size()], turns);
        segments.push_back(LineSegment{PointF{a.x + 200.0F, a.y + 200.0F}, PointF{b.x + 200.0F, b.y + 200.0F}, 1.0F});
    }
    const auto result = propose_regions(segments, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].tight_bounds, expected);
    EXPECT_FLOAT_EQ(result.value()[0].closure_score, 1.0F);
    EXPECT_EQ(result.value()[0].supporting_segments.size(), 4U);
}

/// Every ring endpoint must sit inside the proposal's tight bounds (half-open
/// convention aside).
void expect_ring_endpoints_covered(const RectI& tight_bounds, const std::vector<LineSegment>& ring) {
    for (const LineSegment& segment : ring) {
        EXPECT_TRUE(endpoint_covered_by(tight_bounds, segment.begin));
        EXPECT_TRUE(endpoint_covered_by(tight_bounds, segment.end));
    }
}

/// One random-ring step: the ring must yield exactly one fully closed proposal
/// whose tight bounds cover every endpoint, and the call must be bit-stable.
void expect_ring_is_covered_and_stable(const std::vector<LineSegment>& ring, int sides) {
    const auto first = propose_regions(ring, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().size(), 1U);
    EXPECT_EQ(first.value()[0].supporting_segments.size(), static_cast<std::size_t>(sides));
    EXPECT_FLOAT_EQ(first.value()[0].closure_score, 1.0F);
    expect_ring_endpoints_covered(first.value()[0].tight_bounds, ring);
    const auto second = propose_regions(ring, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(identical_proposals(first.value(), second.value()));
}

TEST(GeometricProposalParams, RejectsInvalidParamsWithInvalidArgument) {
    const std::vector<LineSegment> rectangle = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    const auto expect_invalid = [&rectangle](const GeometricProposalParams& params) {
        expect_rejected(ErrorCode::kInvalidArgument, rectangle, params, ExecutionContext{});
    };

    GeometricProposalParams params;
    params.endpoint_radius_px = 0.0;
    expect_invalid(params);
    params.endpoint_radius_px = -1.0;
    expect_invalid(params);
    params.endpoint_radius_px = nan_double();
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.angle_tolerance_deg = 0.0;
    expect_invalid(params);
    params.angle_tolerance_deg = 90.5;
    expect_invalid(params);
    params.angle_tolerance_deg = nan_double();
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.min_closure_score = -0.1F;
    expect_invalid(params);
    params.min_closure_score = 1.1F;
    expect_invalid(params);
    params.min_closure_score = nan_float();
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.min_segments = 0;
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.max_segments = 0;
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.max_proposals = 0;
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.context_ratio = -1.0;
    expect_invalid(params);
    params.context_ratio = nan_double();
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.min_context_padding_px = -1.0;
    expect_invalid(params);
    params.min_context_padding_px = nan_double();
    expect_invalid(params);

    params = GeometricProposalParams{};
    params.max_context_padding_px = 7.0;  // below the default minimum of 8
    expect_invalid(params);
    params.max_context_padding_px = nan_double();
    expect_invalid(params);
}

TEST(GeometricProposalParams, AcceptsDocumentedBoundaryValues) {
    const std::vector<LineSegment> rectangle = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    const auto expect_single = [&rectangle](const GeometricProposalParams& params) {
        const auto result = propose_regions(rectangle, params, ExecutionContext{});
        ASSERT_TRUE(result.ok());
        ASSERT_EQ(result.value().size(), 1U);
    };

    GeometricProposalParams params;
    params.angle_tolerance_deg = 90.0;  // upper bound of (0, 90]
    expect_single(params);
    params.angle_tolerance_deg = std::numeric_limits<double>::min();  // smallest positive
    expect_single(params);

    params = GeometricProposalParams{};
    params.min_closure_score = 1.0F;  // the closed rectangle scores exactly 1.0
    expect_single(params);
    params.min_closure_score = 0.0F;
    expect_single(params);

    params = GeometricProposalParams{};
    params.min_segments = 1;
    expect_single(params);

    params = GeometricProposalParams{};
    params.context_ratio = 0.0;  // falls back to the minimum padding
    expect_single(params);

    params = GeometricProposalParams{};
    params.min_context_padding_px = 0.0;
    params.max_context_padding_px = 0.0;  // min == max is allowed
    expect_single(params);

    params = GeometricProposalParams{};
    params.endpoint_radius_px = 1e-6;  // exact corner contact still merges
    expect_single(params);
}

TEST(GeometricProposalInput, EmptyInputSucceedsWithNoProposals) {
    const auto result = propose_regions({}, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().empty());
}

TEST(GeometricProposalInput, NonFiniteSegmentDataFailsWholeCall) {
    const std::vector<LineSegment> nan_coordinate{LineSegment{{0.0F, 0.0F}, {nan_float(), 0.0F}, 1.0F}};
    expect_rejected(ErrorCode::kInvalidArgument, nan_coordinate, GeometricProposalParams{}, ExecutionContext{});

    const std::vector<LineSegment> infinite_coordinate{
        LineSegment{{0.0F, 0.0F}, {std::numeric_limits<float>::infinity(), 0.0F}, 1.0F}};
    expect_rejected(ErrorCode::kInvalidArgument, infinite_coordinate, GeometricProposalParams{}, ExecutionContext{});

    const std::vector<LineSegment> nan_confidence{LineSegment{{0.0F, 0.0F}, {1.0F, 0.0F}, nan_float()}};
    expect_rejected(ErrorCode::kInvalidArgument, nan_confidence, GeometricProposalParams{}, ExecutionContext{});

    // One poisoned member fails the whole call even inside an otherwise valid
    // rectangle: no partial results.
    std::vector<LineSegment> poisoned = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    poisoned[2].confidence = nan_float();
    expect_rejected(ErrorCode::kInvalidArgument, poisoned, GeometricProposalParams{}, ExecutionContext{});
}

TEST(GeometricProposalInput, ZeroLengthSegmentsAreDroppedWithoutError) {
    const std::vector<LineSegment> degenerate{LineSegment{{5.0F, 5.0F}, {5.0F, 5.0F}, 0.5F}};
    const auto empty = propose_regions(degenerate, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value().empty());

    // A dropped zero-length segment leaves the surrounding structure intact.
    std::vector<LineSegment> mixed = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    mixed.insert(mixed.begin() + 2, LineSegment{{5.0F, 5.0F}, {5.0F, 5.0F}, 0.5F});
    const auto result = propose_regions(mixed, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].supporting_segments, rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F));
}

TEST(GeometricProposalBudget, MaxSegmentsFailsInclusivelyAndWithoutPartialResults) {
    const std::vector<LineSegment> open_chain{LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F},
                                              LineSegment{{10.0F, 0.0F}, {10.0F, 10.0F}, 1.0F},
                                              LineSegment{{10.0F, 10.0F}, {20.0F, 10.0F}, 1.0F}};
    GeometricProposalParams params;
    params.max_segments = 2;
    expect_rejected(ErrorCode::kBudgetExceeded, open_chain, params, ExecutionContext{});

    // Exactly at the budget is not an error.
    params.max_segments = 3;
    const auto at_limit = propose_regions(open_chain, params, ExecutionContext{});
    ASSERT_TRUE(at_limit.ok());
    EXPECT_TRUE(at_limit.value().empty());  // open chain: never a proposal
}

TEST(GeometricProposalBudget, MaxProposalsFailsWhenQualifyingStructuresExceedBudget) {
    std::vector<LineSegment> segments = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    const std::vector<LineSegment> far = rectangle_chain(200.0F, 100.0F, 100.0F, 50.0F);
    segments.insert(segments.end(), far.begin(), far.end());

    GeometricProposalParams params;
    params.max_proposals = 1;
    expect_rejected(ErrorCode::kBudgetExceeded, segments, params, ExecutionContext{});

    params.max_proposals = 2;
    const auto at_limit = propose_regions(segments, params, ExecutionContext{});
    ASSERT_TRUE(at_limit.ok());
    ASSERT_EQ(at_limit.value().size(), 2U);
}

TEST(GeometricProposalStructures, ClosedRectangleYieldsExactScoresAndBounds) {
    const std::vector<LineSegment> rectangle = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    const auto result = propose_regions(rectangle, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 1U);
    const GeometricRegionProposal& proposal = result.value()[0];

    EXPECT_FLOAT_EQ(proposal.closure_score, 1.0F);
    EXPECT_FLOAT_EQ(proposal.rectangularity, 1.0F);
    EXPECT_FLOAT_EQ(proposal.edge_support, 1.0F);

    EXPECT_EQ(proposal.tight_bounds, (RectI{10, 10, 100, 50}));
    // pad_x = clamp(0.25 * 100) = 25, pad_y = clamp(0.25 * 50) = 12.5 -> floor 12.
    EXPECT_EQ(proposal.context_bounds, (RectI{-15, -2, 150, 74}));

    // The calipers tie of the axis-aligned rectangle keeps the first
    // (horizontal) hull edge.
    expect_oriented_rectangle(proposal.oriented_bounds, 0.0F, 90.0F, 100.0F, 50.0F, 60.0F, 35.0F);

    EXPECT_EQ(proposal.supporting_segments, rectangle);
}

TEST(GeometricProposalStructures, NearClosedRectangleScoresGapAgainstDiagonal) {
    std::vector<LineSegment> rectangle = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    rectangle[3] = LineSegment{{10.0F, 60.0F}, {10.0F, 18.0F}, 1.0F};  // 8 px gap to (10, 10)
    const auto result = propose_regions(rectangle, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 1U);
    const float closure = result.value()[0].closure_score;
    EXPECT_GT(closure, GeometricProposalParams{}.min_closure_score);
    EXPECT_LT(closure, 1.0F);
    EXPECT_NEAR(static_cast<double>(closure), 1.0 - (8.0 / std::sqrt((100.0 * 100.0) + (50.0 * 50.0))), 1e-6);
    EXPECT_EQ(result.value()[0].supporting_segments, rectangle);

    // Tightening the gate above the achieved score suppresses the proposal.
    GeometricProposalParams stricter;
    stricter.min_closure_score = 0.95F;
    const auto suppressed = propose_regions(rectangle, stricter, ExecutionContext{});
    ASSERT_TRUE(suppressed.ok());
    EXPECT_TRUE(suppressed.value().empty());
}

TEST(GeometricProposalStructures, OpenChainUnderMinSegmentsAndJunctionlessPairsYieldNothing) {
    // Open chain: the two dangling junctions are the bounding-box diagonal
    // apart, so closure collapses to 0.
    const std::vector<LineSegment> open_chain{LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F},
                                              LineSegment{{10.0F, 0.0F}, {10.0F, 10.0F}, 1.0F},
                                              LineSegment{{10.0F, 10.0F}, {20.0F, 10.0F}, 1.0F}};
    const auto open = propose_regions(open_chain, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(open.ok());
    EXPECT_TRUE(open.value().empty());

    // A closed triangle (junction-graph cycle) is dropped by min_segments = 4.
    const std::vector<LineSegment> triangle{LineSegment{{0.0F, 0.0F}, {40.0F, 0.0F}, 1.0F},
                                            LineSegment{{40.0F, 0.0F}, {0.0F, 30.0F}, 1.0F},
                                            LineSegment{{0.0F, 30.0F}, {0.0F, 0.0F}, 1.0F}};
    GeometricProposalParams need_four;
    need_four.min_segments = 4;
    const auto too_small = propose_regions(triangle, need_four, ExecutionContext{});
    ASSERT_TRUE(too_small.ok());
    EXPECT_TRUE(too_small.value().empty());
    // The same triangle qualifies under the default gate.
    const auto qualifying = propose_regions(triangle, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(qualifying.ok());
    ASSERT_EQ(qualifying.value().size(), 1U);
    EXPECT_FLOAT_EQ(qualifying.value()[0].closure_score, 1.0F);

    // Two parallel segments sharing both junctions: fewer than three junctions
    // can never bound an area, independent of the thresholds.
    const std::vector<LineSegment> parallel_pair{LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F},
                                                 LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F}};
    GeometricProposalParams loose;
    loose.min_segments = 2;
    loose.min_closure_score = 0.0F;
    const auto pair = propose_regions(parallel_pair, loose, ExecutionContext{});
    ASSERT_TRUE(pair.ok());
    EXPECT_TRUE(pair.value().empty());
}

TEST(GeometricProposalStructures, TShapeAndLargeGapFailClosureGate) {
    // Endpoint-contact T: three dangling junctions -> closure 0.
    const std::vector<LineSegment> t_shape{LineSegment{{0.0F, 0.0F}, {50.0F, 0.0F}, 1.0F},
                                           LineSegment{{50.0F, 0.0F}, {100.0F, 0.0F}, 1.0F},
                                           LineSegment{{50.0F, 0.0F}, {50.0F, 40.0F}, 1.0F}};
    const auto t_result = propose_regions(t_shape, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(t_result.ok());
    EXPECT_TRUE(t_result.value().empty());

    // Near-closed outline with a 40 px gap: closure ~0.64 below the 0.75 gate.
    std::vector<LineSegment> gapped = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    gapped[3] = LineSegment{{10.0F, 60.0F}, {10.0F, 50.0F}, 1.0F};
    const auto gapped_result = propose_regions(gapped, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(gapped_result.ok());
    EXPECT_TRUE(gapped_result.value().empty());
}

TEST(GeometricProposalStructures, BodyContactDoesNotLinkStructures) {
    // Endpoint-only linking: the second segment's endpoint touches the first
    // segment's body but sits ~5.8 px from both of its endpoints, so the two
    // segments stay separate single-segment clusters below min_segments.
    const std::vector<LineSegment> body_contact{LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F},
                                                LineSegment{{5.0F, -3.0F}, {5.0F, 10.0F}, 1.0F}};
    const auto result = propose_regions(body_contact, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().empty());
}

TEST(GeometricProposalGeometry, ContextPaddingClampsToConfiguredBounds) {
    // 8 px sides keep the corners outside the 4 px junction radius; ratio * 8
    // = 2 below the 8 px minimum -> both pads clamp to 8.
    const auto small =
        propose_regions(rectangle_chain(0.0F, 0.0F, 8.0F, 8.0F), GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(small.ok());
    ASSERT_EQ(small.value().size(), 1U);
    EXPECT_EQ(small.value()[0].tight_bounds, (RectI{0, 0, 8, 8}));
    EXPECT_EQ(small.value()[0].context_bounds, (RectI{-8, -8, 24, 24}));

    // ratio * 2000 = 500 above the 64 px maximum -> both pads clamp to 64.
    const auto large =
        propose_regions(rectangle_chain(0.0F, 0.0F, 2000.0F, 1000.0F), GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(large.ok());
    ASSERT_EQ(large.value().size(), 1U);
    EXPECT_EQ(large.value()[0].tight_bounds, (RectI{0, 0, 2000, 1000}));
    EXPECT_EQ(large.value()[0].context_bounds, (RectI{-64, -64, 2128, 1128}));
}

TEST(GeometricProposalGeometry, TightBoundsTrackQuarterTurnsOfOddSizedRectangle) {
    const std::array<PointF, 4> base{PointF{13.0F, 7.0F}, PointF{114.0F, 7.0F}, PointF{114.0F, 58.0F},
                                     PointF{13.0F, 58.0F}};
    // Hand-computed [floor(min), ceil(max)) boxes after each quarter turn and
    // a (200, 200) shift (applied to every corner before the call).
    const std::array<RectI, 4> expected{RectI{213, 207, 101, 51}, RectI{142, 213, 51, 101}, RectI{86, 142, 101, 51},
                                        RectI{207, 86, 51, 101}};
    for (int turns = 0; turns < 4; ++turns) {
        expect_quarter_turn_tight_bounds(base, turns, expected[static_cast<std::size_t>(turns)]);
    }
}

TEST(GeometricProposalGeometry, OrientedBoundsMatchRotatedRectangle) {
    constexpr double kTheta = std::numbers::pi / 6.0;  // 30 degrees
    const auto rotate = [](PointF point) {
        return PointF{static_cast<float>((static_cast<double>(point.x) * std::cos(kTheta)) -
                                         (static_cast<double>(point.y) * std::sin(kTheta))),
                      static_cast<float>((static_cast<double>(point.x) * std::sin(kTheta)) +
                                         (static_cast<double>(point.y) * std::cos(kTheta)))};
    };
    const std::array<PointF, 4> corners{PointF{0.0F, 0.0F}, PointF{100.0F, 0.0F}, PointF{100.0F, 50.0F},
                                        PointF{0.0F, 50.0F}};
    std::vector<LineSegment> segments;
    segments.reserve(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        segments.push_back(LineSegment{rotate(corners[i]), rotate(corners[(i + 1) % corners.size()]), 1.0F});
    }
    const auto result = propose_regions(segments, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 1U);
    expect_oriented_rectangle(result.value()[0].oriented_bounds, 30.0F, 120.0F, 100.0F, 50.0F, 30.80127F, 46.65064F);
}

TEST(GeometricProposalGeometry, BoundsBeyondInt32RangeFailWithInvalidArgument) {
    // Tight box height of 2.5e9 leaves the int32 range.
    const auto tall = propose_regions(rectangle_chain(0.0F, 0.0F, 1000.0F, 2500000000.0F), GeometricProposalParams{},
                                      ExecutionContext{});
    ASSERT_FALSE(tall.ok());
    EXPECT_EQ(tall.status().code(), ErrorCode::kInvalidArgument);

    // The tight box fits (2147483520 <= INT32_MAX) but the default 64 px
    // context padding pushes the width to 2147483648, one past INT32_MAX.
    const auto wide = propose_regions(rectangle_chain(0.0F, 0.0F, 2147483520.0F, 100.0F), GeometricProposalParams{},
                                      ExecutionContext{});
    ASSERT_FALSE(wide.ok());
    EXPECT_EQ(wide.status().code(), ErrorCode::kInvalidArgument);
}

TEST(GeometricProposalGeometry, OutputOrderFollowsSmallestSupportingInputIndex) {
    std::vector<LineSegment> segments = rectangle_chain(200.0F, 100.0F, 100.0F, 50.0F);    // input indices 0-3
    const std::vector<LineSegment> second = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);  // indices 4-7
    segments.insert(segments.end(), second.begin(), second.end());

    const auto result = propose_regions(segments, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 2U);
    EXPECT_EQ(result.value()[0].tight_bounds, (RectI{200, 100, 100, 50}));
    EXPECT_EQ(result.value()[1].tight_bounds, (RectI{10, 10, 100, 50}));
    // Supporting segments list each structure's members in ascending input
    // order.
    ASSERT_EQ(result.value()[0].supporting_segments.size(), 4U);
    ASSERT_EQ(result.value()[1].supporting_segments.size(), 4U);
    EXPECT_EQ(result.value()[0].supporting_segments[0], (LineSegment{{200.0F, 100.0F}, {300.0F, 100.0F}, 1.0F}));
    EXPECT_EQ(result.value()[1].supporting_segments[0], (LineSegment{{10.0F, 10.0F}, {110.0F, 10.0F}, 1.0F}));
}

TEST(GeometricProposalDeterminism, SameInputIsBitStableAndNeverModified) {
    std::vector<LineSegment> segments = rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F);
    const std::vector<LineSegment> far = rectangle_chain(200.0F, 100.0F, 100.0F, 50.0F);
    segments.insert(segments.end(), far.begin(), far.end());
    const std::vector<LineSegment> snapshot = segments;

    const auto first = propose_regions(segments, GeometricProposalParams{}, ExecutionContext{});
    const auto second = propose_regions(segments, GeometricProposalParams{}, ExecutionContext{});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(identical_proposals(first.value(), second.value()));
    EXPECT_EQ(std::memcmp(segments.data(), snapshot.data(), segments.size() * sizeof(LineSegment)), 0);
}

TEST(GeometricProposalCancellation, CancelledContextFailsWithKCancelled) {
    const std::vector<LineSegment> single{LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F}};
    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    expect_rejected(ErrorCode::kCancelled, single, GeometricProposalParams{}, cancelled);
}

TEST(GeometricProposalCancellation, ExpiredDeadlineFailsWithKTimeoutAndFutureDeadlinePasses) {
    const std::vector<LineSegment> single{LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F}};
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    expect_rejected(ErrorCode::kTimeout, single, GeometricProposalParams{}, expired);

    ExecutionContext in_time;
    in_time.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto result =
        propose_regions(rectangle_chain(10.0F, 10.0F, 100.0F, 50.0F), GeometricProposalParams{}, in_time);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().size(), 1U);
}

TEST(GeometricProposalCancellation, CancelledContextWithEmptyInputStillSucceeds) {
    // Cancellation is polled on the quadratic passes; an empty input never
    // reaches a poll row, so the cancelled context is (acceptably) never
    // observed and the call returns an empty success.
    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const auto result = propose_regions({}, GeometricProposalParams{}, cancelled);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().empty());
}

TEST(GeometricProposalProperties, RandomClosedRingsCoverEndpointsAndRepeatBitStable) {
    for (uint32_t seed = 0; seed < 8; ++seed) {
        SCOPED_TRACE(seed);
        const int sides = 3 + static_cast<int>(seed % 4U);  // 3..6
        expect_ring_is_covered_and_stable(closed_ring(seed, sides), sides);
    }
}

}  // namespace
