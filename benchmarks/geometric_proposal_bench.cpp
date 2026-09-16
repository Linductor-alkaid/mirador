// M6-04: verification harness for the experimental geometric region proposals
// (DEC-017, issue #11). Deterministic synthetic scenes with generator-provided
// ground truth cover rectangular UI, rounded corners, broken boundaries,
// decorative frames and texture interference. Two measurement modes per
// RISK-2026-09: mode A feeds the generator's exact segments to propose_regions
// (closure-analysis gain); mode B rasterizes the scene and runs the first-party
// segment detector plus merge_collinear first (end-to-end, exposing input
// segment quality separately from the closure layer). Reports semantic region
// recall, proposal precision, duplicate proposals per entity and tight-ROI
// pixel reduction against the DEC-017 promotion thresholds, a latency curve
// over segment count (RISK-2026-15) and a placeholder temporal probe (M6-05
// owns the cross-frame contract). All numbers are only meaningful on the
// machine that ran the harness (DEC-011); built-in self-checks exit non-zero
// on drift. No randomness anywhere: equal inputs reproduce equal reports.
#include <mirador/execution_context.hpp>
#include <mirador/geometric_proposal.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/line_detector.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/segment_growing_line_detector.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

using mirador::CollinearMergeParams;
using mirador::ExecutionContext;
using mirador::GeometricProposalParams;
using mirador::GeometricRegionProposal;
using mirador::ImageView;
using mirador::LineDetectRequest;
using mirador::LineSegment;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::RectI;
using mirador::SegmentGrowingLineDetector;
using mirador::SegmentGrowingParams;

constexpr int32_t kFrameWidth = 1280;
constexpr int32_t kFrameHeight = 800;
constexpr int kWarmups = 20;
constexpr int kIterations = 300;
constexpr double kIouThreshold = 0.5;
constexpr double kRecoveryThreshold = 0.8;
constexpr auto kLineGray = std::byte{232};
// Radius large enough that the arc chords (~0.77 * radius) survive the
// detector's ~1 px endpoint trimming with >= 80% interval coverage.
constexpr float kRadius = 24.0F;
constexpr float kInvSqrt2 = 0.70710677F;

// DEC-017 promotion thresholds (synthetic basis, initial values).
constexpr double kGateRecall = 0.90;
constexpr double kGatePrecision = 0.50;
constexpr int kGateMaxProposalsPerEntity = 2;
constexpr double kGateRoiReduction = 0.60;

LineSegment seg(const float x0, const float y0, const float x1, const float y1) {
    return LineSegment{PointF{x0, y0}, PointF{x1, y1}, 1.0F};
}

struct GrayFrame {
    std::vector<std::byte> bytes;
    ImageView view;
};

struct SyntheticScene {
    const char* name = "";
    std::vector<RectI> ground_truth;
    std::vector<LineSegment> exact_segments;  // mode A input
    std::vector<LineSegment> drawn_lines;     // rasterized for mode B (input-quality reference)
};

// --- deterministic scene generation -----------------------------------------

void add_box(const RectI& rect, std::vector<LineSegment>& out) {
    const auto x0 = static_cast<float>(rect.x);
    const auto y0 = static_cast<float>(rect.y);
    const auto x1 = static_cast<float>(rect.x + rect.width);
    const auto y1 = static_cast<float>(rect.y + rect.height);
    out.push_back(seg(x0, y0, x1, y0));
    out.push_back(seg(x1, y0, x1, y1));
    out.push_back(seg(x1, y1, x0, y1));
    out.push_back(seg(x0, y1, x0, y0));
}

// Two chords joining the side endpoints `a` (horizontal side) and `b` (vertical
// side) of one rounded corner; they meet the sides exactly at shared endpoints,
// so the junction graph keeps the cycle through all four corners.
void add_arc_chords(const PointF& a, const PointF& b, std::vector<LineSegment>& out) {
    const float mid_x = a.x + (b.x - a.x) * kInvSqrt2;
    const float mid_y = b.y + (a.y - b.y) * kInvSqrt2;
    out.push_back(seg(a.x, a.y, mid_x, mid_y));
    out.push_back(seg(mid_x, mid_y, b.x, b.y));
}

