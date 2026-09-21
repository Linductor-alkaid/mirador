#include <mirador/object_tracker.hpp>

#include <mirador/crop.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/patch_fingerprint.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    const auto it = std::lower_bound(tracks_.begin(), tracks_.end(), track_id,
                                     [](const TargetTrack& track, uint64_t id) { return track.track_id < id; });
    if (it == tracks_.end() || it->track_id != track_id) {
        return Status{ErrorCode::kInvalidArgument, "terminate: unknown track id"};
    }
    if (it->state == TrackState::kTerminated) {
        return Status{ErrorCode::kInvalidArgument, "terminate: track already terminated"};
    }
    const int64_t before = track_bytes(*it);
    it->state = TrackState::kTerminated;
    it->terminated_sequence = frame_sequence;
    it->templates.clear();
    it->negative_templates.clear();
    it->position_history.clear();
    it->templates.shrink_to_fit();
    it->negative_templates.shrink_to_fit();
    it->position_history.shrink_to_fit();
    used_bytes_ -= before - track_bytes(*it);
    return Status::success();
}

void ObjectTracker::reset() noexcept {
    tracks_.clear();
    layout_generation_ = 0;
    used_bytes_ = 0;
    evicted_count_ = 0;
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

}  // namespace mirador
