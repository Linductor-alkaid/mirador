#include <mirador/object_tracker.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/crop.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/patch_fingerprint.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/shift_estimation.hpp>
#include <mirador/status.hpp>
#include <mirador/visual_fingerprint.hpp>

#include "rect_math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mirador {
namespace {

/// PSR denominator epsilon of the E1 peak-quality formula (frozen in the
/// `verify_track` contract): keeps the ratio finite for flat surfaces and
/// single-candidate search sets.
constexpr double kPeakSidelobeEpsilon = 1e-12;
/// Denominator floor of the E2 relative deviation formula (frozen in the
/// `verify_track` contract): stabilizes near-zero baselines.
constexpr double kBaselineDeviationEpsilon = 1e-6;

/// True when every component is finite.
[[nodiscard]] bool all_finite(const RectF& rect) noexcept {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height);
}

/// Pixel ROI covering the float rect: floor on the leading edge, ceil on the
/// trailing edge (the same conservative rule the change ROI mapping uses),
/// clamped into the view. Returns nullopt for an empty clamp result.
[[nodiscard]] std::optional<RectI> covering_roi(const RectF& bounds, const ImageView& view) noexcept {
    const auto x0 = static_cast<int32_t>(std::floor(static_cast<double>(bounds.x)));
    const auto y0 = static_cast<int32_t>(std::floor(static_cast<double>(bounds.y)));
    const auto x1 = static_cast<int32_t>(std::ceil(static_cast<double>(bounds.x + bounds.width)));
    const auto y1 = static_cast<int32_t>(std::ceil(static_cast<double>(bounds.y + bounds.height)));
    const RectI clamped{std::max(x0, 0), std::max(y0, 0), std::min(x1, view.width) - std::max(x0, 0),
                        std::min(y1, view.height) - std::max(y0, 0)};
    if (clamped.width <= 0 || clamped.height <= 0) {
        return std::nullopt;
    }
    return clamped;
}

void truncate_text(std::string& text) noexcept {
    if (text.size() > ObjectTracker::kMaxSemanticsTextBytes) {
        text.resize(ObjectTracker::kMaxSemanticsTextBytes);
    }
}

/// True when every change ROI is a non-empty rect.
[[nodiscard]] bool rois_valid(const std::vector<RectI>& regions) noexcept {
    return std::all_of(regions.begin(), regions.end(),
                       [](const RectI& region) { return region.width > 0 && region.height > 0; });
}

/// Pool entry for one track under a given active-path decision: kTracking
/// tracks get `active_decision`, every other state an explicit kInactive.
[[nodiscard]] TrackGateDecision pool_entry(const TargetTrack& track,
                                           const ChangeGateDecision active_decision) noexcept {
    const bool active = track.state == TrackState::kTracking;
    return TrackGateDecision{track.track_id, track.state, active ? active_decision : ChangeGateDecision::kInactive,
                             std::nullopt};
}

/// First (scan-order) ROI intersecting `bounds`, or nullopt when the bounds
/// stay clear of every change ROI (edge-touching counts as clear). Integer
/// ROIs convert to float per comparison (exact), keeping the scan
/// allocation-free.
[[nodiscard]] std::optional<size_t> first_intersecting_roi(const std::vector<RectI>& regions,
                                                           const RectF& bounds) noexcept {
    for (size_t index = 0; index < regions.size(); ++index) {
        const RectF roi{static_cast<float>(regions[index].x), static_cast<float>(regions[index].y),
                        static_cast<float>(regions[index].width), static_cast<float>(regions[index].height)};
        if (fusion_internal::intersection_area(roi, bounds) > 0.0) {
            return index;
        }
    }
    return std::nullopt;
}

/// Appends the kPartial per-track verdicts (levels 2/3 of the gate) to
/// `trace`: one entry per track, independent per track, with the
/// cancellation/deadline checked per track. Returns the matching Status when
/// `context` aborts the scan mid-way (no partial trace is published — the
/// caller discards `trace`).
[[nodiscard]] Result<void> append_partial_trace(ChangeGateTrace& trace, const std::vector<RectI>& regions,
                                                const std::vector<TargetTrack>& tracks,
                                                const ExecutionContext& context) noexcept {
    for (const TargetTrack& track : tracks) {
        if (is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "evaluate_change_gate cancelled"};
        }
        if (deadline_reached(context)) {
            return Status{ErrorCode::kTimeout, "evaluate_change_gate deadline reached"};
        }
        if (track.state != TrackState::kTracking) {
            trace.tracks.push_back(pool_entry(track, ChangeGateDecision::kInactive));
            continue;
        }
        const std::optional<size_t> hit = first_intersecting_roi(regions, track.last_bounds);
        trace.tracks.push_back(
            TrackGateDecision{track.track_id, track.state,
                              hit.has_value() ? ChangeGateDecision::kVerify : ChangeGateDecision::kReuse, hit});
    }
    return Status::success();
}

// ---- M7-05 neighborhood-verification helpers ----

/// NCC of two same-size packed gray thumbnails: the exact normalization of
/// the VisualIndex template layer (M3-10) — mean-adjusted correlation with
/// its flat-side rule (identical flat thumbnails are a perfect match).
[[nodiscard]] double thumbnail_ncc(const VisualPatchFingerprint& query, const VisualPatchFingerprint& entry) noexcept {
    const size_t count = query.thumbnail_gray.size();
    if (count == 0 || entry.thumbnail_gray.size() != count) {
        return 0.0;
    }
    double query_mean = 0.0;
    double entry_mean = 0.0;
    for (size_t i = 0; i < count; ++i) {
        query_mean += std::to_integer<uint8_t>(query.thumbnail_gray[i]);
        entry_mean += std::to_integer<uint8_t>(entry.thumbnail_gray[i]);
    }
    query_mean /= static_cast<double>(count);
    entry_mean /= static_cast<double>(count);
    double covariance = 0.0;
    double query_variance = 0.0;
    double entry_variance = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double q = std::to_integer<uint8_t>(query.thumbnail_gray[i]) - query_mean;
        const double e = std::to_integer<uint8_t>(entry.thumbnail_gray[i]) - entry_mean;
        covariance += q * e;
        query_variance += q * q;
        entry_variance += e * e;
    }
    const double denominator = std::sqrt(query_variance * entry_variance);
    if (denominator <= 0.0) {
        return query.thumbnail_gray == entry.thumbnail_gray ? 1.0 : 0.0;
    }
    return covariance / denominator;
}

/// True when every descriptor quantity is finite and inside the frozen
/// [0, 1] proposal-contract range.
[[nodiscard]] bool descriptors_valid(const TrackStructureDescriptors& descriptors) noexcept {
    const auto in_range = [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; };
    return in_range(descriptors.closure_score) && in_range(descriptors.rectangularity) &&
           in_range(descriptors.edge_support);
}

/// Per-quantity relative deviations of `current` from `baseline` (frozen E2
/// formula; unclamped, see the `verify_track` contract).
struct StructureDeviation {
    double closure = 0.0;
    double rectangularity = 0.0;
    double edge_support = 0.0;
    double max = 0.0;
};

[[nodiscard]] StructureDeviation structure_deviation(const TrackStructureDescriptors& current,
                                                     const TrackStructureDescriptors& baseline) noexcept {
    const auto deviation = [](float now, float base) {
        return std::fabs(static_cast<double>(now) - static_cast<double>(base)) /
               std::max(std::fabs(static_cast<double>(base)), kBaselineDeviationEpsilon);
    };
    StructureDeviation result;
    result.closure = deviation(current.closure_score, baseline.closure_score);
    result.rectangularity = deviation(current.rectangularity, baseline.rectangularity);
    result.edge_support = deviation(current.edge_support, baseline.edge_support);
    result.max = std::max({result.closure, result.rectangularity, result.edge_support});
    return result;
}

/// The clamped pixel verification ROI (frozen rule shared by
/// `ObjectTracker::verification_roi` and `verify_track`): bounds expanded
/// around the predicted center by `ratio * diagonal / 2` per side, converted
/// with the adopt_track covering rule, clamped into the view.
[[nodiscard]] Result<RectI> clamped_verification_roi(const RectF& bounds, const PointF& predicted_center,
                                                     double diagonal_ratio, const ImageView& view) noexcept {
    const double diagonal = std::hypot(static_cast<double>(bounds.width), static_cast<double>(bounds.height));
    const double margin = diagonal_ratio * diagonal / 2.0;
    const RectF expanded{predicted_center.x - bounds.width / 2.0F - static_cast<float>(margin),
                         predicted_center.y - bounds.height / 2.0F - static_cast<float>(margin),
                         bounds.width + static_cast<float>(2.0 * margin),
                         bounds.height + static_cast<float>(2.0 * margin)};
    const auto clamped = covering_roi(expanded, view);
    if (!clamped.has_value()) {
        return Status{ErrorCode::kInvalidArgument, "verification ROI does not intersect the presented view"};
    }
    return *clamped;
}

/// Integer translation grid of the E1 search: every offset whose translated
/// bounds window stays fully inside the clamped ROI. When no offset
/// qualifies (edge-clamped ROI smaller than the window), exactly the
/// fallback offset (0, 0) is used (frozen in the `verify_track` contract).
struct OffsetGrid {
    int64_t dx_min = 0;
    int64_t dy_min = 0;
    int64_t cols = 1;
    int64_t rows = 1;
    /// True when this grid is the single-offset edge-clamped fallback.
    bool fallback = false;
};

[[nodiscard]] int64_t offset_count(const OffsetGrid& grid) noexcept {
    return grid.cols * grid.rows;
}

[[nodiscard]] int64_t offset_dx(const OffsetGrid& grid, int64_t index) noexcept {
    return grid.dx_min + index % grid.cols;
}

[[nodiscard]] int64_t offset_dy(const OffsetGrid& grid, int64_t index) noexcept {
    return grid.dy_min + index / grid.cols;
}

