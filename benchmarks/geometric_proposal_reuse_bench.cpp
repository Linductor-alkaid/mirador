// M6-05: cross-frame stability probe and cache-gain measurement for the
// experimental geometric region proposals (DEC-017, issue #11; triggered by
// the M6-04 gates passing). Two informational experiments, both measurement
// only — no session contract, no core changes:
// 1. Temporal association probe: eight entities (six rectangular, two
//    rounded) re-rendered over six frames with independent deterministic
//    +/-2 px jitter; consecutive-frame proposals are associated greedily by
//    tight-ROI IoU >= 0.6 and the association rate / paired IoU are reported.
// 2. Geometry-gated VisualIndex comparison: interior-only crops (tight ROI
//    inset 3 px, border excluded) of eight size-distinct entities are
//    fingerprinted into a VisualIndex over frames 0-1 and queried from
//    frames 2-3, once with content-distinguishable interiors and once with
//    ambiguous (identical uniform) interiors. Baseline is the plain
//    VisualIndex top-1; the treatment takes the first candidate whose stored
//    tight bounds match the query proposal's tight bounds (side ratios
//    within [0.8, 1.25]). Shows whether geometry restores hit correctness
//    when vision is ambiguous without hurting the distinct-content case.
// All numbers are only meaningful on the machine that ran the harness
// (DEC-011); built-in self-checks exit non-zero on drift. No randomness.
#include <mirador/execution_context.hpp>
#include <mirador/geometric_proposal.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/patch_fingerprint.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/visual_fingerprint.hpp>
#include <mirador/visual_index.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using mirador::ExecutionContext;
using mirador::GeometricProposalParams;
using mirador::GeometricRegionProposal;
using mirador::ImageView;
using mirador::LineSegment;
using mirador::PatchFingerprintParams;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::RectI;
using mirador::VisualCandidate;
using mirador::VisualIndex;
using mirador::VisualQueryParams;

constexpr int32_t kFrameWidth = 1280;
constexpr int32_t kFrameHeight = 800;
constexpr int kProbeFrames = 6;
constexpr int kProbeEntities = 8;
constexpr int kReuseEntities = 8;
constexpr int kPopulateFrames = 2;  // frames 0..1 fill the index
constexpr int kQueryFrames = 2;     // frames 2..3 query it
constexpr double kAssociationIou = 0.6;
constexpr double kGateMinRatio = 0.8;
constexpr double kGateMaxRatio = 1.25;
constexpr int32_t kThumbSide = 32;
constexpr int64_t kIndexBytes = int64_t{64} * 1024;
constexpr int64_t kFingerprintBytes = int64_t{1024} * 1024;
constexpr auto kLineGray = std::byte{232};
constexpr auto kBackgroundGray = std::byte{24};

LineSegment seg(const float x0, const float y0, const float x1, const float y1) {
    return LineSegment{PointF{x0, y0}, PointF{x1, y1}, 1.0F};
}

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

// --- shared deterministic rasterizer (1 px lines, M6-04 recipe) --------------

struct GrayFrame {
    std::vector<std::byte> bytes;
    ImageView view;
};

