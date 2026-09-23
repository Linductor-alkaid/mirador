#ifndef MIRADOR_OBJECT_TRACKER_HPP
#define MIRADOR_OBJECT_TRACKER_HPP

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/shift_estimation.hpp>
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
    /// updates capture kConfirmed patches and negative-template collection
    /// stores kVetoed patches, both only through `commit_track_evidence`
    /// (M7-06, the frozen collection policies on that method).
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
    /// Motion-model extrapolation of the center. Exactly the center of
    /// `last_bounds` on every path: adoption and confirming commits set it
    /// there, and the M7-07 compensation (`compensate_global_motion`)
    /// maintains the invariant by translating bounds and center equally — a
    /// separate velocity model does not exist (frozen M7-07 decision, see
    /// there).
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
    /// (`EvidenceGrade::kVetoed`) is the evidence-fusion state machine's
    /// contract, delivered as `commit_track_evidence` (M7-06).
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

    // ---- Evidence fusion (M7-06; object-tracking design sections 3 and 5;
    // initial value, calibrated by M7-09 per DEC-019 section 5). ----
    /// Minimum NCC between the frame's candidate patch and a stored negative
    /// template for the impostor veto to fire in `commit_track_evidence`.
    /// [0, 1].
    double impostor_match_threshold = 0.8;

    // ---- Global motion compensation (M7-07; object-tracking design section
    // 6.3; initial value, calibrated by M7-09 per DEC-019 section 5). ----
    /// Minimum `ShiftEstimate::confidence` for `compensate_global_motion` to
    /// apply the caller's shift estimate to the pool; below it the call
    /// reports `applied == false` and the pool stays untouched. [0, 1]. The
    /// default 0.0 applies every well-formed estimate (development smoke
    /// default — no real-world prior exists yet); this option is
    /// RISK-2026-17's compensation gate, calibrated by M7-09.
    double min_compensation_confidence = 0.0;

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
    /// Capacity of the pool-wide bounded redetection-record log (M7-08): the
    /// interruption events (`record_redetection_recapture`) and recapture
    /// associations (`record_redetection_association`) share one log and one
    /// capacity; overflow drops the oldest record, explicitly counted in
    /// `evicted_redetection_record_count()` (RULE-06). [1, 4096].
    int32_t max_redetection_records = 64;
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
    /// instead of skipping it silently (state transitions are the evidence-
    /// fusion state machine's contract, `commit_track_evidence`, M7-06).
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

// ---- Evidence fusion and the tracking state machine (M7-06;
// object-tracking design sections 3, 4 and 6.4 — frozen contract decisions
// recorded here and on `commit_track_evidence`) ----

/// Motion-state scenario of one frame, declared by the caller and
/// conditioning the position-channel weight per the frozen design section 3
/// table (static: full weight; compensated scroll: full weight —
/// compensation restores the static-period validity; generation switch:
/// zeroed). Declaring the scenario is the caller's evidence: the tracker
/// never classifies motion itself. The M7-07 pipeline entries that produce
/// these declarations are `compensate_global_motion` (apply the
/// `estimate_global_shift` result, then declare `kCompensatedScroll`) and
/// `advance_generation_for_classification` (the kGlobal trigger, then
/// declare `kGenerationSwitch`).
enum class PositionScenario : uint8_t {
    /// Static period: the position gate carries full weight (design section
    /// 3, row 1).
    kStationary,
    /// Scroll / window drag with the caller-applied global motion
    /// compensation: stationary-period validity restored (design section 3,
    /// row 2 / section 6.3), so the position gate carries full weight again.
    /// Declaring this scenario asserts the caller already compensated the
    /// coordinates; the behavioral weight equals `kStationary` — the
    /// distinction exists for trace visibility and M7-09 per-scenario
    /// calibration.
    kCompensatedScroll,
    /// Layout generation switched this frame: the position prior is zeroed
    /// (design section 3, row 3 / section 6.4) — gate membership is not
    /// required for confirmation, position-only evidence claims nothing, and
    /// a kTracking track without confirming appearance evidence degrades to
    /// kUncertain. Appearance templates and semantics evidence survive the
    /// switch (design section 6.4).
    kGenerationSwitch,
};

/// Position-channel evidence of one frame for one track (M7-06). The
/// position prior is gating and placeholder input only — it never confirms
/// an identity by itself (DEC-019 section 2, design section 3).
struct TrackPositionEvidence {
    PositionScenario scenario = PositionScenario::kStationary;
    /// True when the frame's candidate position lies inside the position
    /// gate around `predicted_center`. For verification-derived candidates
    /// this is inherent (the frozen `verification_roi` neighborhood is the
    /// gate, and `verify_track` searches only inside it); the flag carries
    /// the caller's own candidate-association verdict. It is ignored where
    /// the gate is void: a `kGenerationSwitch` scenario, or a kLost track
    /// whose stale prior must not gate a recapture (see
    /// `commit_track_evidence`).
    bool inside_gate = false;
};

/// Deterministic outcome of one `commit_track_evidence` call (M7-06): the
/// computed grade, the state transition it drove and the side effects that
/// actually committed. Evidence fields and template stores change only as
/// described by the flags; on any error nothing is published and the pool
/// is untouched.
struct TrackEvidenceCommit {
    uint64_t track_id = 0;
    /// State before the commit (echo).
    TrackState previous_state = TrackState::kTracking;
    /// State after the commit (echo; `find_track` holds the full record).
    TrackState state = TrackState::kTracking;
    /// The grade the frozen decision table computed for this frame.
    EvidenceGrade grade = EvidenceGrade::kPlaceholder;
    /// Scenario echo of the position evidence.
    PositionScenario scenario = PositionScenario::kStationary;
    /// The candidate patch matched a stored negative template at or above
    /// `options().impostor_match_threshold` (the impostor veto fired).
    bool impostor_hit = false;
    /// The candidate semantics conflict with the track semantics under the
    /// DEC-010 gate predicate (the semantic veto fired).
    bool semantics_conflict = false;
    /// A positive appearance template was captured from the candidate patch
    /// (kConfirmed commits only; bounded store, eviction counted in
    /// `evicted_template_count()`).
    bool template_captured = false;
    /// A negative template was captured from the candidate patch
    /// (semantic-conflict vetoes only, and only when the patch did not
    /// already hit a stored negative template; bounded store, eviction
    /// counted in `evicted_negative_template_count()`).
    bool negative_template_captured = false;
};