[[nodiscard]] OffsetGrid verification_offset_grid(const RectF& bounds, const RectI& roi) noexcept {
    const RectF search{static_cast<float>(roi.x), static_cast<float>(roi.y), static_cast<float>(roi.width),
                       static_cast<float>(roi.height)};
    const double dx_min = std::ceil(static_cast<double>(search.x) - static_cast<double>(bounds.x));
    const double dx_max =
        std::floor(static_cast<double>(search.x + search.width) - static_cast<double>(bounds.x + bounds.width));
    const double dy_min = std::ceil(static_cast<double>(search.y) - static_cast<double>(bounds.y));
    const double dy_max =
        std::floor(static_cast<double>(search.y + search.height) - static_cast<double>(bounds.y + bounds.height));
    OffsetGrid grid;
    if (dx_min <= dx_max && dy_min <= dy_max) {
        const auto dx_lo = static_cast<int64_t>(dx_min);
        const auto dx_hi = static_cast<int64_t>(dx_max);
        const auto dy_lo = static_cast<int64_t>(dy_min);
        const auto dy_hi = static_cast<int64_t>(dy_max);
        grid.dx_min = dx_lo;
        grid.dy_min = dy_lo;
        grid.cols = dx_hi - dx_lo + 1;
        grid.rows = dy_hi - dy_lo + 1;
        grid.fallback = false;
    } else {
        grid = OffsetGrid{};
        grid.fallback = true;
    }
    return grid;
}

/// Non-negative saturating product (planned-work arithmetic; any saturated
/// value exceeds every representable budget).
[[nodiscard]] int64_t saturating_mul(int64_t a, int64_t b) noexcept {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    if (a <= 0 || b <= 0) {
        return 0;
    }
    return a > kMax / b ? kMax : a * b;
}

/// Planned work of one E1 scan (frozen formula in the `verify_track`
/// contract): per offset twice the window bytes (crop copy plus fingerprint
/// read) plus the thumbnail bytes plus the per-template NCC reads, plus one
/// response double per offset and template. Saturates at int64 max — any
/// saturated value exceeds every representable budget.
[[nodiscard]] int64_t planned_scan_work(const OffsetGrid& grid, size_t templates_count, const RectF& bounds,
                                        int32_t pixel_bytes, const ObjectTrackerOptions& options) noexcept {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    const auto side = static_cast<int64_t>(options.template_thumb_side);
    const auto templates = static_cast<int64_t>(templates_count);
    const int64_t window_w = std::max<int64_t>(static_cast<int64_t>(std::ceil(static_cast<double>(bounds.width))), 1);
    const int64_t window_h = std::max<int64_t>(static_cast<int64_t>(std::ceil(static_cast<double>(bounds.height))), 1);
    const int64_t per_offset = 2 * window_w * window_h * pixel_bytes + side * side + 2 * templates * side * side;
    const int64_t offsets = offset_count(grid);
    const int64_t scan_work = saturating_mul(offsets, per_offset);
    const int64_t surface_bytes = saturating_mul(offsets, templates * 8);
    if (scan_work == kMax || surface_bytes == kMax || scan_work > kMax - surface_bytes) {
        return kMax;
    }
    return scan_work + surface_bytes;
}

/// Peak and peak-sidelobe quality of one template's response surface, under
/// the frozen total orders (peak: NCC desc, Chebyshev radius asc, dy asc,
/// dx asc; PSR per the `verify_track` contract).
struct SurfacePeak {
    double ncc = 0.0;
    double psr = 0.0;
    size_t index = 0;
};

[[nodiscard]] SurfacePeak surface_peak(const std::vector<double>& responses, const OffsetGrid& grid) noexcept {
    const auto offset_radius = [](int64_t dx, int64_t dy) {
        const auto abs_x = dx < 0 ? -dx : dx;
        const auto abs_y = dy < 0 ? -dy : dy;
        return abs_x > abs_y ? abs_x : abs_y;
    };
    size_t best = 0;
    int64_t best_dx = offset_dx(grid, 0);
    int64_t best_dy = offset_dy(grid, 0);
    int64_t best_radius = offset_radius(best_dx, best_dy);
    for (size_t i = 1; i < responses.size(); ++i) {
        const int64_t dx = offset_dx(grid, static_cast<int64_t>(i));
        const int64_t dy = offset_dy(grid, static_cast<int64_t>(i));
        const int64_t radius = offset_radius(dx, dy);
        const bool better =
            responses[i] > responses[best] ||
            (responses[i] == responses[best] &&
             (radius < best_radius || (radius == best_radius && (dy < best_dy || (dy == best_dy && dx < best_dx)))));
        if (better) {
            best = i;
            best_dx = dx;
            best_dy = dy;
            best_radius = radius;
        }
    }
    const double peak = responses[best];
    double sidelobe_sum = 0.0;
    size_t sidelobe_count = 0;
    for (size_t i = 0; i < responses.size(); ++i) {
        if (i != best) {
            sidelobe_sum += responses[i];
            ++sidelobe_count;
        }
    }
    const double mean = sidelobe_count > 0 ? sidelobe_sum / static_cast<double>(sidelobe_count) : 0.0;
    double variance_sum = 0.0;
    for (size_t i = 0; i < responses.size(); ++i) {
        if (i != best) {
            const double d = responses[i] - mean;
            variance_sum += d * d;
        }
    }
    const double stddev = sidelobe_count > 0 ? std::sqrt(variance_sum / static_cast<double>(sidelobe_count)) : 0.0;
    return SurfacePeak{peak, (peak - mean) / (stddev + kPeakSidelobeEpsilon), best};
}

/// E1 scan: extracts and scores every candidate offset of the grid with the
/// adoption pipeline and fills one response row per template. Cancellation is
/// polled once per offset row; on any error only the Status is returned and
/// the (discarded) responses hold no published result.
[[nodiscard]] Result<void> scan_appearance_responses(const TargetTrack& track, const ImageView& view,
                                                     const OffsetGrid& grid, int64_t budget, int32_t thumb_side,
                                                     const ExecutionContext& context,
                                                     std::vector<std::vector<double>>& responses) noexcept {
    size_t index = 0;
    for (int64_t row = 0; row < grid.rows; ++row) {
        if (is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "verify_track cancelled"};
        }
        if (deadline_reached(context)) {
            return Status{ErrorCode::kTimeout, "verify_track deadline reached"};
        }
        for (int64_t col = 0; col < grid.cols; ++col, ++index) {
            const RectF window{track.last_bounds.x + static_cast<float>(grid.dx_min + col),
                               track.last_bounds.y + static_cast<float>(grid.dy_min + row), track.last_bounds.width,
                               track.last_bounds.height};
            const auto candidate_roi = covering_roi(window, view);
            if (!candidate_roi.has_value()) {
                return Status{ErrorCode::kInvalidArgument, "verify_track: track bounds cover no pixel of the view"};
            }
            auto cropped = crop(view, *candidate_roi, budget);
            if (!cropped.ok()) {
                return cropped.status();
            }
            auto candidate =
                make_visual_patch_fingerprint(cropped.value().view(), PatchFingerprintParams{thumb_side}, budget);
            if (!candidate.ok()) {
                return candidate.status();
            }
            for (size_t t = 0; t < track.templates.size(); ++t) {
                responses[t][index] = thumbnail_ncc(candidate.value(), track.templates[t].fingerprint);
            }
        }
    }
    return Status::success();
}

/// E1 verdict: winning template under (peak NCC, then PSR, then template
/// index), threshold rule with the flatness gate (frozen in the verify_track
/// contract).
[[nodiscard]] AppearanceVerification appearance_verdict(const std::vector<SurfacePeak>& peaks, const OffsetGrid& grid,
                                                        const ObjectTrackerOptions& options) noexcept {
    size_t winner = 0;
    for (size_t t = 1; t < peaks.size(); ++t) {
        const bool better =
            peaks[t].ncc > peaks[winner].ncc || (peaks[t].ncc == peaks[winner].ncc && peaks[t].psr > peaks[winner].psr);
        if (better) {
            winner = t;
        }
    }
    const SurfacePeak& best = peaks[winner];
    AppearanceVerification appearance;
    const bool trusted = best.psr >= options.peak_sidelobe_ratio_min;
    if (trusted && best.ncc >= options.ncc_strong_threshold) {
        appearance.outcome = AppearanceChannelOutcome::kStrong;
    } else if (trusted && best.ncc >= options.ncc_weak_threshold) {
        appearance.outcome = AppearanceChannelOutcome::kWeak;
    }
    appearance.peak_ncc = best.ncc;
    appearance.peak_sidelobe_ratio = best.psr;
    appearance.best_template_index = static_cast<uint32_t>(winner);
    appearance.best_offset_dx = static_cast<int32_t>(offset_dx(grid, static_cast<int64_t>(best.index)));
    appearance.best_offset_dy = static_cast<int32_t>(offset_dy(grid, static_cast<int64_t>(best.index)));
    return appearance;
}

/// E2 verdict: missing inputs report kNotSupplied/kNoBaseline explicitly;
/// otherwise the frozen deviation formula compares against the baseline and
/// the tolerance decides kConsistent/kDeviated.
[[nodiscard]] StructureVerification structure_verdict(const std::optional<TrackStructureDescriptors>& descriptors,
                                                      const TrackStructureDescriptors* baseline,
                                                      const ObjectTrackerOptions& options) noexcept {
    StructureVerification structure;
    if (!descriptors.has_value()) {
        return structure;
    }
    if (baseline == nullptr) {
        structure.outcome = StructureChannelOutcome::kNoBaseline;
        return structure;
    }
    const StructureDeviation deviation = structure_deviation(*descriptors, *baseline);
    structure.closure_deviation = deviation.closure;
    structure.rectangularity_deviation = deviation.rectangularity;
    structure.edge_support_deviation = deviation.edge_support;
    structure.max_deviation = deviation.max;
    structure.outcome = deviation.max <= options.structure_deviation_tolerance ? StructureChannelOutcome::kConsistent
                                                                               : StructureChannelOutcome::kDeviated;
    return structure;
}

// ---- M7-06 evidence-fusion helpers ----

/// Appearance strength of one verification under the frozen M7-06 lift rule:
/// E2 consistency is strong evidence on its own (design section 3 "强" —
/// NCC peak+PSR dual pass OR closure structure consistent); the E2 channel
/// has no weak level of its own.
enum class AppearanceLevel : uint8_t { kNone, kWeak, kStrong };

[[nodiscard]] AppearanceLevel appearance_level(const AppearanceVerification& appearance,
                                               const StructureVerification& structure) noexcept {
    if (appearance.outcome == AppearanceChannelOutcome::kStrong ||
        structure.outcome == StructureChannelOutcome::kConsistent) {
        return AppearanceLevel::kStrong;
    }
    return appearance.outcome == AppearanceChannelOutcome::kWeak ? AppearanceLevel::kWeak : AppearanceLevel::kNone;
}