GrayFrame make_gray_frame() {
    GrayFrame frame;
    frame.bytes.assign(static_cast<std::size_t>(kFrameWidth) * static_cast<std::size_t>(kFrameHeight), kBackgroundGray);
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

void draw_segment(GrayFrame& frame, const LineSegment& line) {
    const auto dx = std::fabs(line.end.x - line.begin.x);
    const auto dy = std::fabs(line.end.y - line.begin.y);
    const int steps = static_cast<int>(std::max(dx, dy));
    for (int i = 0; i <= steps; ++i) {
        const auto t = steps == 0 ? 0.0F : static_cast<float>(i) / static_cast<float>(steps);
        const auto x = static_cast<int>(std::lround(line.begin.x + (line.end.x - line.begin.x) * t));
        const auto y = static_cast<int>(std::lround(line.begin.y + (line.end.y - line.begin.y) * t));
        plot(frame, x, y);
    }
}

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

constexpr float kInvSqrt2 = 0.70710677F;
constexpr float kRadius = 24.0F;

void add_arc_chords(const PointF& a, const PointF& b, std::vector<LineSegment>& out) {
    const auto mid_x = a.x + (b.x - a.x) * kInvSqrt2;
    const auto mid_y = b.y + (a.y - b.y) * kInvSqrt2;
    out.push_back(seg(a.x, a.y, mid_x, mid_y));
    out.push_back(seg(mid_x, mid_y, b.x, b.y));
}

void add_rounded_box(const RectI& rect, const float radius, std::vector<LineSegment>& out) {
    const auto x0 = static_cast<float>(rect.x);
    const auto y0 = static_cast<float>(rect.y);
    const auto x1 = static_cast<float>(rect.x + rect.width);
    const auto y1 = static_cast<float>(rect.y + rect.height);
    const PointF tl_h{x0 + radius, y0};
    const PointF tl_v{x0, y0 + radius};
    const PointF tr_h{x1 - radius, y0};
    const PointF tr_v{x1, y0 + radius};
    const PointF br_h{x1 - radius, y1};
    const PointF br_v{x1, y1 - radius};
    const PointF bl_h{x0 + radius, y1};
    const PointF bl_v{x0, y1 - radius};
    out.push_back(seg(tl_h.x, tl_h.y, tr_h.x, tr_h.y));
    out.push_back(seg(tr_v.x, tr_v.y, br_v.x, br_v.y));
    out.push_back(seg(br_h.x, br_h.y, bl_h.x, bl_h.y));
    out.push_back(seg(bl_v.x, bl_v.y, tl_v.x, tl_v.y));
    add_arc_chords(tl_h, tl_v, out);
    add_arc_chords(tr_h, tr_v, out);
    add_arc_chords(br_h, br_v, out);
    add_arc_chords(bl_h, bl_v, out);
}

// --- part 1: temporal association probe --------------------------------------

struct ProbeEntity {
    RectI base;
    bool rounded;
};

std::array<ProbeEntity, kProbeEntities> make_probe_entities() {
    return {ProbeEntity{RectI{80, 80, 300, 200}, false},   ProbeEntity{RectI{460, 100, 260, 160}, false},
            ProbeEntity{RectI{120, 380, 320, 140}, false}, ProbeEntity{RectI{540, 420, 200, 120}, false},
            ProbeEntity{RectI{820, 80, 180, 300}, false},  ProbeEntity{RectI{1060, 120, 160, 240}, false},
            ProbeEntity{RectI{840, 460, 240, 160}, true},  ProbeEntity{RectI{1120, 480, 120, 160}, true}};
}

RectI shifted(const RectI& rect, const int dx, const int dy) {
    return RectI{rect.x + dx, rect.y + dy, rect.width, rect.height};
}

RectI probe_box(const ProbeEntity& entity, const int index, const int frame) {
    const int dx = ((index * 7 + frame * 5) % 5) - 2;
    const int dy = ((index * 3 + frame * 11) % 5) - 2;
    return shifted(entity.base, dx, dy);
}

std::vector<LineSegment> probe_frame_segments(const std::array<ProbeEntity, kProbeEntities>& entities,
                                              const int frame) {
    std::vector<LineSegment> segments;
    for (int i = 0; i < kProbeEntities; ++i) {
        const RectI box = probe_box(entities[static_cast<std::size_t>(i)], i, frame);
        if (entities[static_cast<std::size_t>(i)].rounded) {
            add_rounded_box(box, kRadius, segments);
        } else {
            add_box(box, segments);
        }
    }
    return segments;
}

// Greedy deterministic association: accept IoU pairs in descending order
// (ties by indices), each proposal used at most once.
struct AssociationStats {
    int total_prev = 0;
    int total_curr = 0;
    int pairs = 0;
    double mean_iou = 0.0;
};

AssociationStats associate(const std::vector<GeometricRegionProposal>& prev,
                           const std::vector<GeometricRegionProposal>& curr) {
    struct Pair {
        double value;
        std::size_t p;
        std::size_t c;
    };
    std::vector<Pair> pairs;
    for (std::size_t p = 0; p < prev.size(); ++p) {
        for (std::size_t c = 0; c < curr.size(); ++c) {
            const double value = iou(prev[p].tight_bounds, curr[c].tight_bounds);
            if (value >= kAssociationIou) {
                pairs.push_back(Pair{value, p, c});
            }
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        if (a.value != b.value) {
            return a.value > b.value;
        }
        if (a.p != b.p) {
            return a.p < b.p;
        }
        return a.c < b.c;
    });
    AssociationStats stats;
    stats.total_prev = static_cast<int>(prev.size());
    stats.total_curr = static_cast<int>(curr.size());
    std::vector<bool> used_prev(prev.size(), false);
    std::vector<bool> used_curr(curr.size(), false);
    double sum = 0.0;
    for (const Pair& pair : pairs) {
        if (used_prev[pair.p] || used_curr[pair.c]) {
            continue;
        }
        used_prev[pair.p] = true;
        used_curr[pair.c] = true;
        ++stats.pairs;
        sum += pair.value;
    }
    stats.mean_iou = stats.pairs == 0 ? 0.0 : sum / static_cast<double>(stats.pairs);
    return stats;
}

// --- part 2: geometry-gated VisualIndex comparison ---------------------------

std::array<RectI, kReuseEntities> make_reuse_entities() {
    // Same content family, deliberately distinct width/height pairs so the
    // geometry gate can discriminate; placed on a sparse grid.
    return {RectI{80, 80, 60, 40},   RectI{220, 80, 80, 50},   RectI{380, 80, 100, 60},   RectI{540, 80, 120, 80},
            RectI{720, 80, 140, 90}, RectI{900, 80, 160, 100}, RectI{1080, 80, 180, 120}, RectI{80, 220, 200, 140}};
}

RectI reuse_box(const RectI& base, const int index, const int frame) {
    const int dx = ((index * 5 + frame * 3) % 5) - 2;
    const int dy = ((index * 11 + frame * 7) % 5) - 2;
    return shifted(base, dx, dy);
}

// Interior content, painted in box-relative coordinates so a translated
// entity renders identical interior pixels — the static-UI reuse scenario
// (content moves rigidly with its border). Distinct variant: strong
// per-entity band phase, static across frames — re-rendered crops are
// byte-identical, the exact layer identifies them and vision alone is
// decisive. Ambiguous variant: one low-contrast band pattern shared by every
// entity, phase advancing per frame — no entity-distinguishing content and
// no exact matches, so discrimination falls to the perceptual/template
// layers.
void fill_interior(GrayFrame& frame, const RectI& box, const int entity, const int frame_idx, const bool ambiguous) {
    for (int32_t y = box.y + 2; y < box.y + box.height - 2; ++y) {
        for (int32_t x = box.x + 2; x < box.x + box.width - 2; ++x) {
            if (x < 0 || y < 0 || x >= kFrameWidth || y >= kFrameHeight) {
                continue;
            }
            const int rx = x - box.x;
            const int ry = y - box.y;
            const int phase = ambiguous ? frame_idx : entity * 5;
            const int amplitude = ambiguous ? 8 : 24;
            const auto value = static_cast<uint8_t>(96 + ((rx / 8 + ry / 8 + phase) % 3) * amplitude);
            frame.bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(kFrameWidth) +
                        static_cast<std::size_t>(x)] = static_cast<std::byte>(value);
        }
    }
}