// ---- Global motion compensation and layout-generation pipeline (M7-07;
// object-tracking design sections 6.3 and 6.4 — frozen contract decisions
// recorded here and on the three pipeline entries). Shape decision: like
// M7-03/M7-05/M7-06, this milestone delivers pool-side primitives and the
// frame pipeline stays with the caller — the caller runs `detect_change`
// (M1) and `estimate_global_shift` (M7-04), feeds this pool the
// classification trigger, the compensation and the lag sweep, and drives
// verification and commits per frame. The tracker holds no previous frame
// and classifies no motion itself (the M7-03/M7-06 freezes). ----

/// Deterministic outcome of one `advance_generation_for_classification` call
/// (M7-07): whether the pool's layout generation advanced, and its value
/// after the call.
struct GenerationAdvance {
    /// True when the classification was kGlobal and the generation advanced.
    bool advanced = false;
    /// This pool's layout generation after the call (the unchanged value when
    /// `advanced` is false).
    uint32_t generation = 0;
};

/// Per-track echo of one `compensate_global_motion` call (M7-07): the bounds
/// before and after the applied translation, for trace visibility and M7-09
/// calibration.
struct MotionCompensationEntry {
    uint64_t track_id = 0;
    /// Track state at compensation time (echo; compensation never changes it).
    TrackState state = TrackState::kTracking;
    /// `last_bounds` before the translation.
    RectF previous_bounds;
    /// `last_bounds` after the translation; the new `predicted_center` is the
    /// exact center of this rect (the frozen center invariant is maintained,
    /// see `compensate_global_motion`).
    RectF compensated_bounds;
};

/// Deterministic whole-pool result of one `compensate_global_motion` call
/// (M7-07): whether the estimate was applied, the evaluated translation, and
/// one entry per compensated track in ascending `track_id` order. Bounded by
/// `options().max_targets` entries.
struct MotionCompensationResult {
    /// False when the confidence gate refused the estimate
    /// (`shift.confidence < options().min_compensation_confidence`): an
    /// explicit, visible refusal — the pool is untouched, nothing is dropped
    /// silently (RULE-06).
    bool applied = false;
    /// The evaluated frame-space translation (echo of `shift.dx`/`shift.dy`,
    /// also when the gate refused it).
    float dx = 0.0F;
    float dy = 0.0F;
    std::vector<MotionCompensationEntry> tracks;
};

// ---- Cascade redetection primitives and identity review (M7-08;
// object-tracking design section 7 — frozen contract decisions recorded here
// and on the four entries). Shape decision: like M7-03/M7-05/M7-06/M7-07,
// this milestone delivers pool-side primitives and the pipeline stays with
// the caller (RULE-12 — the tracker parameterizes backoff and budget state,
// it never decides to detect, never schedules and holds no thread or timer,
// DEC-001/RULE-03). The composed redetection pipeline the upper layer drives
// per design section 7:
//   gate (`evaluate_redetection_gate`, a pure read) reports kTrigger →
//   coarse recall is the caller's Detector Backend call (this pool consumes
//   none of it and ships no candidate filtering of its own) → identity
//   review of the candidate is `verify_track` reused as frozen (M7-05; it
//   accepts kLost tracks, the stale position prior never gates — design
//   section 7) → confirming evidence is committed through
//   `commit_track_evidence` as frozen (M7-06 — the kLost → kTracking edge
//   is the state machine's, not rewritten here) → `record_redetection_recapture`
//   appends the interruption event and closes the episode; review evidence
//   insufficient → the caller adopts the new region through the normal
//   fusion path (`adopt_track`; DEC-010 — the new-id branch never bypasses
//   the static fusion semantics) → `record_redetection_association` records
//   the identity handoff; every failed attempt is accounted by
//   `record_redetection_failure`, and reaching
//   `options().redetect_max_attempts` consecutive failures makes THAT entry
//   perform the kLost → kTerminated transition — the budget-exhausted end
//   state is explicit and visible, the pool is never cleared silently.
//
// const/state-change boundary (the M7-03/05/06 freeze pattern):
// `evaluate_redetection_gate` is the only pure const query of this section
// (the "should redetection run now" backoff-state read); the three record
// entries are the state changes (attempt accounting with the exhaustion
// edge, episode closure with the event append, association append).
//
// Episode bookkeeping (RULE-06): one loss episode per kLost stretch of a
// track, held in a pool-side slot (`kRedetectSlotOverheadBytes`, the
// M7-05/M7-06 parallel-storage pattern; the frozen `TargetTrack` layout is
// untouched) keyed by the episode's kLost entry sequence. The slot is
// allocated by the first accounted failure, read by the gate, cleared by
// `record_redetection_recapture`, `terminate` (including the exhaustion
// edge), track eviction and `reset`. A slot whose key no longer matches the
// track's current kLost entry sequence is stale (the track was recaptured
// without recapture bookkeeping and re-lost) and reads as the fresh episode
// it is. Consecutive-failure counts and backoff waits count the caller's
// frame sequences, never wall-clock time (the M7-06 RULE-03 pattern).
//
// Trace records (privacy, RULE-10/DOD-06): interruption events and
// associations carry ids and frame sequences only — no coordinates, no
// template or image content, no semantics text. Both kinds share one bounded
// pool-wide log (`options().max_redetection_records` capacity +
// `kRedetectionRecordOverheadBytes` each, accounted in `byte_size()` and
// `pool_budget_bytes`); overflow drops the oldest record with an explicit
// counter, and only `reset` clears the log. ----

/// Verdict of one `evaluate_redetection_gate` call (design section 7 trigger
/// and throttle primitives; RULE-12 — the verdict classifies, the upper
/// layer decides and drives every actual Detector call).
enum class RedetectionGateVerdict : uint8_t {
    /// Change present, the backoff window has elapsed and the attempt budget
    /// is not exhausted: the upper layer may spend one redetection attempt
    /// (coarse recall + identity review) this frame.
    kTrigger,
    /// The frame classification is kNone: a static frame never triggers
    /// redetection (design section 7 — the near-zero change gate of section
    /// 6.1 applies to redetection itself; the zero-trigger negative-test
    /// anchor).
    kHoldStaticFrame,
    /// Change present, but the frame sequence is inside the backoff window
    /// scheduled by the previous accounted failure (`wait_frames` reports
    /// the remaining wait).
    kHoldBackoff,
    /// The track is not kLost (kTracking/kUncertain): redetection does not
    /// apply and the gate reports the state explicitly instead of skipping it
    /// silently (the M7-03 kInactive pattern; degradation is the M7-06 state
    /// machine's contract).
    kInactive,
};

