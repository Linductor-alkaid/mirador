#ifndef MIRADOR_STABLE_ID_TRACKER_HPP
#define MIRADOR_STABLE_ID_TRACKER_HPP

#include <mirador/execution_context.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mirador {

/// Matching outcome of one current region (design section 16, DEC-010):
/// retained regions keep their identity, fresh ids are allocated for the rest
/// and split/merge evidence upgrades the event (and bumps the snapshot
/// generation).
enum class IdEvent : uint8_t {
    kRetained,    ///< matched a previous region and keeps its stable_id
    kNew,         ///< no match; allocated a fresh id
    kSplitChild,  ///< fresh id; >= 2 fresh regions fall inside one unmatched previous region
    kMerged,      ///< fresh id; >= 2 unmatched previous regions fall inside this region
};

/// Per-current-region assignment, parallel to the fused region list.
struct IdAssignment {
    uint64_t stable_id = 0;
    IdEvent event = IdEvent::kNew;
};

/// One caller-declared, tracking-confirmed association for `advance` (M7-06
/// DEC-010 gate passthrough; object-tracking design section 6.5): the fused
/// region at `region_index` is the cross-frame tracker's confirmed match of
/// the tracked region carrying `stable_id` (an `ObjectTracker` track in
/// kTracking state after a confirming `commit_track_evidence`), so the
/// pairing bypasses the IoU/center gate and the cost ranking — the session's
/// time-consistency signal stands in for the static gate, with cost set to
/// optimal. The extension rides the threshold-configuration evolution
/// channel reserved by DEC-010 section 4; the frozen static matching
/// semantics are unchanged (an empty association list reproduces the
/// previous behavior bit for bit).
struct ConfirmedAssociation {
    size_t region_index = 0;  ///< index into the current region list
    uint64_t stable_id = 0;   ///< tracked id the region is confirmed against
};

/// Outcome of one `StableIdTracker::advance` (DEC-010): per-region
/// assignments plus the event tallies and the generation decision.
struct StableIdReport {
    std::vector<IdAssignment> assignments;
    size_t retained_count = 0;
    size_t new_count = 0;
    size_t split_count = 0;  ///< previous regions whose identity split
    size_t merge_count = 0;  ///< current regions that swallowed >= 2 unmatched previous ones
    /// True when the producing session must increment the snapshot generation:
    /// any split/merge, or the retained fraction fell below
    /// `StableIdOptions::generation_retention_ratio` (DEC-010 section 3).
    bool generation_bump = false;
};

/// Tunables of gated greedy stable-id matching (DEC-010). All thresholds are
/// inclusive; weights need not sum to one (the cost normalizes over active
/// signals).
struct StableIdOptions {
    /// Gate: pairs with IoU >= this value are match candidates.
    double match_iou_threshold = 0.3;
    /// Gate: pairs whose center displacement <= ratio * previous diagonal are
    /// candidates.
    double center_gate_ratio = 0.5;
    /// Cost signal weights: cost = sum(w_k * (1 - s_k)) / sum(w_k) over the
    /// active signals (IoU, center proximity, text similarity).
    double weight_iou = 0.5;
    double weight_center = 0.3;
    double weight_text = 0.2;
    /// Retained fraction below which the generation increments (DEC-010).
    double generation_retention_ratio = 0.5;
    /// Output budget (RULE-06): more current regions fail with
    /// kBudgetExceeded.
    int32_t max_regions = 4096;
};

/// Cross-snapshot stable identity tracker (design section 16, RULE-09):
/// matches the previous tracked regions against the current fused regions
/// with gated greedy one-to-one assignment. Ids are unique within one tracker
/// instance only — a tracker lives inside a `PerceptionSession` and is never
/// reused across sessions. Not thread-safe, never throws (AGENTS.md).
class StableIdTracker {
public:
    /// Texts longer than this are truncated before the similarity cost
    /// (bounded work, RULE-06).
    static constexpr size_t kMaxTextBytes = 1024;

    StableIdTracker() noexcept = default;
    StableIdTracker(const StableIdTracker&) = delete;
    StableIdTracker& operator=(const StableIdTracker&) = delete;
    StableIdTracker(StableIdTracker&&) noexcept = default;
    StableIdTracker& operator=(StableIdTracker&&) noexcept = default;
    ~StableIdTracker() noexcept = default;

    /// Advances one generation of tracking over `current_regions` (fused
    /// regions in a common coordinate space; coordinates only travel inside
    /// one tracker, so mixing spaces across calls is the caller's error).
    /// On success the tracker's previous-state becomes the assigned current
    /// regions; on error the state is untouched.
    ///
    /// `confirmed_associations` (M7-06, DEC-010 gate passthrough; design
    /// section 6.5): tracking-confirmed pairings applied before the gated
    /// greedy pass. Each named region is pre-matched — kRetained, zero cost
    /// — to the tracked region carrying its `stable_id` when that id is
    /// still tracked and neither side is already paired; every other region
    /// matches exactly as before. An association naming an untracked id is
    /// ignored (the track may have been terminated between frames; the
    /// region falls through to the normal gate). Pre-matched regions count
    /// as retained for the tallies and the generation decision.
    ///
    /// Errors: kInvalidArgument (invalid options, a `region_index` outside
    /// `current_regions`, a zero `stable_id`, or duplicate region indexes
    /// or stable ids across associations), kCancelled/kTimeout from
    /// `context`, kBudgetExceeded (more regions than `max_regions` or
    /// allocation failure). Never throws.
    [[nodiscard]] Result<StableIdReport> advance(
        std::span<const VisualRegion> current_regions, const StableIdOptions& options = {},
        const ExecutionContext& context = {},
        std::span<const ConfirmedAssociation> confirmed_associations = {}) noexcept;

    /// Drops all tracked state; the next advance starts from scratch with a
    /// fresh id space.
    void reset() noexcept;

    /// Number of currently tracked (previous) regions.
    [[nodiscard]] size_t tracked_count() const noexcept { return previous_.size(); }
    /// Largest stable id handed out so far (0 before the first advance).
    [[nodiscard]] uint64_t last_id() const noexcept { return next_id_ - 1; }

private:
    struct TrackedRegion {
        uint64_t stable_id = 0;
        RectF bounds;
        std::string text;  ///< normalized snapshot of the region text
    };

    std::vector<TrackedRegion> previous_;
    uint64_t next_id_ = 1;
};

}  // namespace mirador

#endif  // MIRADOR_STABLE_ID_TRACKER_HPP
