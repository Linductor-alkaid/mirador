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
        plan.freed_bytes += track_bytes(*victim) + baseline_slot_bytes(victim->track_id);
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
    if (options.redetect_backoff_base_frames < 1 ||
        options.redetect_backoff_max_frames < options.redetect_backoff_base_frames ||
        options.redetect_max_attempts < 1) {
        return Status{ErrorCode::kInvalidArgument, "redetection backoff parameters invalid"};
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
    // A victim's pool-side E2 baseline slot (M7-05) is freed with it.
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

    // Commit: evict the planned victims (with their baseline slots), then
    // insert the new track.
    TrackAdoption adoption;
    adoption.track_id = region.stable_id;
    adoption.evicted_track_ids = plan.victim_ids;
    for (const uint64_t victim_id : adoption.evicted_track_ids) {
        erase_structure_baseline(victim_id);
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
    const int64_t before = track_bytes(*track);
    track->state = TrackState::kTerminated;
    track->terminated_sequence = frame_sequence;
    track->templates.clear();
    track->negative_templates.clear();
    track->position_history.clear();
    track->templates.shrink_to_fit();
    track->negative_templates.shrink_to_fit();
    track->position_history.shrink_to_fit();
    used_bytes_ -= before - track_bytes(*track);
    // The pool-side E2 baseline slot is released with the track's evidence
    // data (archives stay cheap, M7-01 freeze semantics).
    used_bytes_ -= erase_structure_baseline(track_id);
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
    layout_generation_ = 0;
    used_bytes_ = 0;
    evicted_count_ = 0;
    evicted_observations_ = 0;
    evicted_templates_ = 0;
    evicted_negative_templates_ = 0;
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