void add_rounded_box(const RectI& rect, const float radius, std::vector<LineSegment>& out) {
    const auto x0 = static_cast<float>(rect.x);
    const auto y0 = static_cast<float>(rect.y);
    const auto x1 = static_cast<float>(rect.x + rect.width);
    const auto y1 = static_cast<float>(rect.y + rect.height);
    const PointF top_left_h{x0 + radius, y0};
    const PointF top_left_v{x0, y0 + radius};
    const PointF top_right_h{x1 - radius, y0};
    const PointF top_right_v{x1, y0 + radius};
    const PointF bottom_right_h{x1 - radius, y1};
    const PointF bottom_right_v{x1, y1 - radius};
    const PointF bottom_left_h{x0 + radius, y1};
    const PointF bottom_left_v{x0, y1 - radius};
    out.push_back(seg(top_left_h.x, top_left_h.y, top_right_h.x, top_right_h.y));
    out.push_back(seg(top_right_v.x, top_right_v.y, bottom_right_v.x, bottom_right_v.y));
    out.push_back(seg(bottom_right_h.x, bottom_right_h.y, bottom_left_h.x, bottom_left_h.y));
    out.push_back(seg(bottom_left_v.x, bottom_left_v.y, top_left_v.x, top_left_v.y));
    add_arc_chords(top_left_h, top_left_v, out);
    add_arc_chords(top_right_h, top_right_v, out);
    add_arc_chords(bottom_right_h, bottom_right_v, out);
    add_arc_chords(bottom_left_h, bottom_left_v, out);
}

// Sides are indexed 0 top, 1 right, 2 bottom, 3 left; the broken side becomes
// two halves around a centered `gap`-pixel gap. One break per box keeps exactly
// two dangling junctions, so closure = 1 - gap / diagonal (DEC-017 scoring).
void add_broken_box(const RectI& rect, const int broken_side, const int32_t gap, std::vector<LineSegment>& out) {
    const auto x0 = static_cast<float>(rect.x);
    const auto y0 = static_cast<float>(rect.y);
    const auto x1 = static_cast<float>(rect.x + rect.width);
    const auto y1 = static_cast<float>(rect.y + rect.height);
    const std::array<LineSegment, 4> sides = {seg(x0, y0, x1, y0), seg(x1, y0, x1, y1), seg(x1, y1, x0, y1),
                                              seg(x0, y1, x0, y0)};
    const auto half = static_cast<float>(gap) * 0.5F;
    for (int side = 0; side < 4; ++side) {
        if (side != broken_side) {
            out.push_back(sides[side]);
            continue;
        }
        const LineSegment& full = sides[side];
        const double length = mirador::segment_length(full);
        const auto ux = static_cast<float>((full.end.x - full.begin.x) / length);
        const auto uy = static_cast<float>((full.end.y - full.begin.y) / length);
        const float mid_x = (full.begin.x + full.end.x) * 0.5F;
        const float mid_y = (full.begin.y + full.end.y) * 0.5F;
        out.push_back(seg(full.begin.x, full.begin.y, mid_x - ux * half, mid_y - uy * half));
        out.push_back(seg(mid_x + ux * half, mid_y + uy * half, full.end.x, full.end.y));
    }
}

SyntheticScene base_scene(const char* name) {
    SyntheticScene scene;
    scene.name = name;
    return scene;
}

void finish_scene(SyntheticScene& scene) {
    scene.drawn_lines = scene.exact_segments;
}

SyntheticScene make_rect_ui_scene() {
    SyntheticScene scene = base_scene("rect-ui");
    const std::array<RectI, 6> boxes = {RectI{80, 80, 480, 320}, RectI{680, 120, 360, 240}, RectI{80, 480, 300, 200},
                                        {440, 520, 180, 60},     {680, 480, 360, 60},       {1120, 80, 120, 320}};
    for (const RectI& box : boxes) {
        add_box(box, scene.exact_segments);
        scene.ground_truth.push_back(box);
    }
    finish_scene(scene);
    return scene;
}

SyntheticScene make_rounded_scene() {
    SyntheticScene scene = base_scene("rounded-ui");
    const std::array<RectI, 4> boxes = {RectI{120, 120, 360, 240}, RectI{560, 120, 300, 180}, RectI{120, 460, 280, 180},
                                        RectI{560, 460, 240, 140}};
    for (const RectI& box : boxes) {
        add_rounded_box(box, kRadius, scene.exact_segments);
        scene.ground_truth.push_back(box);
    }
    finish_scene(scene);
    return scene;
}