/// DEC-010 semantic gate predicate (category compatibility): a conflict
/// exists only when both labels are non-empty and different; an empty label
/// on either side is vacuously compatible.
[[nodiscard]] bool semantics_conflict(const TrackSemantics& track_semantics, const TrackSemantics& candidate) noexcept {
    return !track_semantics.label.empty() && !candidate.label.empty() && track_semantics.label != candidate.label;
}

/// Impostor veto evidence: the candidate patch matches any stored negative
/// template at or above the configured threshold (multi-template best;
/// first hit wins, the answer is boolean).
[[nodiscard]] bool impostor_match(const VisualPatchFingerprint& patch, const std::vector<TrackTemplate>& negatives,
                                  double threshold) noexcept {
    const auto hits_threshold = [&patch, threshold](const TrackTemplate& entry) {
        return thumbnail_ncc(patch, entry.fingerprint) >= threshold;
    };
    return std::ranges::any_of(negatives, hits_threshold);
}

/// Position-gate admission (frozen M7-06 rule): the gate binds unless it is
/// void — a kGenerationSwitch scenario zeroes the position prior, and a
/// kLost track's stale prior must not gate a recapture.
[[nodiscard]] bool gate_admitted(TrackState state, PositionScenario scenario, bool inside_gate) noexcept {
    return inside_gate || scenario == PositionScenario::kGenerationSwitch || state == TrackState::kLost;
}

/// Frozen grade table (first hit wins): vetoes first, then appearance
/// strength with the gate, else the position-only placeholder.
[[nodiscard]] EvidenceGrade fusion_grade(AppearanceLevel level, bool gate_ok, bool impostor, bool conflict) noexcept {
    if (impostor || conflict) {
        return EvidenceGrade::kVetoed;
    }
    if (level == AppearanceLevel::kStrong) {
        return gate_ok ? EvidenceGrade::kConfirmed : EvidenceGrade::kPlaceholder;
    }
    if (level == AppearanceLevel::kWeak) {
        return gate_ok ? EvidenceGrade::kTentative : EvidenceGrade::kPlaceholder;
    }
    return EvidenceGrade::kPlaceholder;
}

/// Frozen state transition of one commit (see `commit_track_evidence`):
/// confirming grades recapture and reset the streak, insufficient grades
/// degrade toward kLost at the limit, kLost is sticky.
struct TrackTransition {
    TrackState state = TrackState::kTracking;
    uint32_t insufficient_streak = 0;
    uint64_t lost_sequence = 0;
};

[[nodiscard]] TrackTransition track_transition(TrackState state, EvidenceGrade grade, uint32_t current_streak,
                                               uint64_t current_lost_sequence, int32_t uncertain_limit,
                                               uint64_t frame_sequence) noexcept {
    TrackTransition next;
    next.insufficient_streak = current_streak;
    next.lost_sequence = current_lost_sequence;
    if (grade == EvidenceGrade::kConfirmed || grade == EvidenceGrade::kTentative) {
        next.state = TrackState::kTracking;
        next.insufficient_streak = 0;
        next.lost_sequence = 0;
        return next;
    }
    next.state = state;
    if (state == TrackState::kLost) {
        return next;
    }
    next.state = TrackState::kUncertain;
    next.insufficient_streak = current_streak + 1U;
    if (next.insufficient_streak >= static_cast<uint32_t>(uncertain_limit)) {
        next.state = TrackState::kLost;
        next.lost_sequence = frame_sequence;
    }
    return next;
}

/// Planned (not yet applied) template capture of one commit. Both template
/// stores hold same-size thumbnails (the store paths pin
/// `template_thumb_side`), so an eviction under count pressure always frees
/// exactly the bytes the insertion needs — the capture is byte-neutral on a
/// full set, exactly like a `record_observation` swap.
struct TemplateCapturePlan {
    bool capture = false;
    bool evict_oldest = false;
};

[[nodiscard]] TemplateCapturePlan positive_capture_plan(size_t template_count, int32_t max_templates) noexcept {
    TemplateCapturePlan plan;
    // The update policy needs an evictable slot beyond the pinned adoption
    // template (index 0); with max_templates == 1 it is off.
    plan.capture = max_templates >= 2;
    plan.evict_oldest = plan.capture && template_count >= static_cast<size_t>(max_templates);
    return plan;
}

[[nodiscard]] TemplateCapturePlan negative_capture_plan(size_t negative_count, int32_t max_negatives) noexcept {
    TemplateCapturePlan plan;
    plan.capture = max_negatives >= 1;
    plan.evict_oldest = plan.capture && negative_count >= static_cast<size_t>(max_negatives);
    return plan;
}

/// Validation gate of one `commit_track_evidence` call (frozen M7-06 order:
/// validation precedes cancellation, the deliberate contrast to
/// `adopt_track`'s M7-02 cancel-first entry). `track` may be null.
[[nodiscard]] Status validate_commit_inputs(const ImageView& presented_view, const TargetTrack* track,
                                            const TrackVerification& verification) noexcept {
    if (auto validated = validate(presented_view); !validated.ok()) {
        return validated.status();
    }
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "commit_track_evidence: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "commit_track_evidence: track already terminated"};
    }
    if (verification.track_id != track->track_id) {
        return Status{ErrorCode::kInvalidArgument, "commit_track_evidence: verification carries another track's id"};
    }
    if (!all_finite(track->last_bounds) || track->last_bounds.width <= 0.0F || track->last_bounds.height <= 0.0F) {
        return Status{ErrorCode::kInvalidArgument, "commit_track_evidence: track bounds must be finite and non-empty"};
    }
    return Status::success();
}

/// Candidate window and its patch (frozen rule): the E1 best offset when the
/// appearance channel has an outcome, else the unchanged bounds — an E2-only
/// confirmation has no position of its own. One extraction with the adoption
/// pipeline serves the impostor check and both captures.
[[nodiscard]] Result<VisualPatchFingerprint> extract_candidate_patch(const TargetTrack& track, bool has_candidate,
                                                                     const TrackVerification& verification,
                                                                     const ImageView& presented_view,
                                                                     const ObjectTrackerOptions& options) noexcept {
    const float offset_x = has_candidate ? static_cast<float>(verification.appearance.best_offset_dx) : 0.0F;
    const float offset_y = has_candidate ? static_cast<float>(verification.appearance.best_offset_dy) : 0.0F;
    const RectF candidate_window{track.last_bounds.x + offset_x, track.last_bounds.y + offset_y,
                                 track.last_bounds.width, track.last_bounds.height};
    const auto candidate_roi = covering_roi(candidate_window, presented_view);
    if (!candidate_roi.has_value()) {
        return Status{ErrorCode::kInvalidArgument,
                      "commit_track_evidence: candidate window covers no pixel of the view"};
    }
    auto cropped = crop(presented_view, *candidate_roi, options.verification_work_budget_bytes);
    if (!cropped.ok()) {
        return cropped.status();
    }
    return make_visual_patch_fingerprint(cropped.value().view(), PatchFingerprintParams{options.template_thumb_side},
                                         options.verification_work_budget_bytes);
}

/// Applies one planned template capture: under count pressure the
/// drop-oldest entry goes first (the `add_template` rule — index 0 of the
/// appearance set is the pinned adoption template), then the entry is
/// appended and the pool byte account updated. A full set swaps
/// byte-neutrally (uniform pinned thumbnail size).
void apply_template_capture(std::vector<TrackTemplate>& set, size_t evict_index, bool evict_oldest,
                            const TrackTemplate& entry, int64_t template_bytes, uint64_t& evicted_counter,
                            int64_t& used_bytes) noexcept {
    if (evict_oldest) {
        set.erase(set.begin() + static_cast<std::ptrdiff_t>(evict_index));
        ++evicted_counter;
        used_bytes -= template_bytes;
    }
    set.push_back(entry);
    used_bytes += template_bytes;
}

// ---- M7-07 global-motion and generation-pipeline helpers ----

/// Planned finiteness of one track's compensation (evaluated before any
/// mutation so the whole-pool application stays atomic).
[[nodiscard]] bool translation_stays_finite(const RectF& bounds, float dx, float dy) noexcept {
    return std::isfinite(bounds.x + dx) && std::isfinite(bounds.y + dy);
}

/// Applies the uniform compensation translation to one live track:
/// `last_bounds` moves by (dx, dy) and `predicted_center` is recomputed as
/// the exact new center — the frozen invariant, with the same formula as
/// adoption and confirming commits.
void apply_compensation(TargetTrack& track, float dx, float dy) noexcept {
    track.last_bounds =
        RectF{track.last_bounds.x + dx, track.last_bounds.y + dy, track.last_bounds.width, track.last_bounds.height};
    track.predicted_center = PointF{track.last_bounds.x + track.last_bounds.width / 2.0F,
                                    track.last_bounds.y + track.last_bounds.height / 2.0F};
}

/// Everything one commit computes before any mutation (frozen table and
/// capture policies; see the `commit_track_evidence` class contract).
struct CommitPlan {
    EvidenceGrade grade = EvidenceGrade::kPlaceholder;
    bool impostor = false;
    bool conflict = false;
    bool has_candidate = false;
    TrackTransition transition;
    bool slot_exists = false;
    bool slot_allocation_needed = false;
    TemplateCapturePlan positive_plan;
    TemplateCapturePlan negative_plan;
    bool want_positive = false;
    bool want_negative = false;
};

