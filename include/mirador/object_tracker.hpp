#ifndef MIRADOR_OBJECT_TRACKER_HPP
#define MIRADOR_OBJECT_TRACKER_HPP

#include <mirador/change_detection.hpp>
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
    /// Bounded impostor (negative) templates confirmed against this track.
    /// Frozen M7-05 boundary: the neighborhood verifier reads positive
    /// templates only — collecting impostor templates and the veto they back
    /// (`EvidenceGrade::kVetoed`) is the M7-06 state machine's contract.
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
    /// Work meter of one `verify_track` call (M7-05): the planned E1 scan
    /// work (frozen formula in the `verify_track` contract) is checked
    /// against this budget before the scan runs, and exceeding it fails the
    /// call with kBudgetExceeded (RULE-06) — shrink
    /// `verification_roi_diagonal_ratio` to bound the offset count for large
    /// tracks. > 0. Development smoke default, calibrated by M7-09 (DEC-019
    /// section 5).
    int64_t verification_work_budget_bytes = int64_t{256} * 1024 * 1024;

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

/// Per-track verdict of one change-gate evaluation (object-tracking design
/// section 6.1, M7-03). The gate classifies only: it never mutates the pool
/// and never fabricates a verification outcome (the neighborhood verifier is
/// M7-05).
enum class ChangeGateDecision : uint8_t {
    /// Short-circuit reuse (levels 1-2 of the gate): the frame is unchanged
    /// or no change ROI intersects the track's bounds, so the track keeps its
    /// position estimate and no verification work is spent on it this frame.
    kReuse,
    /// Level-3 entry: a change ROI intersects the track's bounds (or the
    /// classification is kGlobal) — the track goes to the neighborhood
    /// verification entry, whose body is delivered with M7-05.
    kVerify,
    /// The track is not kTracking (kUncertain/kLost/kTerminated): the gate
    /// makes no short-circuit decision for it and reports the state explicitly
    /// instead of skipping it silently (state transitions belong to M7-06).
    kInactive,
};

/// One track's entry of a `ChangeGateTrace`.
struct TrackGateDecision {
    uint64_t track_id = 0;
    /// Track state at decision time (echo; the gate never changes it).
    TrackState state = TrackState::kTracking;
    ChangeGateDecision decision = ChangeGateDecision::kInactive;
    /// Scan-order index into `ChangeReport::changed_regions` of the first ROI
    /// intersecting the track's bounds. Set only for kVerify verdicts under a
    /// kPartial classification; a kGlobal verdict stays empty because the
    /// whole frame changed and no single ROI drives its verification.
    std::optional<size_t> change_region_index;
};

/// Deterministic whole-pool result of one `evaluate_change_gate` call: the
/// echoed classification plus one entry per held track in ascending
/// `track_id` order (the pool's deterministic enumeration order, terminated
/// archives included). Bounded by `options().max_targets` entries.
struct ChangeGateTrace {
    ChangeClassification classification = ChangeClassification::kNone;
    std::vector<TrackGateDecision> tracks;
};

// ---- Neighborhood verification (M7-05, object-tracking design section 6.2;
// frozen contract decisions recorded here and on `verify_track`) ----

/// The three closure-structure description quantities of a
/// `GeometricRegionProposal` (`geometric_proposal.hpp`, DEC-018 stage 1),
/// carried bare by the M7-05 verification contract. Frozen shape decision:
/// the verifier consumes these numbers instead of the proposal type, so the
/// fusion link interface stays exactly core/image/cache — zero new module
/// dependencies (DEC-019 phase A; no DEC-013 allowed-set evolution needed).
/// The remaining proposal fields (bounds, oriented bounds, segments) carry
/// nothing the deviation comparison consumes. The caller obtains the
/// descriptors by running `propose_regions` inside
/// `ObjectTracker::verification_roi` (or from any equivalent structure
/// evidence source), from the same frame as the view passed to
/// `verify_track`. All values live in [0, 1] per the frozen proposal
/// contract; anything else is rejected as kInvalidArgument.
struct TrackStructureDescriptors {
    /// Degree of closure in [0, 1] (see
    /// `GeometricRegionProposal::closure_score`).
    float closure_score = 0.0F;
    /// Principal-direction alignment fraction in [0, 1].
    float rectangularity = 0.0F;
    /// Junction-sharing segment fraction in [0, 1].
    float edge_support = 0.0F;

    /// Component equality (test convenience).
    [[nodiscard]] friend bool operator==(const TrackStructureDescriptors& lhs,
                                         const TrackStructureDescriptors& rhs) noexcept {
        return lhs.closure_score == rhs.closure_score && lhs.rectangularity == rhs.rectangularity &&
               lhs.edge_support == rhs.edge_support;
    }
};