SyntheticScene make_broken_scene() {
    SyntheticScene scene = base_scene("broken-boundary");
    const std::array<RectI, 4> boxes = {RectI{100, 100, 340, 200}, RectI{520, 100, 300, 160}, RectI{100, 420, 360, 220},
                                        RectI{560, 420, 280, 180}};
    const std::array<int32_t, 4> gaps = {36, 28, 44, 24};
    for (std::size_t i = 0; i < 4; ++i) {
        const int side = i < 2 ? 0 : 2;  // top break on the upper row, bottom break below
        add_broken_box(boxes[i], side, gaps[i], scene.exact_segments);
        scene.ground_truth.push_back(boxes[i]);
    }
    finish_scene(scene);
    return scene;
}

// Decorative closed frames (window border, icon placeholders) are proposed just
// like the ground-truth entities: closure alone cannot separate them, which is
// exactly the precision boundary the experiment must quantify (issue #11).
SyntheticScene make_decorative_scene() {
    SyntheticScene scene = base_scene("decorative-frames");
    add_box(RectI{60, 60, 460, 360}, scene.exact_segments);  // window frame (decoration)
    scene.exact_segments.push_back(seg(60, 110, 520, 110));  // title separator: open, never proposed
    add_box(RectI{100, 150, 80, 80}, scene.exact_segments);  // icon placeholder (decoration)
    add_box(RectI{220, 150, 80, 80}, scene.exact_segments);  // icon placeholder (decoration)
    const std::array<RectI, 4> entities = {RectI{340, 150, 140, 50}, RectI{340, 230, 140, 50}, RectI{100, 300, 380, 40},
                                           RectI{100, 370, 380, 40}};
    for (const RectI& entity : entities) {
        add_box(entity, scene.exact_segments);
        scene.ground_truth.push_back(entity);
    }
    finish_scene(scene);
    return scene;
}

SyntheticScene make_texture_scene() {
    SyntheticScene scene = base_scene("texture-interference");
    const std::array<RectI, 3> boxes = {RectI{700, 100, 300, 200}, RectI{700, 380, 300, 140},
                                        RectI{1080, 100, 160, 300}};
    for (const RectI& box : boxes) {
        add_box(box, scene.exact_segments);
        scene.ground_truth.push_back(box);
    }
    for (int32_t y = 40; y < 760; y += 28) {
        for (int32_t x = 40; x < 640; x += 32) {
            scene.exact_segments.push_back(
                seg(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 10), static_cast<float>(y)));
        }
    }
    finish_scene(scene);
    return scene;
}

using SceneFactory = SyntheticScene (*)();

const std::array<SceneFactory, 5> kScenes = {make_rect_ui_scene, make_rounded_scene, make_broken_scene,
                                             make_decorative_scene, make_texture_scene};

// --- metrics -----------------------------------------------------------------

double iou(const RectI& a, const RectI& b) {
    const RectI overlap = mirador::intersect(a, b);
    if (overlap.width <= 0 || overlap.height <= 0) {
        return 0.0;
    }
    const int64_t inter = static_cast<int64_t>(overlap.width) * overlap.height;
    const int64_t area_a = static_cast<int64_t>(a.width) * a.height;
    const int64_t area_b = static_cast<int64_t>(b.width) * b.height;
    return static_cast<double>(inter) / static_cast<double>(area_a + area_b - inter);
}

struct MatchStats {
    int proposals = 0;
    int entities = 0;
    int covered_entities = 0;
    int matched_proposals = 0;
    int max_proposals_per_entity = 0;
};

void accumulate_matches(const std::vector<GeometricRegionProposal>& proposals, const std::vector<RectI>& ground_truth,
                        MatchStats& stats) {
    stats.proposals += static_cast<int>(proposals.size());
    stats.entities += static_cast<int>(ground_truth.size());
    std::vector<int> per_entity(ground_truth.size(), 0);
    for (const GeometricRegionProposal& proposal : proposals) {
        bool matched = false;
        for (std::size_t i = 0; i < ground_truth.size(); ++i) {
            if (iou(proposal.tight_bounds, ground_truth[i]) >= kIouThreshold) {
                ++per_entity[i];
                matched = true;
            }
        }
        if (matched) {
            ++stats.matched_proposals;
        }
    }
    for (const int count : per_entity) {
        if (count > 0) {
            ++stats.covered_entities;
        }
        stats.max_proposals_per_entity = std::max(stats.max_proposals_per_entity, count);
    }
}