[[nodiscard]] CommitPlan plan_track_commit(const TargetTrack& track, const TrackVerification& verification,
                                           const TrackPositionEvidence& position,
                                           const std::optional<TrackSemantics>& candidate_semantics,
                                           const VisualPatchFingerprint& patch, bool slot_exists,
                                           uint32_t current_streak, uint64_t current_lost_sequence,
                                           uint64_t frame_sequence, const ObjectTrackerOptions& options) noexcept {
    CommitPlan plan;
    plan.has_candidate = verification.appearance.outcome != AppearanceChannelOutcome::kNone;
    const AppearanceLevel level = appearance_level(verification.appearance, verification.structure);
    plan.conflict = candidate_semantics.has_value() && semantics_conflict(track.semantics, *candidate_semantics);
    plan.impostor = plan.has_candidate && !track.negative_templates.empty() &&
                    impostor_match(patch, track.negative_templates, options.impostor_match_threshold);
    const bool gate_ok = gate_admitted(track.state, position.scenario, position.inside_gate);
    plan.grade = fusion_grade(level, gate_ok, plan.impostor, plan.conflict);
    plan.transition = track_transition(track.state, plan.grade, current_streak, current_lost_sequence,
                                       options.uncertain_frame_limit, frame_sequence);
    plan.slot_exists = slot_exists;
    plan.slot_allocation_needed =
        !slot_exists && (plan.transition.insufficient_streak != 0 || plan.transition.lost_sequence != 0);
    plan.positive_plan = positive_capture_plan(track.templates.size(), options.max_templates);
    plan.want_positive = plan.grade == EvidenceGrade::kConfirmed && plan.positive_plan.capture;
    plan.negative_plan = negative_capture_plan(track.negative_templates.size(), options.max_negative_templates);
    plan.want_negative =
        plan.grade == EvidenceGrade::kVetoed && plan.conflict && !plan.impostor && plan.negative_plan.capture;
    return plan;
}

/// Planned byte growth of the commit's stores (0 when every store swaps
/// byte-neutrally or is skipped by its policy).
[[nodiscard]] int64_t planned_store_delta(const CommitPlan& plan, int64_t template_bytes) noexcept {
    int64_t delta = plan.slot_allocation_needed ? ObjectTracker::kStateSlotOverheadBytes : 0;
    if (plan.want_positive && !plan.positive_plan.evict_oldest) {
        delta += template_bytes;
    }
    if (plan.want_negative && !plan.negative_plan.evict_oldest) {
        delta += template_bytes;
    }
    return delta;
}

/// Mutation phase of one commit over the public record type: the planned
/// template captures and the confirming-grade evidence-field updates (the
/// vetoed candidate must never move the track; the position prior is not
/// appearance evidence — the M7-03 freeze). Slot bookkeeping stays with the
/// caller (pool-private storage). Runs only after budget clearance.
void apply_commit_stores(TargetTrack& track, const CommitPlan& plan, const TrackVerification& verification,
                         const VisualPatchFingerprint& patch, uint64_t frame_sequence, uint32_t pool_layout_generation,
                         int64_t template_bytes, uint64_t& evicted_templates, uint64_t& evicted_negative_templates,
                         int64_t& used_bytes) noexcept {
    if (plan.want_positive) {
        apply_template_capture(track.templates, 1, plan.positive_plan.evict_oldest,
                               TrackTemplate{patch, frame_sequence, pool_layout_generation, EvidenceGrade::kConfirmed},
                               template_bytes, evicted_templates, used_bytes);
    }
    if (plan.want_negative) {
        apply_template_capture(track.negative_templates, 0, plan.negative_plan.evict_oldest,
                               TrackTemplate{patch, frame_sequence, pool_layout_generation, EvidenceGrade::kVetoed},
                               template_bytes, evicted_negative_templates, used_bytes);
    }
    if (plan.grade == EvidenceGrade::kConfirmed || plan.grade == EvidenceGrade::kTentative) {
        const float offset_x = plan.has_candidate ? static_cast<float>(verification.appearance.best_offset_dx) : 0.0F;
        const float offset_y = plan.has_candidate ? static_cast<float>(verification.appearance.best_offset_dy) : 0.0F;
        const RectF candidate_window{track.last_bounds.x + offset_x, track.last_bounds.y + offset_y,
                                     track.last_bounds.width, track.last_bounds.height};
        track.last_bounds = candidate_window;
        track.predicted_center = PointF{candidate_window.x + candidate_window.width / 2.0F,
                                        candidate_window.y + candidate_window.height / 2.0F};
        if (plan.has_candidate) {
            track.confidence = std::clamp(static_cast<float>(verification.appearance.peak_ncc), 0.0F, 1.0F);
        }
        track.last_verified_sequence = frame_sequence;
    }
}

// ---- M7-08 cascade-redetection helpers ----

/// Backoff wait of the n-th consecutive accounted failure (n >= 1), frozen
/// formula `min(base * 2^(n-1), max)` over the M7-01 options: the doubling
/// sequence in frames, capped. The loop breaks at the cap, so it runs at
/// most log2(max/base) + 1 doublings; `create` validates base in [1, max],
/// so no overflow is reachable (wait <= max <= int32 max throughout).
[[nodiscard]] int64_t redetection_backoff_wait(const uint32_t failure_index,
                                               const ObjectTrackerOptions& options) noexcept {
    const int64_t base = options.redetect_backoff_base_frames;
    const int64_t max_wait = options.redetect_backoff_max_frames;
    int64_t wait = base;
    for (uint32_t i = 1; i < failure_index && wait < max_wait; ++i) {
        wait = std::min(wait * 2, max_wait);
    }
    return wait;
}

/// Saturating frame-sequence addition for the scheduled next attempt (the
/// recorded sequence plus the backoff wait): a caller sequence near the
/// uint64 maximum saturates instead of wrapping into the past.
[[nodiscard]] uint64_t saturating_sequence_add(const uint64_t sequence, const int64_t wait) noexcept {
    constexpr uint64_t kMaxSequence = std::numeric_limits<uint64_t>::max();
    if (wait <= 0 || sequence > kMaxSequence - static_cast<uint64_t>(wait)) {
        return kMaxSequence;
    }
    return sequence + static_cast<uint64_t>(wait);
}

}  // namespace

int64_t ObjectTracker::track_bytes(const TargetTrack& track) noexcept {
    int64_t bytes = kTrackOverheadBytes;
    const auto template_bytes = [&](const std::vector<TrackTemplate>& set) {
        int64_t sum = 0;
        for (const TrackTemplate& entry : set) {
            sum += kTemplateOverheadBytes + static_cast<int64_t>(entry.fingerprint.thumbnail_gray.size());
        }
        return sum;
    };
    bytes += template_bytes(track.templates);
    bytes += template_bytes(track.negative_templates);
    bytes += static_cast<int64_t>(track.position_history.size()) * kObservationOverheadBytes;
    bytes += static_cast<int64_t>(track.semantics.label.size());
    bytes += static_cast<int64_t>(track.semantics.text.size());
    return bytes;
}

bool ObjectTracker::evicts_before(const TargetTrack& candidate, const TargetTrack& resident) noexcept {
    const bool candidate_terminated = candidate.state == TrackState::kTerminated;
    const bool resident_terminated = resident.state == TrackState::kTerminated;
    if (candidate_terminated != resident_terminated) {
        return candidate_terminated;
    }
    if (candidate.last_verified_sequence != resident.last_verified_sequence) {
        return candidate.last_verified_sequence < resident.last_verified_sequence;
    }
    return candidate.track_id < resident.track_id;
}

int64_t ObjectTracker::baseline_slot_bytes(uint64_t track_id) const noexcept {
    return find_baseline_slot(track_id) != structure_baselines_.cend() ? kStructureBaselineOverheadBytes : 0;
}

ObjectTracker::StateSlots::iterator ObjectTracker::find_state_slot(uint64_t track_id) noexcept {
    return std::lower_bound(state_slots_.begin(), state_slots_.end(), track_id,
                            [](const std::pair<uint64_t, StateSlot>& slot, uint64_t id) { return slot.first < id; });
}

ObjectTracker::StateSlots::const_iterator ObjectTracker::find_state_slot(uint64_t track_id) const noexcept {
    return std::lower_bound(state_slots_.cbegin(), state_slots_.cend(), track_id,
                            [](const std::pair<uint64_t, StateSlot>& slot, uint64_t id) { return slot.first < id; });
}

int64_t ObjectTracker::state_slot_bytes(uint64_t track_id) const noexcept {
    return find_state_slot(track_id) != state_slots_.cend() ? kStateSlotOverheadBytes : 0;
}

int64_t ObjectTracker::erase_state_slot(uint64_t track_id) noexcept {
    const auto slot = find_state_slot(track_id);
    if (slot == state_slots_.end() || slot->first != track_id) {
        return 0;
    }
    state_slots_.erase(slot);
    return kStateSlotOverheadBytes;
}

ObjectTracker::RedetectSlots::iterator ObjectTracker::find_redetect_slot(uint64_t track_id) noexcept {
    return std::lower_bound(redetect_slots_.begin(), redetect_slots_.end(), track_id,
                            [](const std::pair<uint64_t, RedetectSlot>& slot, uint64_t id) { return slot.first < id; });
}

ObjectTracker::RedetectSlots::const_iterator ObjectTracker::find_redetect_slot(uint64_t track_id) const noexcept {
    return std::lower_bound(redetect_slots_.cbegin(), redetect_slots_.cend(), track_id,
                            [](const std::pair<uint64_t, RedetectSlot>& slot, uint64_t id) { return slot.first < id; });
}

int64_t ObjectTracker::redetect_slot_bytes(uint64_t track_id) const noexcept {
    return find_redetect_slot(track_id) != redetect_slots_.cend() ? kRedetectSlotOverheadBytes : 0;
}

int64_t ObjectTracker::erase_redetect_slot(uint64_t track_id) noexcept {
    const auto slot = find_redetect_slot(track_id);
    if (slot == redetect_slots_.end() || slot->first != track_id) {
        return 0;
    }
    redetect_slots_.erase(slot);
    return kRedetectSlotOverheadBytes;
}

void ObjectTracker::archive_track(TargetTrack& track, uint64_t frame_sequence) noexcept {
    const int64_t before = track_bytes(track);
    track.state = TrackState::kTerminated;
    track.terminated_sequence = frame_sequence;
    track.templates.clear();
    track.negative_templates.clear();
    track.position_history.clear();
    track.templates.shrink_to_fit();
    track.negative_templates.shrink_to_fit();
    track.position_history.shrink_to_fit();
    used_bytes_ -= before - track_bytes(track);
    // The pool-side E2 baseline slot is released with the track's evidence
    // data (archives stay cheap, M7-01 freeze semantics), and so are the
    // M7-06 state-machine slot and the M7-08 redetection-episode slot.
    used_bytes_ -= erase_structure_baseline(track.track_id);
    used_bytes_ -= erase_state_slot(track.track_id);
    used_bytes_ -= erase_redetect_slot(track.track_id);
}

