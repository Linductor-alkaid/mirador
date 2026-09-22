#include <mirador/object_tracker.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/crop.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/patch_fingerprint.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>

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
        options.verification_roi_diagonal_ratio > 8.0) {
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
    std::vector<uint64_t> victim_ids;
    int64_t freed_bytes = 0;
    const auto already_victim = [&victim_ids](uint64_t id) {
        return std::find(victim_ids.begin(), victim_ids.end(), id) != victim_ids.end();
    };
    const auto max_count = static_cast<size_t>(options_.max_targets);
    while (tracks_.size() - victim_ids.size() >= max_count ||
           used_bytes_ - freed_bytes + insertion_bytes > options_.pool_budget_bytes) {
        const TargetTrack* victim = nullptr;
        for (const TargetTrack& candidate : tracks_) {
            if (already_victim(candidate.track_id)) {
                continue;
            }
            if (victim == nullptr || evicts_before(candidate, *victim)) {
                victim = &candidate;
            }
        }
        victim_ids.push_back(victim->track_id);
        freed_bytes += track_bytes(*victim);
    }

    // Capture the adoption template before any eviction commits: the capture
    // budget is the pool headroom the planned eviction frees up, so an
    // exact-fit pool can still evict. Any capture error returns before the
    // pool is mutated ("failure leaves the pool untouched").
    const int64_t capture_budget = options_.pool_budget_bytes - used_bytes_ + freed_bytes;
    auto cropped = crop(presented_view, *roi, capture_budget);
    if (!cropped.ok()) {
        return cropped.status();
    }
    auto fingerprint = make_visual_patch_fingerprint(
        cropped.value().view(), PatchFingerprintParams{options_.template_thumb_side}, capture_budget);
    if (!fingerprint.ok()) {
        return fingerprint.status();
    }

    // Commit: evict the planned victims, then insert the new track.
    TrackAdoption adoption;
    adoption.track_id = region.stable_id;
    adoption.evicted_track_ids = std::move(victim_ids);
    for (const uint64_t victim_id : adoption.evicted_track_ids) {
        const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                     [victim_id](const TargetTrack& track) { return track.track_id == victim_id; });
        tracks_.erase(it);
        ++evicted_count_;
    }
    used_bytes_ -= freed_bytes;

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

void ObjectTracker::reset() noexcept {
    tracks_.clear();
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