// 1 - (union of all proposal tight ROIs) / frame pixels: what a consumer would
// process instead of the full frame, decorations included (issue #11 metric).
double tight_roi_reduction(const std::vector<GeometricRegionProposal>& proposals, const int32_t width,
                           const int32_t height) {
    std::vector<uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    int64_t pixels = 0;
    for (const GeometricRegionProposal& proposal : proposals) {
        const RectI clipped = mirador::intersect(proposal.tight_bounds, RectI{0, 0, width, height});
        for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
            for (int32_t x = clipped.x; x < clipped.x + clipped.width; ++x) {
                if (mask[row + static_cast<std::size_t>(x)] == 0) {
                    mask[row + static_cast<std::size_t>(x)] = 1;
                    ++pixels;
                }
            }
        }
    }
    const double frame = static_cast<double>(width) * static_cast<double>(height);
    return 1.0 - static_cast<double>(pixels) / frame;
}

// Fraction of ground-truth entities covered by at least one proposal.
double recall_of(const MatchStats& stats) {
    return stats.entities == 0 ? 1.0
                               : static_cast<double>(stats.covered_entities) / static_cast<double>(stats.entities);
}

// Fraction of proposals that correspond to a ground-truth entity (IoU >= 0.5).
double precision_of(const MatchStats& stats) {
    return stats.proposals == 0 ? 1.0
                                : static_cast<double>(stats.matched_proposals) / static_cast<double>(stats.proposals);
}

// --- self-checks (hand-computed metric cases; non-zero exit on drift) --------

void expect_near(const char* what, const double got, const double want, const double eps) {
    if (std::fabs(got - want) > eps) {
        std::fprintf(stderr, "self-check failed: %s got %.6f want %.6f\n", what, got, want);
        std::exit(1);
    }
}

void expect_int(const char* what, const int got, const int want) {
    if (got != want) {
        std::fprintf(stderr, "self-check failed: %s got %d want %d\n", what, got, want);
        std::exit(1);
    }
}

GeometricRegionProposal proposal_with_bounds(const int32_t x, const int32_t y, const int32_t w, const int32_t h) {
    GeometricRegionProposal proposal;
    proposal.tight_bounds = RectI{x, y, w, h};
    return proposal;
}

void self_check_metrics() {
    const std::vector<RectI> single_entity = {RectI{10, 10, 20, 20}};
    std::vector<GeometricRegionProposal> hit;
    hit.push_back(proposal_with_bounds(10, 10, 20, 20));
    MatchStats stats;
    accumulate_matches(hit, single_entity, stats);
    expect_int("case1 matched", stats.matched_proposals, 1);
    expect_int("case1 covered", stats.covered_entities, 1);
    expect_int("case1 dup max", stats.max_proposals_per_entity, 1);
    expect_near("case1 recall", recall_of(stats), 1.0, 1e-9);
    expect_near("case1 precision", precision_of(stats), 1.0, 1e-9);
    expect_near("case1 reduction", tight_roi_reduction(hit, 100, 100), 0.96, 1e-9);

    std::vector<GeometricRegionProposal> miss;
    miss.push_back(proposal_with_bounds(40, 40, 20, 20));
    MatchStats miss_stats;
    accumulate_matches(miss, single_entity, miss_stats);
    expect_near("case2 recall", recall_of(miss_stats), 0.0, 1e-9);
    expect_near("case2 precision", precision_of(miss_stats), 0.0, 1e-9);
    expect_near("case2 reduction", tight_roi_reduction(miss, 100, 100), 0.96, 1e-9);

    const std::vector<RectI> big_entity = {RectI{0, 0, 100, 100}};
    std::vector<GeometricRegionProposal> partial;
    partial.push_back(proposal_with_bounds(0, 0, 100, 60));  // IoU 0.60 -> match
    partial.push_back(proposal_with_bounds(0, 0, 100, 40));  // IoU 0.40 -> no match
    MatchStats partial_stats;
    accumulate_matches(partial, big_entity, partial_stats);
    expect_int("case3 matched", partial_stats.matched_proposals, 1);
    expect_int("case3 covered", partial_stats.covered_entities, 1);
    expect_near("case3 precision", precision_of(partial_stats), 0.5, 1e-9);

    const std::vector<RectI> dup_entity = {RectI{0, 0, 50, 50}};
    std::vector<GeometricRegionProposal> duplicates;
    duplicates.push_back(proposal_with_bounds(0, 0, 50, 50));
    duplicates.push_back(proposal_with_bounds(5, 5, 50, 50));  // IoU 0.68 -> duplicate
    MatchStats dup_stats;
    accumulate_matches(duplicates, dup_entity, dup_stats);
    expect_int("case4 matched", dup_stats.matched_proposals, 2);
    expect_int("case4 dup max", dup_stats.max_proposals_per_entity, 2);
    expect_near("case4 recall", recall_of(dup_stats), 1.0, 1e-9);
    expect_near("case4 precision", precision_of(dup_stats), 1.0, 1e-9);
    expect_near("case4 reduction", tight_roi_reduction(duplicates, 100, 100), 0.7025, 1e-9);
    std::printf("self-check: metric engine ok\n");
}