ObjectTracker::EvictionPlan ObjectTracker::plan_eviction(int64_t insertion_bytes) const noexcept {
    EvictionPlan plan;
    const auto already_victim = [&plan](uint64_t id) {
        return std::find(plan.victim_ids.begin(), plan.victim_ids.end(), id) != plan.victim_ids.end();
    };
    const auto max_count = static_cast<size_t>(options_.max_targets);
    while (tracks_.size() - plan.victim_ids.size() >= max_count ||
           used_bytes_ - plan.freed_bytes + insertion_bytes > options_.pool_budget_bytes) {
        const TargetTrack* victim = nullptr;
        for (const TargetTrack& candidate : tracks_) {
            if (already_victim(candidate.track_id)) {
                continue;
            }
            if (victim == nullptr || evicts_before(candidate, *victim)) {
                victim = &candidate;
            }
        }
        plan.victim_ids.push_back(victim->track_id);
        plan.freed_bytes += track_bytes(*victim) + baseline_slot_bytes(victim->track_id) +
                            state_slot_bytes(victim->track_id) + redetect_slot_bytes(victim->track_id);
    }
    return plan;
}

Result<ObjectTracker> ObjectTracker::create(const ObjectTrackerOptions& options) noexcept {
    const bool ranges_ok =
        options.max_targets >= 1 && options.max_targets <= 4096 && options.max_position_history >= 1 &&
        options.max_position_history <= 1024 && options.max_templates >= 1 && options.max_templates <= 16 &&
        options.max_negative_templates >= 0 && options.max_negative_templates <= 16 &&
        options.template_thumb_side >= 8 && options.template_thumb_side <= 64 && options.pool_budget_bytes > 0 &&
        options.uncertain_frame_limit >= 1 && options.uncertain_frame_limit <= 4096 &&
        options.max_generation_lag >= 1 && options.max_generation_lag <= 1024;
    if (!ranges_ok) {
        return Status{ErrorCode::kInvalidArgument, "ObjectTrackerOptions value outside its documented range"};
    }
    if (options.ncc_weak_threshold < 0.0 || options.ncc_weak_threshold > 1.0 ||
        options.ncc_strong_threshold < options.ncc_weak_threshold || options.ncc_strong_threshold > 1.0) {
        return Status{ErrorCode::kInvalidArgument, "ncc thresholds must satisfy 0 <= weak <= strong <= 1"};
    }
    if (options.peak_sidelobe_ratio_min < 1.0 || options.structure_deviation_tolerance < 0.0 ||
        options.structure_deviation_tolerance > 1.0 || options.verification_roi_diagonal_ratio <= 0.0 ||
        options.verification_roi_diagonal_ratio > 8.0 || options.verification_work_budget_bytes <= 0) {
        return Status{ErrorCode::kInvalidArgument, "verification threshold outside its documented range"};
    }
    if (options.impostor_match_threshold < 0.0 || options.impostor_match_threshold > 1.0) {
        return Status{ErrorCode::kInvalidArgument, "impostor_match_threshold must be in [0, 1]"};
    }
    if (options.min_compensation_confidence < 0.0 || options.min_compensation_confidence > 1.0) {
        return Status{ErrorCode::kInvalidArgument, "min_compensation_confidence must be in [0, 1]"};
    }
    if (options.redetect_backoff_base_frames < 1 ||
        options.redetect_backoff_max_frames < options.redetect_backoff_base_frames ||
        options.redetect_max_attempts < 1) {
        return Status{ErrorCode::kInvalidArgument, "redetection backoff parameters invalid"};
    }
    if (options.max_redetection_records < 1 || options.max_redetection_records > 4096) {
        return Status{ErrorCode::kInvalidArgument, "max_redetection_records outside its documented range"};
    }
    ObjectTracker tracker;
    tracker.options_ = options;
    return tracker;
}

Result<TrackAdoption> ObjectTracker::adopt_track(const VisualRegion& region, const ImageView& presented_view,
                                                 uint64_t frame_sequence, const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "adopt_track cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "adopt_track deadline reached"};
    }
    if (region.stable_id == 0) {
        return Status{ErrorCode::kInvalidArgument, "adopt_track requires a non-zero stable_id"};
    }
    if (!all_finite(region.bounds) || region.bounds.width <= 0.0F || region.bounds.height <= 0.0F) {
        return Status{ErrorCode::kInvalidArgument, "adopt_track region bounds must be finite and non-empty"};
    }
    if (auto validated = validate(presented_view); !validated.ok()) {
        return validated.status();
    }
    if (region.bounds.x < 0.0F || region.bounds.y < 0.0F ||
        region.bounds.x + region.bounds.width > static_cast<float>(presented_view.width) ||
        region.bounds.y + region.bounds.height > static_cast<float>(presented_view.height)) {
        return Status{ErrorCode::kInvalidArgument, "adopt_track region bounds must lie inside the presented view"};
    }
    if (find_track(region.stable_id) != nullptr) {
        return Status{ErrorCode::kInvalidArgument, "adopt_track id is already tracked"};
    }

    const auto roi = covering_roi(region.bounds, presented_view);
    if (!roi.has_value()) {
        return Status{ErrorCode::kInvalidArgument, "adopt_track region does not cover any pixel"};
    }

    // Worst-case insertion account (track + template + observation + untruncated
    // semantics). A track larger than the whole budget can never fit and fails
    // before anything is evicted, so every error below leaves the pool untouched.
    const int64_t semantics_bytes =
        static_cast<int64_t>(region.label.size()) + static_cast<int64_t>(region.text.size());
    const int64_t thumbnail_bytes =
        static_cast<int64_t>(options_.template_thumb_side) * static_cast<int64_t>(options_.template_thumb_side);
    const int64_t insertion_bytes =
        kTrackOverheadBytes + kTemplateOverheadBytes + thumbnail_bytes + kObservationOverheadBytes + semantics_bytes;
    if (insertion_bytes > options_.pool_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded, "track cannot fit the pool budget"};
    }

    // Plan the eviction (count pressure, then byte pressure) without mutating
    // the pool: terminated tracks first, then oldest by (last_verified_sequence,
    // track_id); every victim id is reported (RULE-06 explicit eviction).
    // A victim's pool-side E2 baseline (M7-05), state-machine (M7-06) and
    // redetection-episode (M7-08) slots are freed with it.
    const EvictionPlan plan = plan_eviction(insertion_bytes);

    // Capture the adoption template before any eviction commits: the capture
    // budget is the pool headroom the planned eviction frees up, so an
    // exact-fit pool can still evict. Any capture error returns before the
    // pool is mutated ("failure leaves the pool untouched").
    const int64_t capture_budget = options_.pool_budget_bytes - used_bytes_ + plan.freed_bytes;
    auto cropped = crop(presented_view, *roi, capture_budget);
    if (!cropped.ok()) {
        return cropped.status();
    }
    auto fingerprint = make_visual_patch_fingerprint(
        cropped.value().view(), PatchFingerprintParams{options_.template_thumb_side}, capture_budget);
    if (!fingerprint.ok()) {
        return fingerprint.status();
    }

    // Commit: evict the planned victims (with their pool-side slots), then
    // insert the new track.
    TrackAdoption adoption;
    adoption.track_id = region.stable_id;
    adoption.evicted_track_ids = plan.victim_ids;
    for (const uint64_t victim_id : adoption.evicted_track_ids) {
        erase_structure_baseline(victim_id);
        erase_state_slot(victim_id);
        erase_redetect_slot(victim_id);
        const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                     [victim_id](const TargetTrack& track) { return track.track_id == victim_id; });
        tracks_.erase(it);
        ++evicted_count_;
    }
    used_bytes_ -= plan.freed_bytes;

    TargetTrack track;
    track.track_id = region.stable_id;
    track.state = TrackState::kTracking;
    track.last_bounds = region.bounds;
    track.predicted_center =
        PointF{region.bounds.x + region.bounds.width / 2.0F, region.bounds.y + region.bounds.height / 2.0F};
    track.layout_generation = layout_generation_;
    track.templates.push_back(
        TrackTemplate{fingerprint.take_value(), frame_sequence, layout_generation_, EvidenceGrade::kConfirmed});
    track.position_history.push_back(
        TrackObservation{frame_sequence, region.bounds, std::clamp(region.confidence, 0.0F, 1.0F), layout_generation_});
    track.semantics.label = region.label;
    track.semantics.text = region.text;
    truncate_text(track.semantics.text);
    track.confidence = std::clamp(region.confidence, 0.0F, 1.0F);
    track.last_verified_sequence = frame_sequence;

    const int64_t inserted_bytes = track_bytes(track);
    tracks_.insert(std::upper_bound(tracks_.begin(), tracks_.end(), track.track_id,
                                    [](uint64_t id, const TargetTrack& existing) { return id < existing.track_id; }),
                   std::move(track));
    used_bytes_ += inserted_bytes;
    return adoption;
}

Result<void> ObjectTracker::terminate(uint64_t track_id, uint64_t frame_sequence) noexcept {
    TargetTrack* track = find_track_mutable(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "terminate: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "terminate: track already terminated"};
    }
    archive_track(*track, frame_sequence);
    return Status::success();
}

Result<void> ObjectTracker::record_observation(uint64_t track_id, const RectF& bounds, float confidence,
                                               uint64_t frame_sequence) noexcept {
    if (!all_finite(bounds) || bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return Status{ErrorCode::kInvalidArgument, "record_observation bounds must be finite and non-empty"};
    }
    TargetTrack* track = find_track_mutable(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "record_observation: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "record_observation: track already terminated"};
    }
    // One observation is a fixed-size element: the count-pressure drop of the
    // oldest entry always frees exactly the bytes the insertion needs, so the
    // byte check can only fail below the history bound (explicit error, pool
    // untouched — never silent growth).
    const bool at_capacity = track->position_history.size() >= static_cast<size_t>(options_.max_position_history);
    const int64_t freed = at_capacity ? kObservationOverheadBytes : 0;
    if (used_bytes_ - freed + kObservationOverheadBytes > options_.pool_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded, "record_observation does not fit the pool budget"};
    }
    if (at_capacity) {
        track->position_history.erase(track->position_history.begin());
        ++evicted_observations_;
        used_bytes_ -= freed;
    }
    track->position_history.push_back(
        TrackObservation{frame_sequence, bounds, std::clamp(confidence, 0.0F, 1.0F), layout_generation_});
    used_bytes_ += kObservationOverheadBytes;
    return Status::success();
}