GrayFrame render_reuse_frame(const std::array<RectI, kReuseEntities>& entities, const int frame, const bool ambiguous) {
    GrayFrame frame_image = make_gray_frame();
    std::vector<LineSegment> segments;
    for (int i = 0; i < kReuseEntities; ++i) {
        const RectI box = reuse_box(entities[static_cast<std::size_t>(i)], i, frame);
        fill_interior(frame_image, box, i, frame, ambiguous);
        add_box(box, segments);
    }
    for (const LineSegment& line : segments) {
        draw_segment(frame_image, line);
    }
    return frame_image;
}

// Interior-only crop window: the tight ROI shrunk by 3 px on every side so
// the fingerprint sees entity content, not border proportions (which would
// otherwise let differently sized entities be told apart by their frames
// alone — the ambiguity the gate must fix).
RectI interior_crop(const RectI& rect) {
    constexpr int32_t kInset = 3;
    return RectI{rect.x + kInset, rect.y + kInset, rect.width - 2 * kInset, rect.height - 2 * kInset};
}

ImageView patch_view(const GrayFrame& frame, const RectI& roi) {
    ImageView view;
    view.data = frame.bytes.data() + static_cast<std::size_t>(roi.y) * static_cast<std::size_t>(kFrameWidth) +
                static_cast<std::size_t>(roi.x);
    view.width = roi.width;
    view.height = roi.height;
    view.row_stride_bytes = kFrameWidth;
    view.format = PixelFormat::kGray8;
    return view;
}

bool side_ratio_ok(const int32_t query_side, const int32_t stored_side) {
    if (query_side <= 0 || stored_side <= 0) {
        return false;
    }
    const double ratio = static_cast<double>(query_side) / static_cast<double>(stored_side);
    return ratio >= kGateMinRatio && ratio <= kGateMaxRatio;
}