/// Evidence level of the E1 (template NCC) channel of one verification
/// (object-tracking design sections 3 and 6.2). Channel evidence only — this
/// is deliberately not an `EvidenceGrade`: mapping grades to state
/// transitions is the M7-06 state machine's decision (DEC-010 gating
/// semantics).
enum class AppearanceChannelOutcome : uint8_t {
    /// Below the weak threshold, or a peak the sidelobe gate rejects: a flat
    /// response surface is never trusted at any peak height.
    kNone,
    /// Peak at or above `ncc_weak_threshold` with trusted peak quality.
    kWeak,
    /// Peak at or above `ncc_strong_threshold` with trusted peak quality.
    kStrong,
};

/// Outcome of the E2 (closure-structure consistency) channel of one
/// verification (design section 6.2). Evidence only; the M7-06 state machine
/// decides what it means for the track. Missing inputs are reported
/// explicitly instead of fabricating a verdict.
enum class StructureChannelOutcome : uint8_t {
    /// The caller passed no descriptors for this call.
    kNotSupplied,
    /// Descriptors supplied, but the track holds no recorded baseline (one
    /// is stored per track by `record_structure_baseline`).
    kNoBaseline,
    /// Every descriptor deviation is within
    /// `structure_deviation_tolerance`.
    kConsistent,
    /// At least one descriptor deviates beyond the tolerance.
    kDeviated,
};

/// E1 channel evidence of one `verify_track` call (M7-05).
struct AppearanceVerification {
    AppearanceChannelOutcome outcome = AppearanceChannelOutcome::kNone;
    /// Winning peak of the NCC response surface in [-1, 1] (1.0 = identical
    /// thumbnails under the M3-10 normalization, including its flat-vs-flat
    /// rule).
    double peak_ncc = 0.0;
    /// Peak-to-sidelobe quality of the winning template's response surface
    /// (frozen formula in the `verify_track` contract); >= 0.
    double peak_sidelobe_ratio = 0.0;
    /// Index into `TargetTrack::templates` of the winning template
    /// (0 = the adoption template).
    uint32_t best_template_index = 0;
    /// Integer translation in presented pixels of the track bounds at which
    /// the winning peak was found, relative to the predicted position
    /// (`bounds.x + dx`, `bounds.y + dy`). (0, 0) on the edge-clamped
    /// fallback (see `verify_track`).
    int32_t best_offset_dx = 0;
    int32_t best_offset_dy = 0;
};

/// E2 channel evidence of one `verify_track` call (M7-05).
struct StructureVerification {
    StructureChannelOutcome outcome = StructureChannelOutcome::kNotSupplied;
    /// Per-quantity relative deviations from the recorded baseline (frozen
    /// formula in the `verify_track` contract); 0 when not comparable.
    /// Unclamped — values can exceed 1 for near-zero baselines and are
    /// reported as computed for M7-09 calibration visibility.
    double closure_deviation = 0.0;
    double rectangularity_deviation = 0.0;
    double edge_support_deviation = 0.0;
    /// Maximum of the three deviations; the channel is kConsistent when this
    /// is <= `structure_deviation_tolerance`.
    double max_deviation = 0.0;
};

/// Whole result of one `verify_track` call: both channel evidences for one
/// track plus the context they were computed against (M7-05).
struct TrackVerification {
    uint64_t track_id = 0;
    /// Track state at verification time (echo; the verifier never changes
    /// it).
    TrackState state = TrackState::kTracking;
    /// The clamped pixel verification ROI the E1 search ran in — the same
    /// region the caller is expected to have run the E2 detection and
    /// `propose_regions` in (`ObjectTracker::verification_roi` computes it
    /// with the identical rule).
    RectI verification_roi;
    AppearanceVerification appearance;
    StructureVerification structure;
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
    /// Byte overhead of one track's E2 structure baseline slot (M7-05).
    static constexpr int64_t kStructureBaselineOverheadBytes = 32;

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