Result<void> ObjectTracker::store_template(std::vector<TrackTemplate>& set, int32_t capacity, size_t pinned_entries,
                                           const TrackTemplate& entry, uint64_t& evicted_counter) noexcept {
    const auto side = static_cast<int64_t>(options_.template_thumb_side);
    const auto expected_bytes = static_cast<int64_t>(entry.fingerprint.thumbnail_gray.size());
    if (entry.fingerprint.thumb_width != options_.template_thumb_side ||
        entry.fingerprint.thumb_height != options_.template_thumb_side || expected_bytes != side * side) {
        return Status{ErrorCode::kInvalidArgument, "template fingerprint does not match template_thumb_side"};
    }
    const int64_t insertion_bytes = kTemplateOverheadBytes + expected_bytes;
    const bool at_capacity = set.size() >= static_cast<size_t>(capacity);
    // Index 0 of the appearance set is the pinned adoption template; eviction
    // under count pressure takes the oldest entry after the pinned prefix.
    if (at_capacity && set.size() <= pinned_entries) {
        return Status{ErrorCode::kBudgetExceeded, "template element budget exhausted with no evictable template"};
    }
    const int64_t freed = at_capacity ? kTemplateOverheadBytes +
                                            static_cast<int64_t>(set[pinned_entries].fingerprint.thumbnail_gray.size())
                                      : 0;
    if (used_bytes_ - freed + insertion_bytes > options_.pool_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded, "template does not fit the pool budget"};
    }
    if (at_capacity) {
        set.erase(set.begin() + static_cast<std::ptrdiff_t>(pinned_entries));
        ++evicted_counter;
        used_bytes_ -= freed;
    }
    set.push_back(entry);
    used_bytes_ += insertion_bytes;
    return Status::success();
}

Result<void> ObjectTracker::add_template(uint64_t track_id, const TrackTemplate& entry) noexcept {
    TargetTrack* track = find_track_mutable(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "add_template: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "add_template: track already terminated"};
    }
    return store_template(track->templates, options_.max_templates, 1, entry, evicted_templates_);
}

Result<void> ObjectTracker::add_negative_template(uint64_t track_id, const TrackTemplate& entry) noexcept {
    TargetTrack* track = find_track_mutable(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "add_negative_template: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "add_negative_template: track already terminated"};
    }
    if (options_.max_negative_templates == 0) {
        return Status{ErrorCode::kBudgetExceeded, "negative template capacity is 0"};
    }
    return store_template(track->negative_templates, options_.max_negative_templates, 0, entry,
                          evicted_negative_templates_);
}

Result<uint32_t> ObjectTracker::advance_layout_generation() noexcept {
    if (layout_generation_ == std::numeric_limits<uint32_t>::max()) {
        return Status{ErrorCode::kBudgetExceeded, "layout generation counter exhausted"};
    }
    ++layout_generation_;
    return layout_generation_;
}

std::vector<TrackObservation> ObjectTracker::observations_in_generation(uint64_t track_id,
                                                                        uint32_t generation) const noexcept {
    std::vector<TrackObservation> entries;
    const TargetTrack* track = find_track(track_id);
    if (track == nullptr) {
        return entries;
    }
    for (const TrackObservation& observation : track->position_history) {
        if (observation.layout_generation == generation) {
            entries.push_back(observation);
        }
    }
    return entries;
}

Result<ChangeGateTrace> ObjectTracker::evaluate_change_gate(const ChangeReport& report,
                                                            const ExecutionContext& context) const noexcept {
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "evaluate_change_gate cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "evaluate_change_gate deadline reached"};
    }
    if (!rois_valid(report.changed_regions)) {
        return Status{ErrorCode::kInvalidArgument, "evaluate_change_gate change ROI must be non-empty"};
    }

    ChangeGateTrace trace;
    trace.classification = report.classification;
    trace.tracks.reserve(tracks_.size());
    switch (report.classification) {
        case ChangeClassification::kNone:
            // Level 1: the frame is unchanged — reuse every track without any
            // per-track geometric work (the near-zero path).
            for (const TargetTrack& track : tracks_) {
                trace.tracks.push_back(pool_entry(track, ChangeGateDecision::kReuse));
            }
            break;
        case ChangeClassification::kPartial:
            // Levels 2/3: per-track ROI intersection against last_bounds (the
            // extrapolated position until M7-04/M7-07).
            if (const auto appended = append_partial_trace(trace, report.changed_regions, tracks_, context);
                !appended.ok()) {
                return appended.status();
            }
            break;
        case ChangeClassification::kGlobal:
            // Level 3 for every track: nothing short-circuits. The layout
            // generation advance stays with the M7-07 pipeline.
            for (const TargetTrack& track : tracks_) {
                trace.tracks.push_back(pool_entry(track, ChangeGateDecision::kVerify));
            }
            break;
        default:
            return Status{ErrorCode::kInvalidArgument, "evaluate_change_gate unknown change classification"};
    }
    return trace;
}

ObjectTracker::BaselineSlots::iterator ObjectTracker::find_baseline_slot(uint64_t track_id) noexcept {
    return std::lower_bound(
        structure_baselines_.begin(), structure_baselines_.end(), track_id,
        [](const std::pair<uint64_t, StructureBaselineSlot>& slot, uint64_t id) { return slot.first < id; });
}

ObjectTracker::BaselineSlots::const_iterator ObjectTracker::find_baseline_slot(uint64_t track_id) const noexcept {
    return std::lower_bound(
        structure_baselines_.cbegin(), structure_baselines_.cend(), track_id,
        [](const std::pair<uint64_t, StructureBaselineSlot>& slot, uint64_t id) { return slot.first < id; });
}

int64_t ObjectTracker::erase_structure_baseline(uint64_t track_id) noexcept {
    const auto slot = find_baseline_slot(track_id);
    if (slot == structure_baselines_.end() || slot->first != track_id) {
        return 0;
    }
    structure_baselines_.erase(slot);
    return kStructureBaselineOverheadBytes;
}

Result<void> ObjectTracker::record_structure_baseline(uint64_t track_id, const TrackStructureDescriptors& descriptors,
                                                      uint64_t frame_sequence) noexcept {
    if (!descriptors_valid(descriptors)) {
        return Status{ErrorCode::kInvalidArgument,
                      "record_structure_baseline descriptors must be finite and in [0, 1]"};
    }
    TargetTrack* track = find_track_mutable(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "record_structure_baseline: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "record_structure_baseline: track already terminated"};
    }
    const auto slot = find_baseline_slot(track_id);
    const bool exists = slot != structure_baselines_.end() && slot->first == track_id;
    // One fixed-size slot per track: the first recording adds the slot bytes,
    // re-recording is byte-neutral (overwrite, never growth).
    if (!exists && used_bytes_ + kStructureBaselineOverheadBytes > options_.pool_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded, "record_structure_baseline does not fit the pool budget"};
    }
    if (exists) {
        slot->second = StructureBaselineSlot{descriptors, frame_sequence, layout_generation_};
    } else {
        structure_baselines_.insert(slot,
                                    {track_id, StructureBaselineSlot{descriptors, frame_sequence, layout_generation_}});
        used_bytes_ += kStructureBaselineOverheadBytes;
    }
    return Status::success();
}

Result<TrackEvidenceCommit> ObjectTracker::commit_track_evidence(
    uint64_t track_id, const TrackVerification& verification, const TrackPositionEvidence& position,
    const std::optional<TrackSemantics>& candidate_semantics, const ImageView& presented_view, uint64_t frame_sequence,
    const ExecutionContext& context) noexcept {
    // Validation precedes cancellation (frozen M7-06 decision, see the
    // class contract); every error below leaves the pool untouched.
    TargetTrack* track = find_track_mutable(track_id);
    if (const Status valid = validate_commit_inputs(presented_view, track, verification); !valid.ok()) {
        return valid;
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "commit_track_evidence cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "commit_track_evidence deadline reached"};
    }

    const bool has_candidate = verification.appearance.outcome != AppearanceChannelOutcome::kNone;
    auto patch = extract_candidate_patch(*track, has_candidate, verification, presented_view, options_);
    if (!patch.ok()) {
        return patch.status();
    }

    // Evidence evaluation, transition and store planning (frozen; nothing
    // mutated yet).
    const auto slot = find_state_slot(track_id);
    const bool slot_exists = slot != state_slots_.end() && slot->first == track_id;
    const CommitPlan plan = plan_track_commit(*track, verification, position, candidate_semantics, patch.value(),
                                              slot_exists, slot_exists ? slot->second.insufficient_streak : 0,
                                              slot_exists ? slot->second.lost_sequence : 0, frame_sequence, options_);

    // Atomicity: the whole store plan is budget-checked together before any
    // mutation; full template sets swap byte-neutrally (uniform pinned
    // thumbnail size).
    const auto side = static_cast<int64_t>(options_.template_thumb_side);
    const int64_t template_bytes = kTemplateOverheadBytes + side * side;
    if (used_bytes_ + planned_store_delta(plan, template_bytes) > options_.pool_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded, "commit_track_evidence planned stores do not fit the pool budget"};
    }

    // Commit — no failure is possible past this point (the plan above is
    // exact for the fixed-size stores).
    TrackEvidenceCommit commit;
    commit.track_id = track_id;
    commit.previous_state = track->state;
    commit.state = plan.transition.state;
    commit.grade = plan.grade;
    commit.scenario = position.scenario;
    commit.impostor_hit = plan.impostor;
    commit.semantics_conflict = plan.conflict;
    commit.template_captured = plan.want_positive;
    commit.negative_template_captured = plan.want_negative;
    apply_commit_stores(*track, plan, verification, patch.value(), frame_sequence, layout_generation_, template_bytes,
                        evicted_templates_, evicted_negative_templates_, used_bytes_);
    if (plan.slot_allocation_needed) {
        state_slots_.insert(find_state_slot(track_id),
                            {track_id, StateSlot{plan.transition.insufficient_streak, plan.transition.lost_sequence}});
        used_bytes_ += kStateSlotOverheadBytes;
    } else if (slot_exists) {
        slot->second = StateSlot{plan.transition.insufficient_streak, plan.transition.lost_sequence};
    }
    track->state = plan.transition.state;
    track->layout_generation = std::max(track->layout_generation, layout_generation_);
    return commit;
}