bool geometry_consistent(const RectI& query, const RectI& stored) {
    return side_ratio_ok(query.width, stored.width) && side_ratio_ok(query.height, stored.height);
}

// True when the top VisualIndex candidate is the queried entity.
bool baseline_top1(const std::vector<VisualCandidate>& candidates, const uint64_t entity) {
    return !candidates.empty() && candidates.front().entry_id == entity;
}

// Top-1 after the geometry gate: the first candidate whose stored tight
// bounds match the query proposal's tight bounds. Returns false when no
// candidate passes the gate (the entity was not retrieved at all).
bool gated_top1(const std::vector<VisualCandidate>& candidates, const RectI& query_bounds,
                const std::vector<RectI>& stored_geometry, uint64_t* winner) {
    const auto usable = [&](const VisualCandidate& candidate) {
        return candidate.entry_id < stored_geometry.size() &&
               geometry_consistent(query_bounds, stored_geometry[static_cast<std::size_t>(candidate.entry_id)]);
    };
    const auto match = std::find_if(candidates.begin(), candidates.end(), usable);
    if (match == candidates.end()) {
        return false;
    }
    *winner = match->entry_id;
    return true;
}

// Retrieve generously, gate strictly: thresholds below the defaults so the
// ambiguous variant actually yields candidates for the gate to re-rank.
VisualQueryParams bench_query_params() {
    VisualQueryParams params;
    params.perceptual_similarity_threshold = 0.75;
    params.template_ncc_threshold = 0.6;
    return params;
}

// --- self-checks --------------------------------------------------------------

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

void expect_true(const char* what, const bool passed) {
    if (!passed) {
        std::fprintf(stderr, "self-check failed: %s\n", what);
        std::exit(1);
    }
}

GeometricRegionProposal proposal_with_bounds(const int32_t x, const int32_t y, const int32_t w, const int32_t h) {
    GeometricRegionProposal proposal;
    proposal.tight_bounds = RectI{x, y, w, h};
    return proposal;
}

void self_check_association() {
    const std::vector<GeometricRegionProposal> prev = {proposal_with_bounds(0, 0, 100, 100)};
    std::vector<GeometricRegionProposal> curr;
    curr.push_back(proposal_with_bounds(2, 2, 100, 100));    // IoU 0.918 -> associate
    curr.push_back(proposal_with_bounds(400, 400, 50, 50));  // disjoint
    const AssociationStats stats = associate(prev, curr);
    expect_int("association pairs", stats.pairs, 1);
    expect_int("association prev", stats.total_prev, 1);
    expect_int("association curr", stats.total_curr, 2);
    expect_near("association iou", stats.mean_iou, iou(RectI{0, 0, 100, 100}, RectI{2, 2, 100, 100}), 1e-9);
    // Tie-breaking determinism: two identical candidates associate by index.
    const std::vector<GeometricRegionProposal> twin_curr = {proposal_with_bounds(0, 0, 100, 100),
                                                            proposal_with_bounds(0, 0, 100, 100)};
    expect_int("association tie pairs", associate(prev, twin_curr).pairs, 1);
    std::printf("self-check: association ok\n");
}

void self_check_geometry_gate() {
    const RectI query{0, 0, 100, 50};
    expect_true("gate accept same", geometry_consistent(query, RectI{0, 0, 100, 50}));
    expect_true("gate accept 0.8", geometry_consistent(query, RectI{0, 0, 125, 62}));
    expect_true("gate accept 1.25", geometry_consistent(query, RectI{0, 0, 80, 40}));
    expect_true("gate reject narrow", !geometry_consistent(query, RectI{0, 0, 79, 50}));
    expect_true("gate reject wide", !geometry_consistent(query, RectI{0, 0, 126, 50}));
    expect_true("gate reject height", !geometry_consistent(query, RectI{0, 0, 100, 39}));
    std::printf("self-check: geometry gate ok\n");
}

// --- runners ------------------------------------------------------------------