    /// Change-gated three-level short circuit over the whole pool (M7-03;
    /// object-tracking design section 6.1). Consumes the caller's
    /// `ChangeReport` — the session main loop runs `detect_change` (M1); this
    /// gate holds no previous frame and never re-implements change detection —
    /// and classifies every held track as a pure read (`const`): no pool state
    /// changes on any path.
    ///
    /// Levels (cost-ascending, per design section 6.1):
    ///   1. `kNone` — frame not significantly changed: every kTracking track
    ///      is `kReuse` with zero per-track geometric work (the near-zero
    ///      path; the fingerprint comparison is the cost `detect_change`
    ///      already paid).
    ///   2. `kPartial` — per track, the change ROIs are tested against
    ///      `last_bounds`, which until the motion primitives land (M7-04/
    ///      M7-07) is exactly the extrapolated position (`predicted_center`
    ///      is the `last_bounds` center). No intersection → `kReuse`
    ///      (disjoint short circuit); intersection → `kVerify` carrying the
    ///      first intersecting ROI's scan-order index. Verdicts are
    ///      independent per track: one track's verdict never influences
    ///      another's.
    ///   3. `kGlobal` — no track short-circuits: every kTracking track is
    ///      `kVerify` (with an empty `change_region_index`). The
    ///      layout-generation advance a global classification may trigger
    ///      belongs to the M7-07 pipeline and is deliberately NOT done here
    ///      (see `advance_layout_generation`).
    ///
    /// Adjudications ahead of the M7-06 state machine: tracks in
    /// kUncertain/kLost/kTerminated get an explicit `kInactive` entry — never
    /// a silent skip; terminated archives stay visible in the trace. Reuse
    /// never advances `last_verified_sequence` or any other evidence field:
    /// a short-circuit consumes no appearance evidence (design section 3 —
    /// the position prior is gating/placeholder evidence only), so
    /// evidence-grade confirmations stay with the M7-06 state machine and
    /// frame-stamped history bookkeeping stays with the caller via
    /// `record_observation`. The gate is a pure decision and therefore takes
    /// no frame_sequence.
    ///
    /// RISK-2026-17 note: before motion compensation exists (M7-04/M7-07), a
    /// scroll-class change intersects every track and all of them enter the
    /// verification entry — that is the designed behavior at this stage, not
    /// a defect.
    ///
    /// Coordinates: the change ROIs must be expressed in this tracker's single
    /// coordinate space (for session pipelines the oriented view space — the
    /// same space `ChangeReport::changed_regions` uses when `detect_change`
    /// runs on the presented view); mixing spaces across calls is the caller's
    /// error, as everywhere in this header.
    ///
    /// Determinism: identical inputs produce bit-identical traces (ascending
    /// `track_id`; ROI indices follow the report's scan order). Errors:
    /// kInvalidArgument when `report.classification` holds an unknown value or
    /// any entry of `changed_regions` is not a non-empty rect;
    /// kCancelled/kTimeout from `context` (checked at entry and per track on
    /// the kPartial path; a cancelled call returns only the Status, never a
    /// partial trace). On error the tracker is untouched. Bounded work:
    /// O(tracks × ROIs), both bounded (RULE-06); the scan is scalar-only, so
    /// nothing is allocated beyond the returned trace and, on error, the
    /// Status message. Never throws.
    [[nodiscard]] Result<ChangeGateTrace> evaluate_change_gate(const ChangeReport& report,
                                                               const ExecutionContext& context = {}) const noexcept;

    /// Verification ROI of one track in the presented view (M7-05;
    /// object-tracking design section 6.2): the track bounds expanded around
    /// `predicted_center` by
    /// `verification_roi_diagonal_ratio * bounds_diagonal / 2` per side
    /// (until the motion pipeline lands, `predicted_center` is exactly the
    /// `last_bounds` center), converted with the `adopt_track` covering rule
    /// (floor on the leading edge, ceil on the trailing edge) and clamped
    /// into the view. Precision note: the expansion is computed in double
    /// and narrowed to float per component before the covering conversion,
    /// so ROI edges can sit one pixel off a full-double computation — equal
    /// inputs stay bit-identical, and M7-09 calibration treats this as the
    /// frozen ROI metering. Frozen ROI semantics — `verify_track` searches
    /// exactly this region for E1, and callers run their E2 evidence
    /// production (line detection + `propose_regions`) inside it so both
    /// channels see the same neighborhood.
    ///
    /// Pure read (`const`). Errors: kInvalidArgument for an invalid view, an
    /// unknown or already-terminated track, or an expanded ROI that does not
    /// intersect the view at all. Never throws.
    [[nodiscard]] Result<RectI> verification_roi(uint64_t track_id, const ImageView& presented_view) const noexcept;