/// Deterministic outcome of one `evaluate_redetection_gate` call (M7-08): a
/// pure per-track classification — no pool state changes on any path.
struct RedetectionGateDecision {
    uint64_t track_id = 0;
    /// Track state at decision time (echo; the gate never changes it).
    TrackState state = TrackState::kLost;
    RedetectionGateVerdict verdict = RedetectionGateVerdict::kInactive;
    /// Consecutive failed redetection attempts accounted for the current loss
    /// episode (0 before the first accounted failure; a stale episode slot
    /// reads as 0 — see the section note above).
    uint32_t attempts = 0;
    /// Remaining backoff wait in frames; nonzero only for kHoldBackoff
    /// verdicts. Saturated at uint32 max (reachable only when the caller
    /// moves its frame sequence backwards).
    uint32_t wait_frames = 0;
};

/// Deterministic outcome of one `record_redetection_failure` call (M7-08):
/// the accounted attempt state after the record. When the attempt budget is
/// exhausted, `state` is kTerminated (the explicit, visible failure of
/// design section 7 — the archive semantics are exactly `terminate`'s) and
/// the backoff fields are zero: a terminated identity schedules nothing.
struct RedetectionFailureRecord {
    uint64_t track_id = 0;
    /// Track state after the record: kLost, or kTerminated on exhaustion.
    TrackState state = TrackState::kLost;
    /// Consecutive failed attempts after this record (>= 1).
    uint32_t attempts = 0;
    /// Backoff wait this failure scheduled, in frames: the frozen doubling
    /// sequence `min(base * 2^(attempts-1), max)` over
    /// `options().redetect_backoff_base_frames` /
    /// `redetect_backoff_max_frames`; 0 when the record terminated the track.
    int32_t backoff_frames = 0;
    /// First frame sequence at which the next attempt may trigger
    /// (the recorded frame sequence + `backoff_frames`, saturating); 0 when
    /// the record terminated the track.
    uint64_t next_attempt_sequence = 0;
};

/// Frozen interruption-event record (M7-08; design section 7 ID semantics —
/// "review passed → the track_id continues, the interruption is recorded").
/// Appended to the bounded pool log by `record_redetection_recapture` after
/// the confirming `commit_track_evidence`; ids and frame sequences only
/// (RULE-10).
struct TrackInterruptionEvent {
    uint64_t track_id = 0;  ///< the recaptured (identity-continued) track
    /// Frame sequence at which the track entered kLost, as supplied by the
    /// caller's evidence (the state machine's kLost entry sequence is zeroed
    /// by the confirming commit, so the caller — which observed the loss
    /// through the commit echo or the sweep trace — carries it; the same
    /// evidence-trust boundary as every method of this header).
    uint64_t lost_sequence = 0;
    /// Frame sequence of the recapture (the recorded call's sequence).
    uint64_t recapture_sequence = 0;
    /// Failed attempts the episode had accounted when it ended.
    uint32_t attempts = 0;
};

/// Frozen recapture-association record (M7-08; design section 7 ID
/// semantics — "evidence insufficient → a new id is assigned by the fusion
/// rules, the association is recorded"). Appended by
/// `record_redetection_association`; a diagnostic identity-handoff note
/// only — neither track's state is changed by it.
struct RedetectionAssociation {
    /// The kLost track whose identity review did not confirm (the open
    /// identity that was replaced).
    uint64_t predecessor_track_id = 0;
    /// The newly adopted track (its id came through `adopt_track` from the
    /// fusion path — DEC-010).
    uint64_t successor_track_id = 0;
    /// Frame sequence the association was recorded at.
    uint64_t sequence = 0;
};

/// Kind tag of one bounded redetection-log entry (M7-08).
enum class RedetectionRecordKind : uint8_t {
    kInterruption,  ///< a `TrackInterruptionEvent` (identity continued)
    kAssociation,   ///< a `RedetectionAssociation` (identity handed off)
};