void self_check_scenes() {
    const GeometricProposalParams params;
    const ExecutionContext context;
    const std::array<int, 5> expected_proposals = {6, 4, 4, 7, 3};
    for (std::size_t i = 0; i < std::size(kScenes); ++i) {
        const SyntheticScene scene = kScenes[i]();
        const auto result = propose_regions(scene.exact_segments, params, context);
        if (!result.ok()) {
            std::fprintf(stderr, "self-check failed: scene %s propose_regions error %d\n", scene.name,
                         static_cast<int>(result.status().code()));
            std::exit(1);
        }
        MatchStats stats;
        accumulate_matches(result.value(), scene.ground_truth, stats);
        expect_int("scene proposals", static_cast<int>(result.value().size()), expected_proposals[i]);
        expect_int("scene covered", stats.covered_entities, stats.entities);
    }
    std::printf("self-check: scene generators ok\n");
}

// --- mode A / mode B ---------------------------------------------------------

void print_stats_row(const char* name, const char* mode, const MatchStats& stats, const double reduction) {
    const double dup_mean = stats.covered_entities == 0 ? 0.0
                                                        : static_cast<double>(stats.matched_proposals) /
                                                              static_cast<double>(stats.covered_entities);
    std::printf("%-18s %s %6d %5d %8d %7.3f %10.3f %7d %9.2f %12.3f\n", name, mode, stats.proposals, stats.entities,
                stats.covered_entities, recall_of(stats), precision_of(stats), stats.max_proposals_per_entity, dup_mean,
                reduction);
}

struct SceneResult {
    MatchStats stats;
    double reduction = 0.0;
};

SceneResult run_mode_a(const SyntheticScene& scene) {
    const GeometricProposalParams params;
    const ExecutionContext context;
    const auto result = propose_regions(scene.exact_segments, params, context);
    if (!result.ok()) {
        std::fprintf(stderr, "mode A failed on %s: error %d\n", scene.name, static_cast<int>(result.status().code()));
        std::exit(1);
    }
    MatchStats stats;
    accumulate_matches(result.value(), scene.ground_truth, stats);
    const SceneResult outcome{stats, tight_roi_reduction(result.value(), kFrameWidth, kFrameHeight)};
    print_stats_row(scene.name, "A", outcome.stats, outcome.reduction);
    return outcome;
}

GrayFrame make_gray_frame() {
    GrayFrame frame;
    frame.bytes.assign(static_cast<std::size_t>(kFrameWidth) * static_cast<std::size_t>(kFrameHeight), std::byte{24});
    frame.view.data = frame.bytes.data();
    frame.view.width = kFrameWidth;
    frame.view.height = kFrameHeight;
    frame.view.row_stride_bytes = kFrameWidth;
    frame.view.format = PixelFormat::kGray8;
    return frame;
}

void plot(GrayFrame& frame, const int x, const int y) {
    if (x < 0 || y < 0 || x >= kFrameWidth || y >= kFrameHeight) {
        return;
    }
    frame.bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(kFrameWidth) + static_cast<std::size_t>(x)] =
        kLineGray;
}