void run_temporal_probe() {
    const std::array<ProbeEntity, kProbeEntities> entities = make_probe_entities();
    const GeometricProposalParams params;
    const ExecutionContext context;
    std::vector<std::vector<GeometricRegionProposal>> frames;
    for (int f = 0; f < kProbeFrames; ++f) {
        auto result = propose_regions(probe_frame_segments(entities, f), params, context);
        if (!result.ok() || static_cast<int>(result.value().size()) != kProbeEntities) {
            std::fprintf(stderr, "temporal probe frame %d failed\n", f);
            std::exit(1);
        }
        frames.push_back(result.take_value());
    }
    double worst_rate = 1.0;
    double worst_iou = 1.0;
    for (int f = 1; f < kProbeFrames; ++f) {
        const AssociationStats stats =
            associate(frames[static_cast<std::size_t>(f - 1)], frames[static_cast<std::size_t>(f)]);
        const double rate = static_cast<double>(stats.pairs) / static_cast<double>(kProbeEntities);
        worst_rate = std::min(worst_rate, rate);
        worst_iou = stats.pairs == 0 ? 0.0 : std::min(worst_iou, stats.mean_iou);
        std::printf("transition %d->%d: associated %d/%d rate %.3f mean-IoU %.3f\n", f - 1, f, stats.pairs,
                    kProbeEntities, rate, stats.mean_iou);
    }
    std::printf("temporal probe: worst association rate %.3f worst mean paired IoU %.3f (informational)\n", worst_rate,
                worst_iou);
}

struct VariantOutcome {
    int queries = 0;
    int baseline_correct = 0;
    int baseline_wrong = 0;
    int baseline_miss = 0;
    int gated_correct = 0;
    int gated_wrong = 0;
    int gated_miss = 0;
};

// Rectangular-only segment set of one reuse frame.
std::vector<LineSegment> reuse_frame_segments(const std::array<RectI, kReuseEntities>& entities, const int frame) {
    std::vector<LineSegment> segments;
    for (int i = 0; i < kReuseEntities; ++i) {
        add_box(reuse_box(entities[static_cast<std::size_t>(i)], i, frame), segments);
    }
    return segments;
}

// Proposals of one reuse frame; every frame must yield exactly one proposal
// per entity, otherwise the harness is broken and we exit.
std::vector<GeometricRegionProposal> reuse_frame_proposals(const std::array<RectI, kReuseEntities>& entities,
                                                           const int frame, const GeometricProposalParams& params,
                                                           const ExecutionContext& context, const char* what) {
    auto result = propose_regions(reuse_frame_segments(entities, frame), params, context);
    if (!result.ok() || static_cast<int>(result.value().size()) != kReuseEntities) {
        std::fprintf(stderr, "reuse %s frame %d failed\n", what, frame);
        std::exit(1);
    }
    return result.take_value();
}

mirador::VisualPatchFingerprint entity_fingerprint(const GrayFrame& image, const GeometricRegionProposal& proposal,
                                                   const PatchFingerprintParams& params, const int frame,
                                                   const int entity, const char* what) {
    auto fingerprint = make_visual_patch_fingerprint(patch_view(image, interior_crop(proposal.tight_bounds)), params,
                                                     kFingerprintBytes);
    if (!fingerprint.ok()) {
        std::fprintf(stderr, "fingerprint failed on %s frame %d entity %d\n", what, frame, entity);
        std::exit(1);
    }
    return fingerprint.take_value();
}

// Fills the index from the populate frames; the stored geometry side table
// records the last populate frame's tight bounds per entry.
void populate_reuse_index(VisualIndex& index, std::vector<RectI>& stored_geometry,
                          const std::array<RectI, kReuseEntities>& entities, const bool ambiguous,
                          const PatchFingerprintParams& fingerprint_params,
                          const GeometricProposalParams& proposal_params, const ExecutionContext& context) {
    for (int f = 0; f < kPopulateFrames; ++f) {
        const GrayFrame image = render_reuse_frame(entities, f, ambiguous);
        const std::vector<GeometricRegionProposal> proposals =
            reuse_frame_proposals(entities, f, proposal_params, context, "populate");
        for (int i = 0; i < kReuseEntities; ++i) {
            const auto fingerprint =
                entity_fingerprint(image, proposals[static_cast<std::size_t>(i)], fingerprint_params, f, i, "populate");
            if (!index.insert(static_cast<uint64_t>(i), fingerprint).ok()) {
                std::fprintf(stderr, "index insert failed on frame %d entity %d\n", f, i);
                std::exit(1);
            }
        }
    }
    const GrayFrame last = render_reuse_frame(entities, kPopulateFrames - 1, ambiguous);
    const std::vector<GeometricRegionProposal> last_proposals =
        reuse_frame_proposals(entities, kPopulateFrames - 1, proposal_params, context, "populate");
    for (int i = 0; i < kReuseEntities; ++i) {
        stored_geometry[static_cast<std::size_t>(i)] = last_proposals[static_cast<std::size_t>(i)].tight_bounds;
    }
}