/// One entry of the pool-wide bounded redetection log (M7-08): the stored
/// form of the two record kinds above, oldest first, capacity
/// `options().max_redetection_records`, overflow drops the oldest (counted).
/// Field mapping — kInterruption: `track_id` is the recaptured track,
/// `related_track_id` is 0, `lost_sequence`/`attempts` carry the event;
/// kAssociation: `track_id` is the predecessor, `related_track_id` the
/// successor, `lost_sequence`/`attempts` are 0. Ids and sequences only
/// (RULE-10); returned by `redetection_records()`.
struct RedetectionRecord {
    RedetectionRecordKind kind = RedetectionRecordKind::kInterruption;
    /// Frame sequence the record was appended at.
    uint64_t sequence = 0;
    uint64_t track_id = 0;
    uint64_t related_track_id = 0;
    uint64_t lost_sequence = 0;  ///< kInterruption only
    uint32_t attempts = 0;       ///< kInterruption only
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
    /// Byte overhead of one track's state-machine slot (M7-06): the
    /// consecutive-insufficient counter and the kLost entry sequence. At
    /// most one slot per track, allocated on the first insufficient/kLost
    /// transition, released by `terminate`, track eviction and `reset`
    /// (the same pool-side parallel storage pattern as the M7-05 baseline
    /// slots; the `TargetTrack` layout stays frozen).
    static constexpr int64_t kStateSlotOverheadBytes = 16;
    /// Byte overhead of one track's redetection episode slot (M7-08): the
    /// loss-episode bookkeeping — the episode's kLost entry sequence (key),
    /// the consecutive failed attempt count and the scheduled next-attempt
    /// sequence. At most one slot per track, allocated by the first accounted
    /// redetection failure, released by `record_redetection_recapture`,
    /// `terminate` (including the budget-exhaustion edge of
    /// `record_redetection_failure`), track eviction and `reset` (the same
    /// pool-side parallel storage pattern as the M7-05 baseline and M7-06
    /// state slots; the `TargetTrack` layout stays frozen).
    static constexpr int64_t kRedetectSlotOverheadBytes = 24;
    /// Byte overhead of one record of the bounded pool-wide redetection log
    /// (M7-08): interruption events and recapture associations, ids and
    /// frame sequences only.
    static constexpr int64_t kRedetectionRecordOverheadBytes = 48;

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
    /// stays visible ("failure is visible", design section 4). The pool-side
    /// E2 baseline (M7-05), state-machine (M7-06) and redetection-episode
    /// (M7-08) slots are released with it. This is the caller-driven
    /// kLost/kTracking → kTerminated edge of the state machine (M7-06); the
    /// redetect-budget-exhausted edge is M7-08's and is performed by
    /// `record_redetection_failure` itself (same archive semantics, no need
    /// to call this entry there). Errors:
    /// kInvalidArgument for an unknown or already-terminated id. Never throws.
    [[nodiscard]] Result<void> terminate(uint64_t track_id, uint64_t frame_sequence) noexcept;

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
    /// The trigger decision (global change classification) is the M7-07
    /// pipeline's, delivered as `advance_generation_for_classification`;
    /// per-track degradation on a generation switch belongs to the
    /// evidence-fusion state machine (`commit_track_evidence` with a
    /// `kGenerationSwitch` scenario, M7-06) — this call only moves the
    /// deterministic counter.
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
    ///      `last_bounds`, which is exactly the extrapolated position on
    ///      every path (`predicted_center` is the `last_bounds` center,
    ///      maintained by the M7-07 compensation). No intersection →
    ///      `kReuse` (disjoint short circuit); intersection → `kVerify`
    ///      carrying the first intersecting ROI's scan-order index.
    ///      Verdicts are independent per track: one track's verdict never
    ///      influences another's.
    ///   3. `kGlobal` — no track short-circuits: every kTracking track is
    ///      `kVerify` (with an empty `change_region_index`). The
    ///      layout-generation advance a global classification may trigger
    ///      belongs to the M7-07 pipeline and is deliberately NOT done here
    ///      (delivered as `advance_generation_for_classification`).
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
    /// generation decision happens here (all of that is the evidence-fusion
    /// state machine's contract, delivered as `commit_track_evidence` in
    /// M7-06 — the same boundary the M7-03 gate froze). This
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