    /// Neighborhood verification of one track (M7-05; object-tracking design
    /// section 6.2): a pure per-track decision — `const`, no pool state
    /// changes on any path, `last_verified_sequence` and every other evidence
    /// field stay untouched, and no grade-to-state mapping or layout-
    /// generation decision happens here (all of that is the M7-06 state
    /// machine's contract, the same boundary the M7-03 gate froze). This
    /// entry accepts tracks in any non-terminated state (the M7-08
    /// redetection identity review reuses it for kLost candidates);
    /// kTerminated is rejected because its identity is closed. Two channels:
    ///
    /// E1 template NCC (multi-template best with peak-sidelobe quality):
    /// every integer translation of the track bounds window that stays fully
    /// inside the verification ROI is extracted with the adoption pipeline
    /// (`crop` + `make_visual_patch_fingerprint` at
    /// `options().template_thumb_side`) and NCC-scored against every stored
    /// positive template with the M3-10 normalization (the `VisualIndex`
    /// template layer formula; negative templates are never read — the
    /// impostor veto is frozen for M7-06). Each template yields one response
    /// surface over the offsets. Per template, the peak is the maximum under
    /// the total order (peak NCC, then offset Chebyshev radius, then dy,
    /// then dx — the M7-04 winner style), and the peak-to-sidelobe quality
    /// is `PSR = (peak - mean_sidelobe) / (stddev_sidelobe + 1e-12)` with
    /// the population stddev over that surface's remaining responses: a
    /// flat surface (stddev 0, peak equal to the sidelobe mean) scores 0, a
    /// single-candidate surface scores peak/1e-12 (trivially distinctive).
    /// The winning template is the maximum under (peak NCC, then PSR, then
    /// template index). `outcome` is kStrong when peak >=
    /// `ncc_strong_threshold` and PSR >= `peak_sidelobe_ratio_min`, else
    /// kWeak when peak >= `ncc_weak_threshold` and PSR >=
    /// `peak_sidelobe_ratio_min`, else kNone: a peak on a flat response
    /// surface is never trusted, at any height.
    ///
    /// E2 closure-structure consistency: the caller-supplied descriptors are
    /// compared against the track's recorded baseline
    /// (`record_structure_baseline`). Per quantity the relative deviation is
    /// `|current - baseline| / max(|baseline|, 1e-6)`, evaluated in double
    /// from the float values; the channel is kConsistent when the maximum of
    /// the three is <= `structure_deviation_tolerance`. kNotSupplied (no
    /// descriptors passed) and kNoBaseline (no baseline recorded yet — a
    /// fresh track starts without one; record the adoption frame's proposal
    /// to bootstrap the channel) report the missing-input states explicitly
    /// instead of fabricating a verdict.
    ///
    /// Search-set fallback: when the clamped ROI is smaller than the track
    /// window (edge-clamped tracks) the strict "window inside ROI" offset
    /// set is empty and exactly the offset (0, 0) is evaluated on the
    /// clamped window instead — the "is it still where we think it is"
    /// check; the fallback is visible through `best_offset_* == 0`.
    ///
    /// Bounded work (RULE-06): the planned work — offsets x (2 x
    /// ceil(bounds width) x ceil(bounds height) x bytes-per-pixel +
    /// `template_thumb_side^2` + 2 x template_count x
    /// `template_thumb_side^2`) plus offsets x template_count x 8 bytes of
    /// response-surface storage — is checked against
    /// `options().verification_work_budget_bytes` before the scan starts;
    /// exceeding it fails with kBudgetExceeded before any pixel is read.
    /// Metering precision: the plan sizes the window with the ceil of the
    /// bounds extents, while the per-offset crop follows the covering rule,
    /// so fractional bounds can read up to one pixel row/column more per
    /// offset than the meter counts — the formula is the frozen accounting
    /// meter and that margin is M7-09 calibration's to own. The scan polls
    /// `context` once per offset row; kCancelled/kTimeout return only the
    /// Status, never a partial verification. Validation errors take
    /// precedence over cancellation — a frozen decision of this work item
    /// (M7-05), the deliberate contrast to `adopt_track`, whose M7-02 entry
    /// checks cancellation first.
    ///
    /// Coordinates: `presented_view` must be presented in this tracker's
    /// single coordinate space, and the descriptors must come from the same
    /// frame as `presented_view`; mixing spaces or frames is the caller's
    /// error (the verifier cannot detect either, as everywhere in this
    /// header). Presented dimensions may differ from the adoption frame —
    /// the ROI clamps.
    ///
    /// Determinism: identical inputs produce bit-identical results — fixed
    /// scan order (dy rows ascending, dx columns ascending), the total
    /// orders above, and double arithmetic only on exact integer sums or
    /// single divisions.
    ///
    /// Errors: kInvalidArgument for an invalid view, an unknown or
    /// already-terminated track, a track with no appearance templates (a
    /// defensive branch — unreachable through the public API: terminated
    /// tracks are rejected above and adoption always stores exactly one
    /// template), a verification ROI that does not intersect the view, a
    /// fallback window covering no pixel of the view, or descriptors that
    /// are non-finite or outside [0, 1]; kBudgetExceeded as above;
    /// kCancelled/kTimeout from `context`. Never throws.
    [[nodiscard]] Result<TrackVerification> verify_track(
        uint64_t track_id, const ImageView& presented_view,
        const std::optional<TrackStructureDescriptors>& structure_descriptors,
        const ExecutionContext& context = {}) const noexcept;