void tally_query(VariantOutcome& outcome, const std::vector<VisualCandidate>& candidates,
                 const GeometricRegionProposal& proposal, const int entity, const std::vector<RectI>& stored_geometry) {
    ++outcome.queries;
    if (candidates.empty()) {
        ++outcome.baseline_miss;
    } else if (baseline_top1(candidates, static_cast<uint64_t>(entity))) {
        ++outcome.baseline_correct;
    } else {
        ++outcome.baseline_wrong;
    }
    uint64_t gated_id = 0;
    if (!gated_top1(candidates, proposal.tight_bounds, stored_geometry, &gated_id)) {
        ++outcome.gated_miss;
    } else if (gated_id == static_cast<uint64_t>(entity)) {
        ++outcome.gated_correct;
    } else {
        ++outcome.gated_wrong;
    }
}

VariantOutcome run_reuse_queries(VisualIndex& index, const std::array<RectI, kReuseEntities>& entities,
                                 const bool ambiguous, const std::vector<RectI>& stored_geometry,
                                 const PatchFingerprintParams& fingerprint_params,
                                 const GeometricProposalParams& proposal_params, const ExecutionContext& context,
                                 const VisualQueryParams& query_params) {
    VariantOutcome outcome;
    for (int f = kPopulateFrames; f < kPopulateFrames + kQueryFrames; ++f) {
        const GrayFrame image = render_reuse_frame(entities, f, ambiguous);
        const std::vector<GeometricRegionProposal> proposals =
            reuse_frame_proposals(entities, f, proposal_params, context, "query");
        for (int i = 0; i < kReuseEntities; ++i) {
            const auto fingerprint =
                entity_fingerprint(image, proposals[static_cast<std::size_t>(i)], fingerprint_params, f, i, "query");
            const auto candidates = index.query(fingerprint, query_params);
            if (!candidates.ok()) {
                std::fprintf(stderr, "index query failed on frame %d entity %d\n", f, i);
                std::exit(1);
            }
            tally_query(outcome, candidates.value(), proposals[static_cast<std::size_t>(i)], i, stored_geometry);
        }
    }
    return outcome;
}

void run_reuse_variant(const char* name, const bool ambiguous) {
    const std::array<RectI, kReuseEntities> entities = make_reuse_entities();
    VisualIndex index = VisualIndex::create(kIndexBytes, kThumbSide).take_value();
    const PatchFingerprintParams fingerprint_params;
    const GeometricProposalParams proposal_params;
    const ExecutionContext context;
    const VisualQueryParams query_params = bench_query_params();

    std::vector<RectI> stored_geometry(static_cast<std::size_t>(kReuseEntities), RectI{});
    populate_reuse_index(index, stored_geometry, entities, ambiguous, fingerprint_params, proposal_params, context);
    const VariantOutcome outcome = run_reuse_queries(index, entities, ambiguous, stored_geometry, fingerprint_params,
                                                     proposal_params, context, query_params);
    const auto queries = static_cast<double>(outcome.queries);
    std::printf(
        "%-10s queries=%d baseline top-1 %d (%.3f) wrong %d miss %d | gated top-1 %d (%.3f) wrong %d miss "
        "%d\n",
        name, outcome.queries, outcome.baseline_correct, static_cast<double>(outcome.baseline_correct) / queries,
        outcome.baseline_wrong, outcome.baseline_miss, outcome.gated_correct,
        static_cast<double>(outcome.gated_correct) / queries, outcome.gated_wrong, outcome.gated_miss);
}

}  // namespace

int main() {
    self_check_association();
    self_check_geometry_gate();

    std::printf("\n1. temporal association probe, %d entities x %d frames, jitter +/-2 px, IoU >= %.2f\n",
                kProbeEntities, kProbeFrames, kAssociationIou);
    run_temporal_probe();

    std::printf(
        "\n2. geometry-gated VisualIndex comparison, %d entities, index from frames 0-%d, queries "
        "frames %d-%d\n",
        kReuseEntities, kPopulateFrames - 1, kPopulateFrames, kPopulateFrames + kQueryFrames - 1);
    run_reuse_variant("distinct", false);
    run_reuse_variant("ambiguous", true);

    std::printf(
        "\nDEC-011 qualification: measurement only (DEC-017 item 6: cache gain is not a gate);\n"
        "synthetic scenes, valid solely on this machine. Real-screenshot reuse evaluation follows\n"
        "docs/benchmarks/evaluation-scenes.md offline conventions (data never enters the repository).\n");
    return 0;
}