    /// Evidence fusion and state commit of one verified frame for one track
    /// (M7-06; object-tracking design sections 3, 4 and 6.4). This is the
    /// frame pipeline's single state-mutating evidence stage: the M7-03
    /// gate and the M7-05 verifier are pure `const` decisions by frozen
    /// contract, and the grade-to-state mapping they deliberately deferred
    /// happens here and nowhere else. Inputs are the caller's frame
    /// evidence: the `TrackVerification` produced by `verify_track` (the
    /// commit does not re-run the scan and trusts the channel outcomes it is
    /// handed — the same evidence-trust boundary as every method of this
    /// header), the caller-declared `TrackPositionEvidence`, the candidate
    /// semantics of the caller's region association, and the presented view
    /// (the candidate patch is re-extracted from it for the impostor check
    /// and the template captures).
    ///
    /// Candidate window: the track bounds translated by the E1
    /// `best_offset_*` when the E1 channel has an outcome (kWeak/kStrong),
    /// else the unchanged bounds — an E2-only confirmation has no position
    /// of its own. The patch is extracted with the adoption pipeline
    /// (`crop` covering rule + `make_visual_patch_fingerprint` at
    /// `options().template_thumb_side`); one extraction serves the impostor
    /// check and both captures.
    ///
    /// Grade decision table (frozen; evaluated top to bottom, first hit
    /// wins, design section 3):
    ///   1. Impostor evidence — the candidate patch matches any stored
    ///      negative template with NCC >=
    ///      `options().impostor_match_threshold` (checked only when the E1
    ///      channel found a candidate) → `EvidenceGrade::kVetoed`
    ///      (`impostor_hit`).
    ///   2. Semantic conflict — the candidate semantics conflict with the
    ///      track semantics under the DEC-010 gate predicate (both labels
    ///      non-empty and different; no candidate supplied is vacuously
    ///      compatible) → kVetoed (`semantics_conflict`).
    ///   3. Strong appearance — E1 kStrong, or E2 kConsistent — with the
    ///      position gate admitted → `EvidenceGrade::kConfirmed`.
    ///   4. Weak appearance — E1 kWeak — with the position gate admitted →
    ///      `EvidenceGrade::kTentative`.
    ///   5. Everything else → `EvidenceGrade::kPlaceholder` (position prior
    ///      only, or appearance present but the gate refused it: under
    ///      DEC-019 section 2 the position gate is a necessary condition,
    ///      so an out-of-gate candidate is not admitted as identity
    ///      evidence).
    /// The gate is admitted when `position.inside_gate` is true, or when it
    /// is void: a `kGenerationSwitch` scenario (position prior zeroed, so
    /// appearance + semantics confirm without it — design section 6.4), or
    /// a kLost track (the stale prior must not gate a recapture; the M7-08
    /// identity review is appearance evidence by design section 7).
    ///
    /// State transitions (frozen; deterministic, no wall-clock — RULE-03):
    ///   - kConfirmed / kTentative: the track becomes kTracking from every
    ///     accepted state. The kLost → kTracking entry is the recapture
    ///     semantic of design section 4, defined here as a state-machine
    ///     rule only — the redetection primitives and the identity-review
    ///     entry that produce such commits are M7-08 work (`verify_track`
    ///     already accepts non-terminated tracks for that reuse), and the
    ///     interruption-event log stayed with M7-08 too: it is
    ///     `record_redetection_recapture`, called by the redetection
    ///     pipeline after this commit confirms the recapture.
    ///   - kPlaceholder / kVetoed: kTracking and kUncertain degrade to
    ///     kUncertain — a vetoed commit is excluded-candidate evidence, so
    ///     the appearance channel has nothing to confirm with this frame —
    ///     while kLost tracks stay kLost (loss is sticky until a confirming
    ///     grade, caller `terminate`, or the M7-07 generation-exhaustion
    ///     sweep `sweep_generation_lag`). The
    ///     consecutive-insufficient counter (placeholder and vetoed commits
    ///     since the last confirming one) increments per commit, and
    ///     reaching `options().uncertain_frame_limit` transitions the track
    ///     to kLost: the L-th consecutive insufficient commit is the
    ///     transition, the (L-1)-th leaves the track kUncertain (both sides
    ///     of the boundary are observable). The limit counts commits, not
    ///     wall-clock frames — the tracker has no timer and frame stepping
    ///     stays with the M7-07 pipeline.
    ///   - kTerminated tracks are rejected (identity closed, M7-01).
    ///
    /// Committed fields on a confirming grade: `last_bounds` moves to the
    /// candidate window, `predicted_center` stays exactly the new
    /// `last_bounds` center (frozen invariant — maintained, not replaced, by
    /// the M7-07 motion path: `compensate_global_motion` translates bounds
    /// and center equally), `confidence` becomes the clamped E1 peak NCC (the
    /// prior value is
    /// kept on an E2-only confirmation — the structure channel has no
    /// confidence scalar), `last_verified_sequence` receives
    /// `frame_sequence`, and `layout_generation` advances to the pool's
    /// current generation (every commit — the track has lived through it).
    /// A kConfirmed commit also captures the candidate patch as a positive
    /// appearance template; kTentative never writes templates. Placeholder
    /// and vetoed commits change no evidence field: the vetoed candidate
    /// must not move the track, and the position prior is not appearance
    /// evidence (the M7-03 freeze).
    ///
    /// Frozen template-collection policies: a kConfirmed commit captures
    /// the candidate patch as a positive template when
    /// `options().max_templates >= 2` (an evictable slot beyond the pinned
    /// adoption template must exist; with `max_templates == 1` the update
    /// policy is off and the commit succeeds without a capture). A
    /// semantic-conflict veto captures the candidate patch as a negative
    /// template when `options().max_negative_templates >= 1` AND the patch
    /// did not already hit a stored negative template — an impostor is
    /// collected exactly once, at its first rejected confirmation attempt,
    /// and impostor-hit vetoes store nothing (the template is already
    /// pooled). Both captures go through the bounded stores (oldest
    /// evicted first, explicitly counted in the eviction counters).
    ///
    /// State bookkeeping (RULE-06): the consecutive-insufficient counter
    /// and the kLost entry sequence live in one pool-side slot per track
    /// (`kStateSlotOverheadBytes`), allocated on first need, released by
    /// `terminate`, track eviction and `reset`, accounted in `byte_size()`
    /// and `pool_budget_bytes` exactly like the M7-05 baseline slots. The
    /// whole commit is atomic: the patch is extracted before any mutation,
    /// the planned template insertions and slot allocation are
    /// budget-checked together, and any failure — including a capture that
    /// cannot fit — leaves the pool completely untouched.
    ///
    /// Validation precedes cancellation (frozen M7-06 decision — the same
    /// order as the M7-05 verifier and the deliberate contrast to
    /// `adopt_track`'s M7-02 cancel-first entry): cancellation is polled
    /// exactly once, at the entry — after validation and immediately before
    /// the patch extraction, the commit's only pixel work.
    ///
    /// Coordinates: `presented_view` must be the same frame, in the same
    /// space, that produced `verification` — mixing frames or spaces is the
    /// caller's error, as everywhere in this header. Determinism: identical
    /// inputs produce bit-identical commits and traces (fixed evaluation
    /// order, the total orders above, exact integer sums with single
    /// divisions only).
    ///
    /// Errors: kInvalidArgument for an invalid view, an unknown or already-
    /// terminated track, a verification carrying another track's id,
    /// non-finite or empty track bounds, or a candidate window that covers
    /// no pixel of the view; kBudgetExceeded when the planned slot or
    /// template stores cannot fit the pool budget; kCancelled/kTimeout from
    /// `context`. Never throws.
    [[nodiscard]] Result<TrackEvidenceCommit> commit_track_evidence(
        uint64_t track_id, const TrackVerification& verification, const TrackPositionEvidence& position,
        const std::optional<TrackSemantics>& candidate_semantics, const ImageView& presented_view,
        uint64_t frame_sequence, const ExecutionContext& context = {}) noexcept;

    /// Applies the M7-07 pipeline's frozen trigger decision that maps the
    /// frame's global change classification onto the layout generation
    /// (object-tracking design section 6.4; the decision the M7-03 gate
    /// deliberately deferred — `evaluate_change_gate` stays a pure read):
    /// `ChangeClassification::kGlobal` advances the generation by one (the
    /// design section 6.4 global events: dialogs, page switches, theme
    /// changes), `kNone` and `kPartial` leave it untouched. This entry only
    /// moves the deterministic counter (through `advance_layout_generation`)
    /// and reports the outcome — per-track degradation after a switch stays
    /// with the evidence-fusion state machine: the caller declares
    /// `PositionScenario::kGenerationSwitch` on this frame's
    /// `commit_track_evidence` calls, which zeroes the position prior and
    /// degrades a kTracking track without confirming appearance evidence to
    /// kUncertain (M7-06). The exhaustive-evidence end state of a switch is
    /// `sweep_generation_lag`'s contract.
    ///
    /// Determinism: identical inputs produce identical outcomes. Errors:
    /// kInvalidArgument for an unknown classification value;
    /// kBudgetExceeded passed through from `advance_layout_generation` when
    /// the uint32 counter is exhausted (explicit failure, generation
    /// unchanged). Never throws.
    [[nodiscard]] Result<GenerationAdvance> advance_generation_for_classification(
        ChangeClassification classification) noexcept;