// 1 px DDA along the major axis. The width matters: a 2 px border forms one
// contiguous edge region whose ridge growth wraps the whole ring (mod-180
// alignment joins opposite ridges), the single fit fails and no segment
// survives; 1 px borders keep one ridge region per side, exactly like real
// thin UI strokes.
void draw_segment(GrayFrame& frame, const LineSegment& line) {
    const float dx = std::fabs(line.end.x - line.begin.x);
    const float dy = std::fabs(line.end.y - line.begin.y);
    const int steps = static_cast<int>(std::max(dx, dy));
    for (int i = 0; i <= steps; ++i) {
        const auto t = steps == 0 ? 0.0F : static_cast<float>(i) / static_cast<float>(steps);
        const auto x = static_cast<int>(std::lround(line.begin.x + (line.end.x - line.begin.x) * t));
        const auto y = static_cast<int>(std::lround(line.begin.y + (line.end.y - line.begin.y) * t));
        plot(frame, x, y);
    }
}

SegmentGrowingParams bench_detector_params() {
    SegmentGrowingParams params;
    params.max_segments = 4096;  // each drawn line contributes two ridge segments
    return params;
}

// The detector yields two ridges per 1 px line (2 px apart); the default
// distance tolerance of 1.5 keeps them separate, 3.0 unifies each drawn line
// into one segment so proposals rest on the drawn geometry.
CollinearMergeParams bench_merge_params() {
    CollinearMergeParams params;
    params.distance_tolerance = 3.0;
    return params;
}

// Coverage of `line`'s interval by detected segments (10-degree alignment,
// 3 px perpendicular distance); feeds the input-quality rate below.
double line_coverage(const LineSegment& line, const std::vector<LineSegment>& segments) {
    const double length = mirador::segment_length(line);
    if (length <= 0.0) {
        return 1.0;
    }
    const double ux = (line.end.x - line.begin.x) / length;
    const double uy = (line.end.y - line.begin.y) / length;
    std::vector<std::pair<double, double>> spans;
    for (const LineSegment& candidate : segments) {
        const double other = mirador::segment_length(candidate);
        if (other <= 0.0) {
            continue;
        }
        const double dir_x = (candidate.end.x - candidate.begin.x) / other;
        const double dir_y = (candidate.end.y - candidate.begin.y) / other;
        if (std::fabs(dir_x * ux + dir_y * uy) < 0.9848) {
            continue;
        }
        const double ox = candidate.begin.x - line.begin.x;
        const double oy = candidate.begin.y - line.begin.y;
        if (std::fabs(ox * uy - oy * ux) > 3.0) {
            continue;
        }
        const double start = ox * ux + oy * uy;
        const double end = (candidate.end.x - line.begin.x) * ux + (candidate.end.y - line.begin.y) * uy;
        spans.emplace_back(std::max(0.0, std::min(start, end)), std::min(length, std::max(start, end)));
    }
    std::sort(spans.begin(), spans.end());
    double covered = 0.0;
    double cursor = 0.0;
    for (const auto& span : spans) {
        if (span.second <= cursor) {
            continue;
        }
        covered += span.second - std::max(span.first, cursor);
        cursor = span.second;
    }
    return covered / length;
}

double drawn_line_recovery(const std::vector<LineSegment>& drawn, const std::vector<LineSegment>& detected) {
    if (drawn.empty()) {
        return 1.0;
    }
    int recovered = 0;
    for (const LineSegment& line : drawn) {
        if (line_coverage(line, detected) >= kRecoveryThreshold) {
            ++recovered;
        }
    }
    return static_cast<double>(recovered) / static_cast<double>(drawn.size());
}

void run_mode_b(const SyntheticScene& scene) {
    GrayFrame frame = make_gray_frame();
    for (const LineSegment& line : scene.drawn_lines) {
        draw_segment(frame, line);
    }
    SegmentGrowingLineDetector detector(bench_detector_params());
    const LineDetectRequest request;
    const ExecutionContext context;
    const auto detected = detector.detect(frame.view, request, context);
    if (!detected.ok()) {
        std::fprintf(stderr, "mode B detection failed on %s: error %d\n", scene.name,
                     static_cast<int>(detected.status().code()));
        std::exit(1);
    }
    const auto merged = merge_collinear(detected.value().segments, bench_merge_params());
    if (!merged.ok()) {
        std::fprintf(stderr, "mode B merge failed on %s: error %d\n", scene.name,
                     static_cast<int>(merged.status().code()));
        std::exit(1);
    }
    const GeometricProposalParams params;
    const auto result = propose_regions(merged.value(), params, context);
    if (!result.ok()) {
        std::fprintf(stderr, "mode B proposals failed on %s: error %d\n", scene.name,
                     static_cast<int>(result.status().code()));
        std::exit(1);
    }
    MatchStats stats;
    accumulate_matches(result.value(), scene.ground_truth, stats);
    const double reduction = tight_roi_reduction(result.value(), kFrameWidth, kFrameHeight);
    const double recovery = drawn_line_recovery(scene.drawn_lines, merged.value());
    std::printf("%-18s %s %6zu %11zu %8.3f %7.3f %10.3f %7d %12.3f\n", scene.name, "B", merged.value().size(),
                detected.value().segments.size(), recovery, recall_of(stats), precision_of(stats),
                stats.max_proposals_per_entity, reduction);
}