Result<GenerationAdvance> ObjectTracker::advance_generation_for_classification(
    const ChangeClassification classification) noexcept {
    GenerationAdvance advance;
    advance.generation = layout_generation_;
    switch (classification) {
        case ChangeClassification::kNone:
        case ChangeClassification::kPartial:
            // The frozen trigger: only a global classification advances the
            // layout generation (design section 6.4).
            return advance;
        case ChangeClassification::kGlobal: {
            const auto advanced = advance_layout_generation();
            if (!advanced.ok()) {
                // uint32 exhaustion: explicit failure, generation unchanged.
                return advanced.status();
            }
            advance.advanced = true;
            advance.generation = advanced.value();
            return advance;
        }
        default:
            return Status{ErrorCode::kInvalidArgument,
                          "advance_generation_for_classification unknown change classification"};
    }
}

Result<MotionCompensationResult> ObjectTracker::compensate_global_motion(const ShiftEstimate& shift,
                                                                         const ExecutionContext& context) noexcept {
    // Entry-only cancellation poll (frozen: the whole-pool pass is bounded,
    // trivial per track and infallible after validation, so a mid-loop poll
    // could only abort a half-applied pool).
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "compensate_global_motion cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "compensate_global_motion deadline reached"};
    }
    if (!std::isfinite(shift.dx) || !std::isfinite(shift.dy)) {
        return Status{ErrorCode::kInvalidArgument, "compensate_global_motion: shift estimate must be finite"};
    }
    if (!std::isfinite(shift.confidence) || shift.confidence < 0.0F || shift.confidence > 1.0F) {
        return Status{ErrorCode::kInvalidArgument, "compensate_global_motion: shift confidence must be in [0, 1]"};
    }

    MotionCompensationResult result;
    result.dx = shift.dx;
    result.dy = shift.dy;
    if (static_cast<double>(shift.confidence) < options_.min_compensation_confidence) {
        // Explicit refusal (RULE-06): the evaluated estimate is echoed, the
        // pool stays untouched.
        return result;
    }
    result.applied = true;

    // Planned before any mutation: every non-terminated track's translated
    // bounds must stay finite, else the whole call fails and the pool is
    // untouched.
    for (const TargetTrack& track : tracks_) {
        if (track.state == TrackState::kTerminated) {
            continue;
        }
        if (!translation_stays_finite(track.last_bounds, shift.dx, shift.dy)) {
            return Status{ErrorCode::kInvalidArgument,
                          "compensate_global_motion: compensated bounds leave the finite float range"};
        }
    }
    result.tracks.reserve(tracks_.size());
    for (TargetTrack& track : tracks_) {
        if (track.state == TrackState::kTerminated) {
            continue;
        }
        const RectF previous = track.last_bounds;
        apply_compensation(track, shift.dx, shift.dy);
        result.tracks.push_back(MotionCompensationEntry{track.track_id, track.state, previous, track.last_bounds});
    }
    return result;
}

Result<std::vector<uint64_t>> ObjectTracker::sweep_generation_lag(uint64_t frame_sequence,
                                                                  const ExecutionContext& context) noexcept {
    // Entry-only cancellation poll (same rationale as
    // `compensate_global_motion`).
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "sweep_generation_lag cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "sweep_generation_lag deadline reached"};
    }

    // Plan first (pure): the tracks the frozen exhaustion rule condemns plus
    // the bookkeeping their transitions need — any failure below leaves the
    // pool untouched.
    struct SweepEntry {
        uint64_t track_id = 0;
        bool slot_exists = false;
    };
    std::vector<SweepEntry> plan;
    const auto lag_limit = static_cast<uint64_t>(options_.max_generation_lag);
    const auto pool_generation = static_cast<uint64_t>(layout_generation_);
    for (const TargetTrack& track : tracks_) {
        if (track.state != TrackState::kUncertain) {
            // Exhausted evidence: only a track the M7-06 state machine has
            // already judged insufficient is swept; kLost stays (sticky) and
            // kTracking keeps its confirmed status.
            continue;
        }
        // Pool generation >= track generation on every path (each commit
        // stamps the track with the pool's current generation), so the
        // unsigned subtraction cannot underflow.
        if (pool_generation - static_cast<uint64_t>(track.layout_generation) <= lag_limit) {
            continue;
        }
        plan.push_back({track.track_id, state_slot_bytes(track.track_id) != 0});
    }
    int64_t planned_slot_bytes = 0;
    for (const SweepEntry& entry : plan) {
        if (!entry.slot_exists) {
            planned_slot_bytes += kStateSlotOverheadBytes;
        }
    }
    if (used_bytes_ + planned_slot_bytes > options_.pool_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded,
                      "sweep_generation_lag state-slot allocation does not fit the pool budget"};
    }

    // Commit — no failure is possible past this point.
    std::vector<uint64_t> swept;
    swept.reserve(plan.size());
    for (const SweepEntry& entry : plan) {
        TargetTrack* track = find_track_mutable(entry.track_id);
        track->state = TrackState::kLost;
        const auto slot = find_state_slot(entry.track_id);
        if (slot != state_slots_.end() && slot->first == entry.track_id) {
            slot->second.lost_sequence = frame_sequence;
        } else {
            state_slots_.insert(slot, {entry.track_id, StateSlot{0, frame_sequence}});
            used_bytes_ += kStateSlotOverheadBytes;
        }
        swept.push_back(entry.track_id);
    }
    return swept;
}

Result<RedetectionGateDecision> ObjectTracker::evaluate_redetection_gate(
    uint64_t track_id, const ChangeClassification classification, const uint64_t frame_sequence,
    const ExecutionContext& context) const noexcept {
    // Entry-only cancellation poll (the `evaluate_change_gate` precedent —
    // an O(1) bounded read).
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "evaluate_redetection_gate cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "evaluate_redetection_gate deadline reached"};
    }
    switch (classification) {
        case ChangeClassification::kNone:
        case ChangeClassification::kPartial:
        case ChangeClassification::kGlobal:
            break;
        default:
            return Status{ErrorCode::kInvalidArgument, "evaluate_redetection_gate unknown change classification"};
    }
    const TargetTrack* track = find_track(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "evaluate_redetection_gate: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "evaluate_redetection_gate: track already terminated"};
    }

    RedetectionGateDecision decision;
    decision.track_id = track_id;
    decision.state = track->state;
    if (track->state != TrackState::kLost) {
        // Redetection is the kLost path; live tracks get an explicit
        // kInactive verdict, never a silent skip (the M7-03 pattern).
        decision.verdict = RedetectionGateVerdict::kInactive;
        return decision;
    }
    if (classification == ChangeClassification::kNone) {
        // Change-gate coupling: a static frame never triggers redetection
        // (design section 7; the zero-trigger negative-test anchor).
        decision.verdict = RedetectionGateVerdict::kHoldStaticFrame;
        return decision;
    }
    // Episode view: the slot describes the current loss episode only when its
    // key matches the track state slot's kLost entry sequence; a stale slot
    // (recaptured without recapture bookkeeping, then re-lost) reads as the
    // fresh episode it is. Every kLost track holds a state slot with its
    // entry sequence (frozen M7-06/M7-07 loss paths) — the fallback below is
    // defensive and deterministic.
    const auto state_slot = find_state_slot(track_id);
    const uint64_t episode_key =
        (state_slot != state_slots_.cend() && state_slot->first == track_id) ? state_slot->second.lost_sequence : 0;
    const auto slot = find_redetect_slot(track_id);
    const bool slot_valid =
        slot != redetect_slots_.cend() && slot->first == track_id && slot->second.episode_lost_sequence == episode_key;
    decision.attempts = slot_valid ? slot->second.consecutive_failures : 0;
    const uint64_t next_attempt = slot_valid ? slot->second.next_attempt_sequence : 0;
    if (frame_sequence < next_attempt) {
        decision.verdict = RedetectionGateVerdict::kHoldBackoff;
        decision.wait_frames = static_cast<uint32_t>(
            std::min<uint64_t>(next_attempt - frame_sequence, std::numeric_limits<uint32_t>::max()));
        return decision;
    }
    // Reachable only with attempts < redetect_max_attempts: the max-th
    // accounted failure terminates the track, so an exhausted budget is the
    // visible kTerminated archive, never a gate verdict.
    decision.verdict = RedetectionGateVerdict::kTrigger;
    return decision;
}

Result<RedetectionFailureRecord> ObjectTracker::record_redetection_failure(uint64_t track_id,
                                                                           const uint64_t frame_sequence,
                                                                           const ExecutionContext& context) noexcept {
    // Entry-only cancellation poll (the `compensate_global_motion`
    // precedent — an O(1) bounded entry, infallible after validation).
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "record_redetection_failure cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "record_redetection_failure deadline reached"};
    }
    TargetTrack* track = find_track_mutable(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_failure: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_failure: track already terminated"};
    }
    if (track->state != TrackState::kLost) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_failure requires a kLost track"};
    }

    // Episode view (see evaluate_redetection_gate): the slot counts for the
    // current loss episode only; a stale slot restarts the count.
    const auto state_slot = find_state_slot(track_id);
    const uint64_t episode_key =
        (state_slot != state_slots_.end() && state_slot->first == track_id) ? state_slot->second.lost_sequence : 0;
    const auto slot = find_redetect_slot(track_id);
    const bool slot_valid =
        slot != redetect_slots_.end() && slot->first == track_id && slot->second.episode_lost_sequence == episode_key;
    const uint32_t new_failures = (slot_valid ? slot->second.consecutive_failures : 0U) + 1U;

    RedetectionFailureRecord record;
    record.track_id = track_id;
    record.attempts = new_failures;

    // Budget exhaustion: THIS entry performs the kLost → kTerminated edge
    // with exactly `terminate`'s archive semantics — the explicit, visible
    // end state of design section 7, never a silent pool clear.
    if (new_failures >= static_cast<uint32_t>(options_.redetect_max_attempts)) {
        archive_track(*track, frame_sequence);
        record.state = TrackState::kTerminated;
        return record;
    }

    const int64_t wait = redetection_backoff_wait(new_failures, options_);
    const uint64_t next_attempt = saturating_sequence_add(frame_sequence, wait);
    const bool slot_exists = slot != redetect_slots_.end() && slot->first == track_id;
    if (!slot_exists) {
        // First accounted failure of the episode: allocate the slot with a
        // budget check (RULE-06); any failure leaves the pool untouched.
        if (used_bytes_ + kRedetectSlotOverheadBytes > options_.pool_budget_bytes) {
            return Status{ErrorCode::kBudgetExceeded,
                          "record_redetection_failure slot allocation does not fit the pool budget"};
        }
        redetect_slots_.insert(slot, {track_id, RedetectSlot{episode_key, next_attempt, new_failures}});
        used_bytes_ += kRedetectSlotOverheadBytes;
    } else {
        // Existing (valid or stale) slot: the fixed-size update is
        // byte-neutral.
        slot->second = RedetectSlot{episode_key, next_attempt, new_failures};
    }
    record.state = TrackState::kLost;
    record.backoff_frames = static_cast<int32_t>(wait);
    record.next_attempt_sequence = next_attempt;
    return record;
}