    /// Consumes one caller-produced global shift estimate (the M7-04
    /// `estimate_global_shift` result) and applies the design section 6.3
    /// motion compensation to the pool: every held non-terminated track's
    /// `last_bounds` and `predicted_center` are translated by
    /// (`shift.dx`, `shift.dy`) — the frame content moved globally, so every
    /// live position estimate moves with it and the position prior regains
    /// its static-period validity (design section 3, row 2). After the call
    /// the caller declares `PositionScenario::kCompensatedScroll` on this
    /// frame's commits (behavioral weight equals `kStationary`; the
    /// distinction is trace and M7-09 per-scenario calibration visibility).
    ///
    /// Frozen contract decisions (design section 6.3 M7-07 landing):
    ///   - Evidence trust: the estimate is the caller's evidence, exactly
    ///     like every other input of this header — the tracker never runs
    ///     `estimate_global_shift` itself (it holds no previous frame, the
    ///     M7-03 freeze). One call per frame's measured shift is the
    ///     pipeline discipline; feeding the same estimate again translates
    ///     again.
    ///   - Confidence gate: the estimate is applied only when
    ///     `shift.confidence >= options().min_compensation_confidence`;
    ///     below it the result reports `applied == false` and the pool is
    ///     untouched — an explicit refusal, never a silent drop (RULE-06).
    ///     The default threshold 0.0 applies every well-formed estimate;
    ///     the knob is RISK-2026-17's compensation gate and is calibrated
    ///     by M7-09 (DEC-019 section 5).
    ///   - Center invariant maintained: `predicted_center` stays exactly the
    ///     new `last_bounds` center (recomputed from the translated bounds,
    ///     the same formula as `adopt_track` and confirming commits). The
    ///     design's "velocity update" is realized as the translation itself:
    ///     the frozen `TargetTrack` layout (M7-01) has no velocity state,
    ///     and a pipeline feeding each frame's measured shift needs no
    ///     extrapolation lead; any velocity model is M7-09 calibration's to
    ///     propose within Experimental.
    ///   - Scope: kTracking, kUncertain and kLost tracks translate (a kLost
    ///     record keeps its last-known position meaningful as a redetection
    ///     hint in current coordinates); kTerminated archives stay at their
    ///     termination position (identity closed, M7-01). Position history,
    ///     templates, negative templates, E2 baselines, layout generations
    ///     and state slots are untouched — compensation is a coordinate
    ///     update, not evidence, and never rewrites past observations.
    ///
    /// Precision: component-wise float addition (IEEE round-to-nearest,
    /// deterministic); the DOD-03 matrix applies to the compensation path —
    /// rotation and format metadata never affect the translation, it runs on
    /// presented coordinates like every method of this header.
    ///
    /// Determinism: identical inputs produce bit-identical results (fixed
    /// ascending `track_id` application order). Errors: kInvalidArgument
    /// for a non-finite `shift.dx`/`shift.dy`, a confidence outside [0, 1],
    /// or a translation that would drive any non-terminated track's bounds
    /// out of the finite float range; kCancelled/kTimeout from `context` —
    /// polled exactly once at the entry (the `commit_track_evidence`
    /// precedent: the whole-pool pass is bounded, trivial per track and
    /// infallible after validation, so a mid-loop poll could only abort a
    /// half-applied pool). On any error the pool is untouched. Never throws.
    [[nodiscard]] Result<MotionCompensationResult> compensate_global_motion(
        const ShiftEstimate& shift, const ExecutionContext& context = {}) noexcept;

    /// Generation-lag exhaustion sweep (M7-07; the design section 6.4
    /// "generation lag beyond the threshold with exhausted evidence →
    /// kLost" rule — the third kLost stickiness party named by
    /// `commit_track_evidence`, beside confirming grades and caller
    /// `terminate`). Frozen exhaustion definition — a live track is swept
    /// to kLost exactly when BOTH hold:
    ///   - Generation lag: `layout_generation() - track.layout_generation`
    ///     is greater than `options().max_generation_lag`. Every commit
    ///     stamps the track's generation with the pool's current one
    ///     (`commit_track_evidence`), so a lagging track has received no
    ///     commit of any grade for more than `max_generation_lag`
    ///     generations.
    ///   - Exhausted evidence: the track is kUncertain — the M7-06 state
    ///     machine has already judged its frame evidence insufficient
    ///     (position-only placeholder), and no confirming evidence arrived
    ///     across the lag window. kTracking tracks keep their confirmed
    ///     status: their degradation is the M7-06 commit chain (per-frame
    ///     `kGenerationSwitch` commits), which this sweep never fabricates.
    ///     kLost stays kLost (sticky, semantics unchanged); kTerminated
    ///     archives are untouched.
    ///
    /// The transition sets `state` to kLost and records `frame_sequence` as
    /// the loss time in the track's state slot (defensively allocated with a
    /// budget check when absent — a kUncertain track always holds one, since
    /// its degrading commit allocated it). The sweep is planned before
    /// anything mutates and any failure leaves the pool untouched. Every
    /// swept id is reported, ascending (RULE-06 visibility: degradation is
    /// never silent). The kLost → kTracking edge stays exclusively with
    /// confirming commits (M7-06) and the M7-08 identity review.
    ///
    /// Determinism: identical inputs produce identical swept sets (ascending
    /// `track_id`; bounded by `options().max_targets`). Errors:
    /// kCancelled/kTimeout from `context` — polled exactly once at the
    /// entry (same rationale as `compensate_global_motion`);
    /// kBudgetExceeded when a defensive state-slot allocation would not fit
    /// the pool budget. Never throws.
    [[nodiscard]] Result<std::vector<uint64_t>> sweep_generation_lag(uint64_t frame_sequence,
                                                                     const ExecutionContext& context = {}) noexcept;