    /// Records the E2 baseline of one track (M7-05 bookkeeping; the design
    /// section 6.2 "pooled baseline"): the descriptors become the track's
    /// comparison baseline for subsequent `verify_track` calls, stamped with
    /// `frame_sequence` and this pool's current layout generation. Each
    /// track holds exactly one baseline slot — recording again overwrites it
    /// (frozen shape decision: a bounded history would add eviction policy
    /// without a consumer; M7-06/M7-09 can extend within Experimental if
    /// calibration needs one). The caller's policy decides when to record —
    /// typically on identity-confirmed frames only, which bounds baseline
    /// drift; the verifier itself never records (pure decision). The slot
    /// costs `kStructureBaselineOverheadBytes`, is accounted in
    /// `byte_size()` and `pool_budget_bytes`, and is released by
    /// `terminate`, by track eviction and by `reset`.
    ///
    /// Errors: kInvalidArgument for descriptors that are non-finite or
    /// outside [0, 1], an unknown or already-terminated track;
    /// kBudgetExceeded when the pool byte budget cannot hold one more
    /// baseline slot. On error the pool is untouched. Never throws.
    [[nodiscard]] Result<void> record_structure_baseline(uint64_t track_id,
                                                         const TrackStructureDescriptors& descriptors,
                                                         uint64_t frame_sequence) noexcept;

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
    /// (`kObservationOverheadBytes` each) + semantics text/label byte lengths,
    /// plus one `kStructureBaselineOverheadBytes` slot per track that holds an
    /// E2 structure baseline (M7-05). Always <= `options().pool_budget_bytes`.
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

    /// Pool-side E2 baseline slot of one track (M7-05). Kept outside the
    /// frozen `TargetTrack` layout; at most one slot per track, sorted by
    /// track_id like `tracks_` (ids always a subset of the live tracks).
    struct StructureBaselineSlot {
        TrackStructureDescriptors descriptors;
        uint64_t frame_sequence = 0;
        uint32_t layout_generation = 0;
    };
    using BaselineSlots = std::vector<std::pair<uint64_t, StructureBaselineSlot>>;
    /// Iterator to the track's baseline slot, or `end()` when absent.
    [[nodiscard]] BaselineSlots::iterator find_baseline_slot(uint64_t track_id) noexcept;
    [[nodiscard]] BaselineSlots::const_iterator find_baseline_slot(uint64_t track_id) const noexcept;
    /// Removes the track's baseline slot if present; returns the bytes freed
    /// (informational — adopt_track's commit path folds them into its planned
    /// eviction account and deliberately ignores the return).
    int64_t erase_structure_baseline(uint64_t track_id) noexcept;
    /// Bytes of the track's baseline slot (0 when the track holds none).
    [[nodiscard]] int64_t baseline_slot_bytes(uint64_t track_id) const noexcept;
    /// Planned (not yet applied) eviction of an `adopt_track` insertion:
    /// terminated tracks first, then oldest by (`last_verified_sequence`,
    /// `track_id`), until the insertion fits both the count and byte bounds.
    struct EvictionPlan {
        std::vector<uint64_t> victim_ids;
        int64_t freed_bytes = 0;
    };
    [[nodiscard]] EvictionPlan plan_eviction(int64_t insertion_bytes) const noexcept;

    ObjectTrackerOptions options_;
    /// Tracks sorted by ascending track_id (deterministic enumeration).
    std::vector<TargetTrack> tracks_;
    /// E2 baselines keyed by track_id, ascending (M7-05).
    BaselineSlots structure_baselines_;
    uint32_t layout_generation_ = 0;
    int64_t used_bytes_ = 0;
    uint64_t evicted_count_ = 0;
    uint64_t evicted_observations_ = 0;
    uint64_t evicted_templates_ = 0;
    uint64_t evicted_negative_templates_ = 0;
};

}  // namespace mirador

#endif  // MIRADOR_OBJECT_TRACKER_HPP