// --- latency curve (RISK-2026-15) and temporal probe (placeholder) -----------

bool overlaps_padded_box(const int32_t x, const int32_t y, const RectI& box) {
    constexpr int32_t kPad = 16;  // keeps filler dashes clear of the junction radius
    return x + 10 >= box.x - kPad && x <= box.x + box.width + kPad && y >= box.y - kPad &&
           y <= box.y + box.height + kPad;
}

// 8 closed boxes plus isolated horizontal dashes (14 px end-to-end gaps, so no
// filler junctions) until `target_count` segments are reached.
std::vector<LineSegment> make_timing_segments(const int target_count) {
    std::vector<LineSegment> segments;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 4; ++col) {
            add_box(RectI{80 + col * 300, 80 + row * 220, 160, 100}, segments);
        }
    }
    const std::array<RectI, 8> boxes = {RectI{80, 80, 160, 100},   RectI{380, 80, 160, 100}, RectI{680, 80, 160, 100},
                                        RectI{980, 80, 160, 100},  RectI{80, 300, 160, 100}, RectI{380, 300, 160, 100},
                                        RectI{680, 300, 160, 100}, RectI{980, 300, 160, 100}};
    for (int32_t y = 40; y < kFrameHeight && static_cast<int>(segments.size()) < target_count; y += 24) {
        for (int32_t x = 40; x < kFrameWidth - 20 && static_cast<int>(segments.size()) < target_count; x += 24) {
            bool padded = false;
            for (const RectI& box : boxes) {
                padded = padded || overlaps_padded_box(x, y, box);
            }
            if (!padded) {
                segments.push_back(seg(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 10),
                                       static_cast<float>(y)));
            }
        }
    }
    return segments;
}

double percentile_us(std::vector<int64_t>& samples, const double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    return static_cast<double>(samples[index]) / 1000.0;
}

void run_timing_curve() {
    const GeometricProposalParams params;
    const ExecutionContext context;
    const std::array<int, 5> targets = {64, 128, 256, 512, 1024};
    for (const int target : targets) {
        const std::vector<LineSegment> segments = make_timing_segments(target);
        if (static_cast<int>(segments.size()) < target) {
            std::fprintf(stderr, "timing generator capacity: want %d got %zu\n", target, segments.size());
            std::exit(1);
        }
        for (int i = 0; i < kWarmups; ++i) {
            const auto warm = propose_regions(segments, params, context);
            if (!warm.ok()) {
                std::fprintf(stderr, "timing warmup failed at n=%d\n", target);
                std::exit(1);
            }
        }
        std::vector<int64_t> samples;
        samples.reserve(static_cast<std::size_t>(kIterations));
        for (int i = 0; i < kIterations; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto result = propose_regions(segments, params, context);
            const auto stop = std::chrono::steady_clock::now();
            if (!result.ok()) {
                std::fprintf(stderr, "timing run failed at n=%d\n", target);
                std::exit(1);
            }
            samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
        }
        std::printf("n=%4d p50=%9.2fus p95=%9.2fus\n", target, percentile_us(samples, 0.50),
                    percentile_us(samples, 0.95));
    }
}

int best_iou_percent(const RectI& entity, const std::vector<GeometricRegionProposal>& proposals) {
    int best_permille = 0;
    for (const GeometricRegionProposal& proposal : proposals) {
        best_permille = std::max(best_permille, static_cast<int>(iou(proposal.tight_bounds, entity) * 1000.0));
    }
    return best_permille;
}