    /// Redetection gate of one kLost track (M7-08; object-tracking design
    /// section 7, the trigger and throttle primitives — RULE-12: this entry
    /// only classifies, the upper layer decides and drives every Detector
    /// call). A pure read (`const`): no pool state changes on any path, no
    /// scheduling, no thread, no timer (RULE-03). Coupling with the change
    /// gate: the caller's M1 classification decides eligibility — a kNone
    /// (static) frame never triggers redetection (the design section 7
    /// "static frame, no retry" rule, the same near-zero gate that section
    /// 6.1 applies to verification; the zero-trigger negative-test anchor) —
    /// while kPartial/kGlobal frames are eligible. Change ROIs are
    /// deliberately not consumed: a kLost track has no valid position prior
    /// to intersect with (the stale prior must not gate a recapture, the
    /// frozen M7-06 decision), so only the classification carries gate
    /// information for this path.
    ///
    /// Verdicts (see `RedetectionGateVerdict`): kInactive for live non-kLost
    /// tracks (explicit echo, never a silent skip); for kLost tracks
    /// kHoldStaticFrame under kNone, otherwise the episode's backoff state
    /// decides — kHoldBackoff while `frame_sequence` is before the scheduled
    /// next-attempt sequence (`wait_frames` reports the remaining wait),
    /// else kTrigger. The episode view reads the track's redetection slot
    /// (frozen M7-08 shape, see the section note): attempts are the
    /// consecutive failures accounted for the CURRENT loss episode — a slot
    /// keyed by an older episode (recaptured without recapture bookkeeping,
    /// then re-lost) reads as the fresh episode it is. Invariant: a kLost
    /// track has always accounted fewer failures than
    /// `options().redetect_max_attempts` — the max-th accounted failure
    /// terminates the track (`record_redetection_failure`), so the exhausted
    /// end state is the visible kTerminated archive, never a gate verdict.
    ///
    /// Entry-only cancellation poll (the `evaluate_change_gate` precedent:
    /// an O(1) bounded read, so a mid-read poll could only abort a verdict
    /// that costs nothing to redo). Determinism: identical inputs produce
    /// bit-identical decisions. Errors: kInvalidArgument for an unknown
    /// classification value, an unknown track id, or a terminated track
    /// (identity closed — the per-track-entry rule of `verify_track`);
    /// kCancelled/kTimeout from `context`. Never throws.
    [[nodiscard]] Result<RedetectionGateDecision> evaluate_redetection_gate(
        uint64_t track_id, ChangeClassification classification, uint64_t frame_sequence,
        const ExecutionContext& context = {}) const noexcept;

    /// Accounts one failed redetection attempt of one kLost track (M7-08;
    /// the attempt-bookkeeping state change of the frozen const/state-change
    /// boundary). The record IS the caller's declaration that an attempt was
    /// made and failed (evidence trust — the pool observes no Detector call
    /// of its own). Effects, planned before anything mutates:
    ///   - The episode's consecutive-failure count increments (a slot keyed
    ///     by an older episode counts as a fresh episode: the count restarts
    ///     at 1).
    ///   - Before exhaustion: the frozen doubling backoff
    ///     `min(base * 2^(attempts-1), max)` is scheduled — the returned
    ///     record carries the wait and the saturating
    ///     `frame_sequence + backoff_frames` next-attempt sequence; the gate
    ///     holds subsequent triggers until that sequence. Frame counting,
    ///     never wall-clock (RULE-03).
    ///   - At exhaustion — the attempt count reaches
    ///     `options().redetect_max_attempts` — THIS entry performs the
    ///     kLost → kTerminated transition with exactly `terminate`'s archive
    ///     semantics (identity record stays, evidence stores and pool-side
    ///     slots are released, `terminated_sequence` stamped): the
    ///     budget-exhausted end state of design section 7, explicit and
    ///     visible, never a silent pool clear. The record reports the
    ///     kTerminated state with zeroed backoff fields.
    ///
    /// The redetection-episode slot allocation (first accounted failure) is
    /// budget-checked (`kRedetectSlotOverheadBytes`); on any error the pool
    /// is untouched. Entry-only cancellation poll (the
    /// `compensate_global_motion` precedent: an O(1) bounded entry,
    /// infallible after validation). Determinism: identical inputs produce
    /// bit-identical records. Errors: kInvalidArgument for an unknown track
    /// id, a terminated track, or a track that is not kLost (redetection is
    /// the kLost path; kTracking/kUncertain accounting would fabricate an
    /// episode); kBudgetExceeded when the slot allocation does not fit
    /// `pool_budget_bytes`; kCancelled/kTimeout from `context`. Never throws.
    [[nodiscard]] Result<RedetectionFailureRecord> record_redetection_failure(
        uint64_t track_id, uint64_t frame_sequence, const ExecutionContext& context = {}) noexcept;

    /// Records the interruption event of a recaptured track (M7-08; design
    /// section 7 ID semantics — review passed, the track_id continues, the
    /// interruption is recorded). Call AFTER the confirming
    /// `commit_track_evidence`: the track must be kTracking again (that
    /// state IS the recapture proof; the kLost → kTracking edge itself
    /// stays exclusively the frozen M7-06 state machine's). The event —
    /// `TrackInterruptionEvent`: the caller-supplied `lost_sequence`
    /// evidence, the recapture `frame_sequence`, and the episode's accounted
    /// failure count — is appended to the bounded pool-wide redetection log
    /// and the episode slot is released (the episode is closed; a later
    /// loss starts fresh). The log append is planned before anything
    /// mutates: on any error neither the log nor the episode slot changes.
    ///
    /// `lost_sequence` is the caller's evidence (the state machine zeroes
    /// the kLost entry sequence on the confirming commit, so the pool cannot
    /// recover it — the caller observed the loss through the commit echo or
    /// the sweep trace and carries it; the same evidence-trust boundary as
    /// every input of this header). It must not exceed the recapture
    /// sequence. Zero accounted failures (first-attempt recapture) record
    /// `attempts == 0` legitimately.
    ///
    /// Entry-only cancellation poll (as `record_redetection_failure`).
    /// Determinism: identical inputs produce bit-identical events and log
    /// order. Errors: kInvalidArgument for an unknown track id, a
    /// terminated track, a track that is not kTracking (the recapture commit
    /// has not happened), or `lost_sequence > frame_sequence`;
    /// kBudgetExceeded when the log cannot hold one more record;
    /// kCancelled/kTimeout from `context`. Never throws.
    [[nodiscard]] Result<TrackInterruptionEvent> record_redetection_recapture(
        uint64_t track_id, uint64_t frame_sequence, uint64_t lost_sequence,
        const ExecutionContext& context = {}) noexcept;

    /// Records a recapture association (M7-08; design section 7 ID
    /// semantics — review evidence insufficient, the caller assigned a new id
    /// through the normal fusion path, the identity handoff is recorded). A
    /// bounded-log append only: neither track's state is changed, the kLost
    /// predecessor keeps its sticky loss (its later disposition — caller
    /// `terminate`, exhaustion, eviction — stays with the frozen paths), and
    /// the association is a diagnostic note, not an identity claim.
    ///
    /// Entry-only cancellation poll (as `record_redetection_failure`).
    /// Determinism: identical inputs produce bit-identical records and log
    /// order. Errors: kInvalidArgument when the ids are equal, the
    /// predecessor is unknown or not kLost (the association documents a
    /// replaced open identity — terminated or live predecessors are not
    /// that), or the successor is unknown or terminated;
    /// kBudgetExceeded when the log cannot hold one more record;
    /// kCancelled/kTimeout from `context`. Never throws.
    [[nodiscard]] Result<RedetectionAssociation> record_redetection_association(
        uint64_t predecessor_track_id, uint64_t successor_track_id, uint64_t frame_sequence,
        const ExecutionContext& context = {}) noexcept;