Result<TrackInterruptionEvent> ObjectTracker::record_redetection_recapture(uint64_t track_id,
                                                                           const uint64_t frame_sequence,
                                                                           const uint64_t lost_sequence,
                                                                           const ExecutionContext& context) noexcept {
    // Entry-only cancellation poll (as record_redetection_failure).
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "record_redetection_recapture cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "record_redetection_recapture deadline reached"};
    }
    TargetTrack* track = find_track_mutable(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_recapture: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_recapture: track already terminated"};
    }
    if (track->state != TrackState::kTracking) {
        // The confirming commit must have happened: the kTracking state IS
        // the recapture proof (frozen M7-06 edge — this entry records it,
        // it does not produce it).
        return Status{ErrorCode::kInvalidArgument,
                      "record_redetection_recapture requires a recaptured (kTracking) track"};
    }
    if (lost_sequence > frame_sequence) {
        return Status{ErrorCode::kInvalidArgument,
                      "record_redetection_recapture lost_sequence exceeds the recapture sequence"};
    }

    const auto slot = find_redetect_slot(track_id);
    const uint32_t attempts =
        (slot != redetect_slots_.end() && slot->first == track_id) ? slot->second.consecutive_failures : 0;

    // Plan the log append before any mutation (RULE-06): on error neither
    // the log nor the episode slot changes.
    if (const auto appended = append_redetection_record(RedetectionRecord{
            RedetectionRecordKind::kInterruption, frame_sequence, track_id, 0, lost_sequence, attempts});
        !appended.ok()) {
        return appended.status();
    }
    // Episode closed: release the slot (a later loss starts a fresh episode).
    used_bytes_ -= erase_redetect_slot(track_id);
    return TrackInterruptionEvent{track_id, lost_sequence, frame_sequence, attempts};
}

Result<RedetectionAssociation> ObjectTracker::record_redetection_association(const uint64_t predecessor_track_id,
                                                                             const uint64_t successor_track_id,
                                                                             const uint64_t frame_sequence,
                                                                             const ExecutionContext& context) noexcept {
    // Entry-only cancellation poll (as record_redetection_failure).
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "record_redetection_association cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "record_redetection_association deadline reached"};
    }
    if (predecessor_track_id == successor_track_id) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_association ids must differ"};
    }
    const TargetTrack* predecessor = find_track(predecessor_track_id);
    if (predecessor == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_association: unknown predecessor track id"};
    }
    if (predecessor->state != TrackState::kLost) {
        // The association documents a replaced open identity; a terminated
        // (identity closed) or live predecessor is not that.
        return Status{ErrorCode::kInvalidArgument, "record_redetection_association requires a kLost predecessor"};
    }
    const TargetTrack* successor = find_track(successor_track_id);
    if (successor == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_association: unknown successor track id"};
    }
    if (successor->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "record_redetection_association: successor already terminated"};
    }
    if (const auto appended = append_redetection_record(RedetectionRecord{
            RedetectionRecordKind::kAssociation, frame_sequence, predecessor_track_id, successor_track_id, 0, 0});
        !appended.ok()) {
        return appended.status();
    }
    return RedetectionAssociation{predecessor_track_id, successor_track_id, frame_sequence};
}

Result<void> ObjectTracker::append_redetection_record(const RedetectionRecord& record) noexcept {
    // One fixed-size element: the count-pressure drop of the oldest entry
    // always frees exactly the bytes the insertion needs (the
    // record_observation rule), so the byte check can only fail below the
    // history bound (explicit error, log untouched — never silent growth).
    const bool at_capacity = redetection_records_.size() >= static_cast<size_t>(options_.max_redetection_records);
    const int64_t freed = at_capacity ? kRedetectionRecordOverheadBytes : 0;
    if (used_bytes_ - freed + kRedetectionRecordOverheadBytes > options_.pool_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded, "redetection record does not fit the pool budget"};
    }
    if (at_capacity) {
        redetection_records_.erase(redetection_records_.begin());
        ++evicted_redetection_records_;
        used_bytes_ -= freed;
    }
    redetection_records_.push_back(record);
    used_bytes_ += kRedetectionRecordOverheadBytes;
    return Status::success();
}

std::vector<RedetectionRecord> ObjectTracker::redetection_records() const noexcept {
    return redetection_records_;
}

Result<RectI> ObjectTracker::verification_roi(uint64_t track_id, const ImageView& presented_view) const noexcept {
    if (auto validated = validate(presented_view); !validated.ok()) {
        return validated.status();
    }
    const TargetTrack* track = find_track(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "verification_roi: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "verification_roi: track already terminated"};
    }
    return clamped_verification_roi(track->last_bounds, track->predicted_center,
                                    options_.verification_roi_diagonal_ratio, presented_view);
}

Result<TrackVerification> ObjectTracker::verify_track(
    uint64_t track_id, const ImageView& presented_view,
    const std::optional<TrackStructureDescriptors>& structure_descriptors,
    const ExecutionContext& context) const noexcept {
    // Validation precedes cancellation (frozen M7-05 decision — the
    // deliberate contrast to adopt_track, whose M7-02 entry checks
    // cancellation first): every error below leaves the pool untouched —
    // this method is const, so it cannot mutate it in the first place.
    if (auto validated = validate(presented_view); !validated.ok()) {
        return validated.status();
    }
    const TargetTrack* track = find_track(track_id);
    if (track == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "verify_track: unknown track id"};
    }
    if (track->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "verify_track: track already terminated"};
    }
    if (track->templates.empty()) {
        return Status{ErrorCode::kInvalidArgument, "verify_track: track has no appearance templates"};
    }
    if (!all_finite(track->last_bounds) || track->last_bounds.width <= 0.0F || track->last_bounds.height <= 0.0F) {
        return Status{ErrorCode::kInvalidArgument, "verify_track: track bounds must be finite and non-empty"};
    }
    if (structure_descriptors.has_value() && !descriptors_valid(*structure_descriptors)) {
        return Status{ErrorCode::kInvalidArgument, "verify_track: structure descriptors must be finite and in [0, 1]"};
    }
    const auto roi_result = clamped_verification_roi(track->last_bounds, track->predicted_center,
                                                     options_.verification_roi_diagonal_ratio, presented_view);
    if (!roi_result.ok()) {
        return roi_result.status();
    }
    const RectI roi = roi_result.value();
    const OffsetGrid grid = verification_offset_grid(track->last_bounds, roi);
    if (const int64_t planned = planned_scan_work(grid, track->templates.size(), track->last_bounds,
                                                  bytes_per_pixel(presented_view.format), options_);
        planned > options_.verification_work_budget_bytes) {
        return Status{ErrorCode::kBudgetExceeded, "verify_track planned search work exceeds the work budget"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "verify_track cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "verify_track deadline reached"};
    }

    const auto templates_count = static_cast<size_t>(track->templates.size());
    std::vector<std::vector<double>> responses(templates_count,
                                               std::vector<double>(static_cast<size_t>(offset_count(grid)), 0.0));
    if (const auto scanned =
            scan_appearance_responses(*track, presented_view, grid, options_.verification_work_budget_bytes,
                                      options_.template_thumb_side, context, responses);
        !scanned.ok()) {
        return scanned.status();
    }

    std::vector<SurfacePeak> peaks(templates_count);
    for (size_t t = 0; t < templates_count; ++t) {
        peaks[t] = surface_peak(responses[t], grid);
    }

    const auto slot = find_baseline_slot(track_id);
    const bool has_slot = slot != structure_baselines_.cend() && slot->first == track_id;
    const TrackStructureDescriptors* baseline = has_slot ? &slot->second.descriptors : nullptr;

    TrackVerification verification;
    verification.track_id = track_id;
    verification.state = track->state;
    verification.verification_roi = roi;
    verification.appearance = appearance_verdict(peaks, grid, options_);
    verification.structure = structure_verdict(structure_descriptors, baseline, options_);
    return verification;
}

void ObjectTracker::reset() noexcept {
    tracks_.clear();
    structure_baselines_.clear();
    state_slots_.clear();
    redetect_slots_.clear();
    redetection_records_.clear();
    layout_generation_ = 0;
    used_bytes_ = 0;
    evicted_count_ = 0;
    evicted_observations_ = 0;
    evicted_templates_ = 0;
    evicted_negative_templates_ = 0;
    evicted_redetection_records_ = 0;
}

std::vector<uint64_t> ObjectTracker::track_ids() const noexcept {
    std::vector<uint64_t> ids;
    ids.reserve(tracks_.size());
    for (const TargetTrack& track : tracks_) {
        ids.push_back(track.track_id);
    }
    return ids;
}

const TargetTrack* ObjectTracker::find_track(uint64_t track_id) const noexcept {
    const auto it = std::lower_bound(tracks_.begin(), tracks_.end(), track_id,
                                     [](const TargetTrack& track, uint64_t id) { return track.track_id < id; });
    if (it == tracks_.end() || it->track_id != track_id) {
        return nullptr;
    }
    return &*it;
}

TargetTrack* ObjectTracker::find_track_mutable(uint64_t track_id) noexcept {
    const auto it = std::lower_bound(tracks_.begin(), tracks_.end(), track_id,
                                     [](const TargetTrack& track, uint64_t id) { return track.track_id < id; });
    if (it == tracks_.end() || it->track_id != track_id) {
        return nullptr;
    }
    return &*it;
}

}  // namespace mirador