// Placeholder for the M6-05 cross-frame probe: the same scene re-rendered with
// a deterministic +/-2 px jitter; reports worst per-entity tight-ROI IoU and
// proposal-count stability. Informational only, no gate (DEC-017 item 5).
void run_temporal_probe() {
    const SyntheticScene base = make_rect_ui_scene();
    const GeometricProposalParams params;
    const ExecutionContext context;
    int worst_permille = 1000;
    int first_count = -1;
    bool counts_stable = true;
    for (int shift = -2; shift <= 2; ++shift) {
        std::vector<LineSegment> segments = base.exact_segments;
        for (LineSegment& segment : segments) {
            segment.begin.x += static_cast<float>(shift);
            segment.begin.y += static_cast<float>(shift);
            segment.end.x += static_cast<float>(shift);
            segment.end.y += static_cast<float>(shift);
        }
        const auto result = propose_regions(segments, params, context);
        if (!result.ok()) {
            std::fprintf(stderr, "temporal probe failed at shift %d\n", shift);
            std::exit(1);
        }
        if (first_count < 0) {
            first_count = static_cast<int>(result.value().size());
        } else if (static_cast<int>(result.value().size()) != first_count) {
            counts_stable = false;
        }
        int frame_min_permille = 1000;
        for (const RectI& entity : base.ground_truth) {
            const RectI shifted{entity.x + shift, entity.y + shift, entity.width, entity.height};
            frame_min_permille = std::min(frame_min_permille, best_iou_percent(shifted, result.value()));
        }
        worst_permille = std::min(worst_permille, frame_min_permille);
        std::printf("frame shift %+d: proposals=%zu min-entity-IoU=%.3f\n", shift, result.value().size(),
                    static_cast<double>(frame_min_permille) / 1000.0);
    }
    std::printf("temporal placeholder: worst per-entity IoU=%.3f count-stable=%s\n",
                static_cast<double>(worst_permille) / 1000.0, counts_stable ? "yes" : "no");
}

void print_gate(const char* metric, const bool passed) {
    std::printf("gate %-42s %s\n", metric, passed ? "PASS" : "FAIL");
}

}  // namespace

int main() {
    self_check_metrics();
    self_check_scenes();

    std::printf("\ngeometric proposal verification harness, %dx%d gray frames, propose_regions defaults\n", kFrameWidth,
                kFrameHeight);
    std::printf("scene              mode props   gt covered  recall precision dup-max dup-mean roi-reduce\n");

    MatchStats total;
    double min_reduction = 1.0;
    for (const SceneFactory factory : kScenes) {
        const SyntheticScene scene = factory();
        const SceneResult outcome = run_mode_a(scene);
        total.proposals += outcome.stats.proposals;
        total.entities += outcome.stats.entities;
        total.covered_entities += outcome.stats.covered_entities;
        total.matched_proposals += outcome.stats.matched_proposals;
        total.max_proposals_per_entity =
            std::max(total.max_proposals_per_entity, outcome.stats.max_proposals_per_entity);
        min_reduction = std::min(min_reduction, outcome.reduction);
    }
    std::printf("%-18s %s %6d %5d %8d %7.3f %10.3f %7d %9s %12.3f\n", "AGGREGATE", "A", total.proposals, total.entities,
                total.covered_entities, recall_of(total), precision_of(total), total.max_proposals_per_entity, "-",
                min_reduction);
    print_gate("semantic recall >= 0.90", recall_of(total) >= kGateRecall);
    print_gate("proposal precision >= 0.50", precision_of(total) >= kGatePrecision);
    print_gate("duplicates per entity <= 2", total.max_proposals_per_entity <= kGateMaxProposalsPerEntity);
    print_gate("tight ROI reduction >= 0.60", min_reduction >= kGateRoiReduction);

    std::printf("\nscene              mode merged     raw recov  recall precision dup-max roi-reduce\n");
    for (const SceneFactory factory : kScenes) {
        run_mode_b(factory());
    }

    std::printf("\npropose_regions latency vs input size (mode A generator, %d iters after %d warmups)\n", kIterations,
                kWarmups);
    run_timing_curve();

    std::printf("\ntemporal probe (placeholder, M6-05 owns the cross-frame contract)\n");
    run_temporal_probe();

    std::printf(
        "\nDEC-011 qualification: all numbers synthetic-scene only and valid solely on the machine\n"
        "that ran this harness; real-screenshot evaluation follows the offline conventions in\n"
        "docs/benchmarks/evaluation-scenes.md (explicit path, data never enters the repository).\n");
    return 0;
}