    /// Snapshot of the bounded pool-wide redetection log (M7-08), oldest
    /// first — `RedetectionRecord` entries; filter by `kind` for the
    /// interruption events and associations as recorded. Ids and frame
    /// sequences only (RULE-10: no coordinates, no template or image
    /// content, no semantics text). Pure read; cleared by `reset` only.
    /// Never throws.
    [[nodiscard]] std::vector<RedetectionRecord> redetection_records() const noexcept;

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
    /// Current layout generation of this tracker's source. The M7-07
    /// trigger (`advance_generation_for_classification`) advances it;
    /// pool-only usage keeps it at 0.
    [[nodiscard]] uint32_t layout_generation() const noexcept { return layout_generation_; }
    /// Total bytes the pool currently accounts for, summed over tracks as:
    /// `kTrackOverheadBytes` + templates and negative templates
    /// (`kTemplateOverheadBytes` + thumbnail bytes each) + observations
    /// (`kObservationOverheadBytes` each) + semantics text/label byte lengths,
    /// plus one `kStructureBaselineOverheadBytes` slot per track that holds an
    /// E2 structure baseline (M7-05), one `kStateSlotOverheadBytes` slot
    /// per track that holds a state-machine slot (M7-06) and one
    /// `kRedetectSlotOverheadBytes` slot per track that holds a
    /// redetection-episode slot (M7-08), plus `kRedetectionRecordOverheadBytes`
    /// per record of the bounded redetection log (M7-08). Always <=
    /// `options().pool_budget_bytes`.
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
    /// Cumulative number of redetection-log records dropped from the bounded
    /// log's front by capacity pressure since creation or `reset` (M7-08,
    /// RULE-06 accounting; the same rule as the other eviction counters —
    /// termination/`reset` releases do not count).
    [[nodiscard]] uint64_t evicted_redetection_record_count() const noexcept { return evicted_redetection_records_; }

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
    /// Pool-side state-machine slot of one track (M7-06), parallel storage
    /// like the baseline slots above and sorted by track_id the same way.
    /// Holds the consecutive-insufficient counter driving the
    /// `uncertain_frame_limit` transition and the sequence at which the
    /// track entered kLost; absent means both zero.
    struct StateSlot {
        uint32_t insufficient_streak = 0;
        uint64_t lost_sequence = 0;
    };
    using StateSlots = std::vector<std::pair<uint64_t, StateSlot>>;
    /// Iterator to the track's state slot, or `end()` when absent.
    [[nodiscard]] StateSlots::iterator find_state_slot(uint64_t track_id) noexcept;
    [[nodiscard]] StateSlots::const_iterator find_state_slot(uint64_t track_id) const noexcept;
    /// Bytes of the track's state slot (0 when the track holds none).
    [[nodiscard]] int64_t state_slot_bytes(uint64_t track_id) const noexcept;
    /// Removes the track's state slot if present; returns the bytes freed.
    int64_t erase_state_slot(uint64_t track_id) noexcept;
    /// Pool-side redetection-episode slot of one track (M7-08), parallel
    /// storage like the baseline/state slots above and sorted by track_id
    /// the same way. Holds the current loss episode's kLost entry sequence
    /// (the staleness key), the consecutive failed attempt count driving the
    /// backoff/exhaustion primitives and the scheduled next-attempt
    /// sequence; absent means no accounted episode.
    struct RedetectSlot {
        uint64_t episode_lost_sequence = 0;
        uint64_t next_attempt_sequence = 0;
        uint32_t consecutive_failures = 0;
    };
    using RedetectSlots = std::vector<std::pair<uint64_t, RedetectSlot>>;
    /// Iterator to the track's redetection slot, or `end()` when absent.
    [[nodiscard]] RedetectSlots::iterator find_redetect_slot(uint64_t track_id) noexcept;
    [[nodiscard]] RedetectSlots::const_iterator find_redetect_slot(uint64_t track_id) const noexcept;
    /// Bytes of the track's redetection slot (0 when the track holds none).
    [[nodiscard]] int64_t redetect_slot_bytes(uint64_t track_id) const noexcept;
    /// Removes the track's redetection slot if present; returns the bytes
    /// freed.
    int64_t erase_redetect_slot(uint64_t track_id) noexcept;
    /// Archive semantics shared by `terminate` and the budget-exhaustion
    /// edge of `record_redetection_failure` (frozen M7-01 visibility rule):
    /// the track becomes kTerminated at `frame_sequence`, its evidence
    /// stores are released and the pool-side baseline, state-machine and
    /// redetection slots are erased with it.
    void archive_track(TargetTrack& track, uint64_t frame_sequence) noexcept;
    /// Shared bounded-log append of `record_redetection_recapture` and
    /// `record_redetection_association`: fixed-size records, capacity
    /// pressure drops the oldest entry (counted in
    /// `evicted_redetection_record_count()`), pool byte budget enforced. On
    /// error the log — and the pool — are untouched.
    [[nodiscard]] Result<void> append_redetection_record(const RedetectionRecord& record) noexcept;
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
    /// State-machine slots keyed by track_id, ascending (M7-06).
    StateSlots state_slots_;
    /// Redetection-episode slots keyed by track_id, ascending (M7-08).
    RedetectSlots redetect_slots_;
    /// Bounded pool-wide redetection log, oldest first (M7-08).
    std::vector<RedetectionRecord> redetection_records_;
    uint32_t layout_generation_ = 0;
    int64_t used_bytes_ = 0;
    uint64_t evicted_count_ = 0;
    uint64_t evicted_observations_ = 0;
    uint64_t evicted_templates_ = 0;
    uint64_t evicted_negative_templates_ = 0;
    uint64_t evicted_redetection_records_ = 0;
};

}  // namespace mirador

#endif  // MIRADOR_OBJECT_TRACKER_HPP
