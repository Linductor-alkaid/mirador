#ifndef MIRADOR_OBJECT_TRACKER_HPP
#define MIRADOR_OBJECT_TRACKER_HPP

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/visual_fingerprint.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mirador {

/// Experimental (M7, DEC-019/DEC-020): fields and signatures may change within
/// M7 until the go/no-go freeze (the DEC-017/DEC-018 promotion pattern); the
/// contract is not covered by the compatibility promise until then. Tracks the
/// identity of visual objects across frames of one image source: a bounded
/// target pool plus a four-state lifecycle per track. The frame-step pipeline
/// (change-gated short-circuiting, neighborhood verification, motion
/// compensation, cascade redetection) extends this class in the pipeline work
/// items under the same Experimental header, the way `PerceptionSession`
/// gained `fuse()` in M4.
///
/// Coordinates: one tracker instance belongs to exactly one coordinate space —
/// all regions and views passed to it must be presented in that same space
/// (for session pipelines: the oriented view space). Mixing spaces across calls
/// is the caller's error, as in `StableIdTracker`. State is deterministic with
/// no wall-clock dependence; the tracker never throws, never creates threads
/// and never touches the network or the filesystem (RULE-03, RULE-10).

/// Lifecycle state of one track (object-tracking design section 4).
enum class TrackState : uint8_t {
    kTracking,    ///< confirmed tracking; the near-zero reuse paths apply
    kUncertain,   ///< position estimate only; placeholder output, low confidence
    kLost,        ///< target considered gone; redetection is the caller's call (RULE-12)
    kTerminated,  ///< retry budget exhausted or caller-terminated; identity closed
};

/// Strength of the evidence that justified the latest per-frame decision for a
/// track (object-tracking design section 3). Grades gate state transitions,
/// never identity claims by themselves.
enum class EvidenceGrade : uint8_t {
    kConfirmed,    ///< strong: appearance + position gate + semantics agree
    kTentative,    ///< medium: weak appearance match inside the position gate
    kPlaceholder,  ///< weak: position prior only; no identity claim is made
    kVetoed,       ///< rejected candidate: semantic conflict or impostor evidence
};

/// One bounded position-history entry of a track.
struct TrackObservation {
    uint64_t frame_sequence = 0;
    RectF bounds;  ///< observed bounds in the tracker's coordinate space
    float confidence = 0.0F;
    uint32_t layout_generation = 0;  ///< global layout generation at observation
};

/// One appearance template of a track: the normalized gray patch fingerprint
/// (M3-09 contract) plus capture provenance. Negative templates use the same
/// record; their subject is the impostor the track must reject.
struct TrackTemplate {
    VisualPatchFingerprint fingerprint;
    uint64_t frame_sequence = 0;  ///< frame the patch was captured from
    uint32_t layout_generation = 0;
    /// Evidence grade that justified the capture. High-confidence template
    /// updates only ever capture kConfirmed patches once verification exists.
    EvidenceGrade capture_grade = EvidenceGrade::kConfirmed;
};

/// Semantics snapshot of a track at adoption or later confirmation: category
/// and text travel with the track so semantic-compatibility gating (DEC-010
/// gate predicate) can veto impostor candidates without re-reading evidence.
struct TrackSemantics {
    std::string label;  ///< detector label or external role snapshot
    std::string text;   ///< concatenated text snapshot, truncated to
                        ///< `ObjectTracker::kMaxSemanticsTextBytes`
};

/// Public view of one track in the pool (object-tracking design section 5).
/// The pool returns references into its storage; they stay valid until the
/// next non-const call on the owning tracker.
struct TargetTrack {
    /// Identity, extending the fused stable_id of the adopting region (RULE-09:
    /// unique within this tracker/session only, never across sessions).
    uint64_t track_id = 0;
    TrackState state = TrackState::kTracking;
    /// Last confirmed or adopted bounds in the tracker's coordinate space.
    RectF last_bounds;
    /// Motion-model extrapolation of the center. Until the motion primitives
    /// exist (M7-04/M7-07) this is exactly the center of `last_bounds`.
    PointF predicted_center;
    /// Bounded observation history, oldest first, explicit eviction on overflow.
    std::vector<TrackObservation> position_history;
    /// Highest layout generation this track has lived through.
    uint32_t layout_generation = 0;
    /// Bounded appearance templates; index 0 is the adoption template.
    std::vector<TrackTemplate> templates;
    /// Bounded impostor templates confirmed against this track (populated by
    /// the verification pipeline from M7-05/M7-06 on).
    std::vector<TrackTemplate> negative_templates;
    TrackSemantics semantics;
    float confidence = 0.0F;
    /// Sequence of the latest evidence confirmation; adoption counts as one.
    uint64_t last_verified_sequence = 0;
    /// Termination sequence; 0 while the track is not kTerminated.
    uint64_t terminated_sequence = 0;
};

/// Tunables and frozen initial defaults of the target pool and the tracking
/// state machine (M7-01 contract freeze; threshold initial values are
/// development defaults and are calibrated against the M7-09 harness per
/// DEC-019 section 5 — any change afterwards is recorded there). Invalid
/// values fail `ObjectTracker::create` with kInvalidArgument.
struct ObjectTrackerOptions {
    // ---- Pool bounds (RULE-06: every limit explicit, overflow is explicit
    // eviction, never silent growth). ----
    /// Maximum number of live or archived tracks. [1, 4096].
    int32_t max_targets = 64;
    /// Maximum stored observations per track; overflow drops the oldest.
    /// [1, 1024].
    int32_t max_position_history = 32;
    /// Maximum appearance templates per track; overflow drops the oldest
    /// non-initial template. [1, 16].
    int32_t max_templates = 4;
    /// Maximum impostor (negative) templates per track. [0, 16].
    int32_t max_negative_templates = 4;
    /// Square thumbnail side of captured templates in pixels. [8, 64]
    /// (`PatchFingerprintParams` range).
    int32_t template_thumb_side = 32;
    /// Byte budget of the whole pool including per-track bookkeeping
    /// (documented accounting in `ObjectTracker::byte_size`). > 0.
    int64_t pool_budget_bytes = int64_t{1} * 1024 * 1024;

    // ---- State-machine timing (object-tracking design section 4). ----
    /// Frames a track may stay kUncertain before it degrades to kLost. [1, 4096].
    int32_t uncertain_frame_limit = 5;
    /// How many layout generations a track may lag before exhaustion rules make
    /// it kLost. [1, 1024].
    int32_t max_generation_lag = 1;

    // ---- Neighborhood verification thresholds (design section 6.2; initial
    // values, calibrated by M7-09). ----
    /// NCC peak at or above this value counts as strong appearance evidence.
    /// [0, 1], >= `ncc_weak_threshold`.
    double ncc_strong_threshold = 0.8;
    /// NCC peak at or above this value (below the strong threshold) counts as
    /// weak appearance evidence. [0, 1].
    double ncc_weak_threshold = 0.6;
    /// Minimum peak-to-sidelobe quality of the NCC response surface for the
    /// peak to be trusted (PSR-style flatness rejection). >= 1.
    double peak_sidelobe_ratio_min = 5.0;
    /// Maximum relative deviation of the closure-structure description
    /// quantities from the track's pooled baseline for the E2 channel to count
    /// as consistent. [0, 1].
    double structure_deviation_tolerance = 0.2;
    /// Verification ROI expansion around the predicted position, in multiples
    /// of the track's bounds diagonal. (0, 8].
    double verification_roi_diagonal_ratio = 1.0;

    // ---- Cascade redetection primitives (design section 7): the upper layer
    // drives every call (RULE-12); these values only parameterize the backoff
    // and budget the tracker reports on. ----
    /// First backoff wait, in frames, after a failed redetection attempt. >= 1.
    int32_t redetect_backoff_base_frames = 1;
    /// Backoff wait ceiling in frames; the sequence doubles from the base up to
    /// this value. >= `redetect_backoff_base_frames`.
    int32_t redetect_backoff_max_frames = 60;
    /// Maximum redetection attempts before a kLost track becomes kTerminated.
    /// >= 1.
    int32_t redetect_max_attempts = 8;
};

/// Outcome of one pool adoption: the new track id plus the ids of tracks
/// explicitly evicted to make room (ascending; RULE-06 visibility).
struct TrackAdoption {
    uint64_t track_id = 0;
    std::vector<uint64_t> evicted_track_ids;
};

/// Bounded cross-frame target pool over fused regions (object-tracking design
/// section 5, SCOPE-13). The pool is the structural core of the M7 tracker:
/// adoption captures the initial appearance template from the frame, every
/// resource is byte-budgeted with explicit eviction, and termination keeps the
/// failure visible. Not thread-safe, move-only, never throws (AGENTS.md).
class ObjectTracker {
public:
    /// Text longer than this is truncated before storage (bounded work, RULE-06).
    static constexpr size_t kMaxSemanticsTextBytes = 1024;
    /// Byte overhead of one stored template beyond its thumbnail bytes.
    static constexpr int64_t kTemplateOverheadBytes = 64;
    /// Byte overhead of one stored position observation.
    static constexpr int64_t kObservationOverheadBytes = 32;
    /// Fixed byte overhead of one track (identity, state, semantics bookkeeping).
    static constexpr int64_t kTrackOverheadBytes = 128;

    ObjectTracker() noexcept = default;
    ObjectTracker(const ObjectTracker&) = delete;
    ObjectTracker& operator=(const ObjectTracker&) = delete;
    ObjectTracker(ObjectTracker&&) noexcept = default;
    ObjectTracker& operator=(ObjectTracker&&) noexcept = default;
    ~ObjectTracker() noexcept = default;

    /// Creates a tracker with the given options. Errors: kInvalidArgument for
    /// any option outside its documented range (including
    /// `ncc_weak_threshold` > `ncc_strong_threshold` or a backoff maximum below
    /// the base). Never throws.
    [[nodiscard]] static Result<ObjectTracker> create(const ObjectTrackerOptions& options = {}) noexcept;

    /// Adopts one fused region as a new track: `region.stable_id` becomes the
    /// track id (RULE-09), the region bounds/confidence/semantics initialize
    /// the track, and the initial appearance template is captured from
    /// `presented_view` by cropping the region bounds (floor on the leading
    /// edge, ceil on the trailing edge, clamped to the view) and running the
    /// M3-09 patch fingerprint with `options.template_thumb_side`. The initial
    /// observation is recorded with `frame_sequence`.
    ///
    /// Resource rules: when the pool already holds `max_targets` tracks or the
    /// insertion would exceed `pool_budget_bytes`, eviction runs in a fixed
    /// order — terminated tracks first (oldest termination first), then live
    /// tracks oldest-first by (`last_verified_sequence`, `track_id`) — until
    /// the insertion fits; every evicted id is reported in the result. The
    /// eviction is planned first, the initial template is captured against the
    /// post-eviction headroom, and only a successful capture commits the
    /// eviction: any failure leaves the pool untouched. A track that could
    /// never fit fails with kBudgetExceeded. Eviction of live tracks is
    /// explicit and reported, never silent.
    ///
    /// Errors: kInvalidArgument (invalid view, zero stable_id, non-finite or
    /// empty bounds, bounds outside the presented view, or an id that is
    /// already tracked — including terminated ones), kBudgetExceeded as
    /// above or when the template capture cannot fit the remaining budget,
    /// kUnsupportedFormat when the view format has no gray conversion,
    /// kCancelled/kTimeout from `context`. On error the pool is untouched.
    /// Never throws.
    [[nodiscard]] Result<TrackAdoption> adopt_track(const VisualRegion& region, const ImageView& presented_view,
                                                    uint64_t frame_sequence,
                                                    const ExecutionContext& context = {}) noexcept;

    /// Terminates a track at the caller's request (upper-layer stop or budget
    /// policy): the track becomes kTerminated at `frame_sequence` and its
    /// templates, negative templates and position history are released so
    /// archives stay cheap; the identity record (bounds, semantics, state)
    /// stays visible ("failure is visible", design section 4). Errors:
    /// kInvalidArgument for an unknown or already-terminated id. Never throws.
    [[nodiscard]] Result<void> terminate(uint64_t track_id, uint64_t frame_sequence) noexcept;
    [[nodiscard]] Result<void> terminate(uint64_t track_id) noexcept;

    /// Appends one position observation to the track's bounded history (M7-02
    /// pool structure): the entry is stamped with this pool's current layout
    /// generation, and when the history already holds `max_position_history`
    /// entries the oldest one is evicted first (explicit, counted in
    /// `evicted_observation_count()`). Bookkeeping only: the call does not
    /// touch `state`, `last_bounds`, `predicted_center`, `confidence` or
    /// `last_verified_sequence` — evidence-grade confirmation updates arrive
    /// with the M7-06 state machine and decide when a history entry may count
    /// as a verification.
    ///
    /// Errors: kInvalidArgument for an unknown or already-terminated track or
    /// non-finite / non-positive bounds; kBudgetExceeded when the pool byte
    /// budget cannot hold one more observation. On error the pool is
    /// untouched. `confidence` is clamped to [0, 1]. Never throws.
    [[nodiscard]] Result<void> record_observation(uint64_t track_id, const RectF& bounds, float confidence,
                                                  uint64_t frame_sequence) noexcept;

    /// Stores one appearance template in the track's bounded template set
    /// (M7-02): entries are kept in insertion order after the pinned index 0
    /// (the adoption template), and when the set already holds `max_templates`
    /// entries the oldest non-initial template is evicted first (explicit,
    /// counted in `evicted_template_count()`). The adoption template itself is
    /// never evicted here; with `max_templates == 1` the call therefore
    /// exhausts its element budget and fails. The caller builds the fingerprint
    /// (for example with `make_visual_patch_fingerprint`, as `adopt_track`
    /// does); the pool validates and stores it.
    ///
    /// Errors: kInvalidArgument for an unknown or already-terminated track or
    /// a fingerprint whose `thumb_width`/`thumb_height`/byte size do not match
    /// `options().template_thumb_side`; kBudgetExceeded when the element
    /// capacity is exhausted with no evictable template, or when the pool byte
    /// budget cannot hold the insertion. On error the pool is untouched.
    /// Never throws.
    [[nodiscard]] Result<void> add_template(uint64_t track_id, const TrackTemplate& entry) noexcept;

    /// Stores one impostor (negative) template in the track's bounded set
    /// (M7-02): entries are kept in insertion order and when the set already
    /// holds `max_negative_templates` entries the oldest one is evicted first
    /// (explicit, counted in `evicted_negative_template_count()`). Negative
    /// templates feed the impostor veto of the verification pipeline (M7-05/
    /// M7-06); the pool only stores them.
    ///
    /// Errors: kInvalidArgument for an unknown or already-terminated track or
    /// a fingerprint size mismatch as in `add_template`; kBudgetExceeded when
    /// `max_negative_templates` is 0 or the element capacity is exhausted, or
    /// when the pool byte budget cannot hold the insertion. On error the pool
    /// is untouched. Never throws.
    [[nodiscard]] Result<void> add_negative_template(uint64_t track_id, const TrackTemplate& entry) noexcept;

    /// Advances this pool's layout generation by one and returns the new
    /// generation (M7-02 primitive). Later observations are stamped with it;
    /// existing history entries keep the generation they were recorded under,
    /// which is what makes `observations_in_generation` grouping meaningful.
    /// The trigger decision (global change classification) belongs to the
    /// M7-07 pipeline, and per-track degradation on a generation switch to the
    /// M7-06 state machine — this call only moves the deterministic counter.
    /// Errors: kBudgetExceeded when the uint32 counter is exhausted. Never
    /// throws.
    [[nodiscard]] Result<uint32_t> advance_layout_generation() noexcept;

    /// Position observations of one track recorded under `generation`, oldest
    /// first (design section 6.4 "history grouped by generation"). Empty when
    /// the track is unknown or no entry matches. Never throws.
    [[nodiscard]] std::vector<TrackObservation> observations_in_generation(uint64_t track_id,
                                                                           uint32_t generation) const noexcept;

    /// Drops all pool state; the next adoption starts from scratch. Caller
    /// initiated — the tracker never clears itself silently.
    void reset() noexcept;

    [[nodiscard]] const ObjectTrackerOptions& options() const noexcept { return options_; }
    /// Number of tracks currently held, including terminated ones.
    [[nodiscard]] size_t track_count() const noexcept { return tracks_.size(); }
    /// Track ids in ascending order (deterministic enumeration order).
    [[nodiscard]] std::vector<uint64_t> track_ids() const noexcept;
    /// Returns the track with `track_id`, or nullptr when absent. The pointer
    /// is valid until the next non-const call on this tracker.
    [[nodiscard]] const TargetTrack* find_track(uint64_t track_id) const noexcept;
    /// Current layout generation of this tracker's source. The change-gated
    /// pipeline (M7-07) advances it; pool-only usage keeps it at 0.
    [[nodiscard]] uint32_t layout_generation() const noexcept { return layout_generation_; }
    /// Total bytes the pool currently accounts for, summed over tracks as:
    /// `kTrackOverheadBytes` + templates and negative templates
    /// (`kTemplateOverheadBytes` + thumbnail bytes each) + observations
    /// (`kObservationOverheadBytes` each) + semantics text/label byte lengths.
    /// Always <= `options().pool_budget_bytes`.
    [[nodiscard]] int64_t byte_size() const noexcept { return used_bytes_; }
    /// Cumulative number of tracks evicted by budget/count pressure since
    /// creation or `reset` (RULE-06 accounting).
    [[nodiscard]] uint64_t evicted_track_count() const noexcept { return evicted_count_; }
    /// Cumulative number of position observations evicted from per-track
    /// histories by `record_observation` overflow since creation or `reset`
    /// (RULE-06 accounting; termination releases do not count — they are
    /// caller actions, not budget evictions).
    [[nodiscard]] uint64_t evicted_observation_count() const noexcept { return evicted_observations_; }
    /// Cumulative number of appearance templates evicted by `add_template`
    /// overflow since creation or `reset` (same accounting rule).
    [[nodiscard]] uint64_t evicted_template_count() const noexcept { return evicted_templates_; }
    /// Cumulative number of impostor templates evicted by
    /// `add_negative_template` overflow since creation or `reset`.
    [[nodiscard]] uint64_t evicted_negative_template_count() const noexcept { return evicted_negative_templates_; }

private:
    /// Accounted bytes of one track (must track the `byte_size` contract).
    [[nodiscard]] static int64_t track_bytes(const TargetTrack& track) noexcept;
    /// Eviction priority: true when `candidate` should be evicted before
    /// `resident` (terminated first, then oldest verification, then lower id).
    [[nodiscard]] static bool evicts_before(const TargetTrack& candidate, const TargetTrack& resident) noexcept;
    /// Mutable track lookup by id, or nullptr when absent.
    [[nodiscard]] TargetTrack* find_track_mutable(uint64_t track_id) noexcept;
    /// Shared bounded-template-store path of `add_template` and
    /// `add_negative_template`: validates the fingerprint against the pinned
    /// thumbnail side, applies the count-pressure drop-oldest rule over `set`
    /// (skipping the first `pinned_entries` entries), enforces the pool byte
    /// budget and commits both the eviction and the insertion atomically.
    [[nodiscard]] Result<void> store_template(std::vector<TrackTemplate>& set, int32_t capacity, size_t pinned_entries,
                                              const TrackTemplate& entry, uint64_t& evicted_counter) noexcept;

    ObjectTrackerOptions options_;
    /// Tracks sorted by ascending track_id (deterministic enumeration).
    std::vector<TargetTrack> tracks_;
    uint32_t layout_generation_ = 0;
    int64_t used_bytes_ = 0;
    uint64_t evicted_count_ = 0;
    uint64_t evicted_observations_ = 0;
    uint64_t evicted_templates_ = 0;
    uint64_t evicted_negative_templates_ = 0;
};

}  // namespace mirador

#endif  // MIRADOR_OBJECT_TRACKER_HPP
