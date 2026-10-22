#ifndef MIRADOR_CONTINUOUS_PERCEPTION_HPP
#define MIRADOR_CONTINUOUS_PERCEPTION_HPP

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/transform.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mirador {

class ObjectTracker;

/// Experimental (M9, DEC-026): fields and signatures may change within M9
/// until the M9-07 freeze/promotion gate (the DEC-017/DEC-018 promotion
/// pattern); the contract is not covered by the compatibility promise until
/// then. M9-01 freezes the coordination/relations/policy/event domain
/// vocabulary below — source epochs, coverage/freshness states, work-token
/// bindings, event provenance with begin/update/end semantics and the
/// DEC-026 section 7 reference budgets — plus the pure rules that pin those
/// semantics (relation classification, focus resolution, staleness checks,
/// structural consistency and byte accounting). The deterministic relation-view
/// builder over a snapshot landed in M9-02; the stateful coordination
/// primitives (plan / accept / query / drain_events) follow in M9-03..M9-05
/// inside this same Experimental header, the way `ObjectTracker` grew its
/// pipeline in M7-03..M7-08. Existing single-frame fusion, stable ids and
/// the DEC-021 action-binding protocol are untouched: everything here is
/// opt-in additive, and old API results are unchanged.

/// Reference coordination limits of the continuous-perception contract
/// (DEC-026 section 7, tentative until the M9-06 calibration). They cap the
/// contract's worst-case coordination memory: configuration may tighten them
/// per instance, never loosen them past these constants — a caller needing
/// more must build its own path. Element caps never replace the byte budget:
/// every owned string, vector, relation, token and event counts against
/// `ContinuousPerceptionOptions::max_coordination_bytes`.
inline constexpr int64_t kContinuousMaxCoordinationBytes = int64_t{8} * 1024 * 1024;
inline constexpr int32_t kContinuousMaxEntities = 1024;
inline constexpr int32_t kContinuousMaxRelations = 4096;
inline constexpr int32_t kContinuousMaxEvents = 256;
inline constexpr int32_t kContinuousMaxFocusConditions = 64;

/// Semantic payload freshness of one entity (DEC-026 section 3). Every
/// reusable result carries exactly one of these states; "not recomputed" is
/// never reported as "unchanged".
enum class Freshness : uint8_t {
    /// Produced by a recognition run in the current processing cycle.
    kFresh,
    /// Served from verified reuse: the reuse was proven by tracking validation
    /// plus a coverage-precise re-check (DEC-023 fine mode where text is
    /// involved), not by an unchanged fingerprint alone.
    kValidatedReuse,
    /// Carried over from an earlier cycle without re-validation in this one.
    /// Queryable, but explicitly not a claim that the content is unchanged.
    kUnvalidated,
    /// Invalidated by a layout switch, rotation/resolution change, model or
    /// parameter change, or a policy-scope change (DEC-026 section 3).
    kInvalidated,
};

/// Intermediate observation state of an entity the coordinator currently
/// cannot confirm (DEC-026 section 8 disappearance semantics). None of these
/// states is a confirmed disappearance: only the effective-coverage rule
/// (the entity's area stays validly covered while the target is missing)
/// advances an `end` event. Occlusion, unanalyzed coverage gaps and tracker
/// loss stay queryable through `TemporalEntityState`.
enum class ObservationState : uint8_t {
    /// Observed with valid coverage in the current cycle.
    kObserved,
    /// The entity's area is validly covered but the target is occluded by
    /// something else; the previous identity is retained as an observation.
    kOccluded,
    /// The tracker lost localization; no position claim is made until
    /// re-capture (DEC-010 identity rules govern re-capture).
    kTrackingLost,
    /// The entity's area was not analyzed this cycle (budget shortfall,
    /// truncated coverage or outside the focus scope); state unknown.
    kUnanalyzed,
};

/// Event lifecycle axis (DEC-026 section 8): appearance opens a lifecycle
/// (begin), confirmed content/relation changes update it (update), and only a
/// confirmed disappearance or a source reset closes it (end). Persistent
/// existence of the same entity never produces a repeated event.
enum class EventSemantic : uint8_t { kBegin, kUpdate, kEnd };

/// What changed, as the content axis of one event (DEC-026 sections 6/8).
/// The frozen pairing with `EventSemantic`: kAppeared pairs with kBegin;
/// kTextChanged/kAppearanceChanged/kRelationChanged pair with kUpdate;
/// kDisappeared and kSourceReset pair with kEnd.
enum class SemanticChangeKind : uint8_t {
    kAppeared,           ///< entity first confirmed (after the confirmation window)
    kTextChanged,        ///< confirmed text of an existing entity changed
    kAppearanceChanged,  ///< confirmed geometry/label/appearance class changed
    kRelationChanged,    ///< confirmed relation changed (occlusion, hierarchy, layout)
    kDisappeared,        ///< confirmed disappearance under the coverage rule
    kSourceReset,        ///< old-epoch lifecycle explicitly terminated by a source reset
};

/// Relation kind of one edge in the opt-in relation view (DEC-026 section 4).
/// Original evidence is never destroyed by any of these; they are a bounded
/// view beside the existing fusion output, not a replacement of it.
enum class RelationKind : uint8_t {
    /// Same-entity duplicate evidence: geometry, class/text compatibility and
    /// reliability all supported the merge. All `evidence_ids` and the merge
    /// trace are preserved on the kept entity.
    kDuplicateEvidence,
    /// Component containment ("text inside a button", "icon inside a
    /// container"). Parent and child both stay independently queryable.
    kPartOf,
    /// Plain spatial overlap without semantic compatibility: the regions stay
    /// independent. Containment alone never merges transitively.
    kOverlap,
    /// Same-entity geometry with contradicting type claims (e.g. an icon that
    /// OCR misreads as text): recorded as a conflict / low-reliability
    /// candidate; the original evidence of both sides survives.
    kConflict,
};

/// Outcome of the frozen per-pair relation rule (DEC-026 section 4).
enum class RelationClassification : uint8_t {
    kNone,       ///< no intersection: unrelated
    kDuplicate,  ///< same-entity duplicate evidence (merge candidate)
    kPartOf,     ///< component containment edge
    kOverlap,    ///< plain overlap, kept independent
    kConflict,   ///< same-entity geometry, contradicting claims
};

/// Why a work binding cannot be adopted (DEC-026 section 5). Every rejection
/// is visible with its reason and counted; a rejected submission never
/// touches the confirmed snapshot, generation or the complete cache.
enum class WorkRejectionReason : uint8_t {
    /// The token was issued by a different source identity than this
    /// coordinator tracks.
    kForeignSource,
    /// The token predates the current source epoch (window/device rebuild);
    /// old-epoch work can never be re-validated by reusing the same name.
    kStaleSourceEpoch,
    /// The policy revision changed since the token was planned.
    kStalePolicyRevision,
    /// The visual generation advanced past the token's generation (RULE-09).
    kStaleGeneration,
    /// A newer frame is current (or the token names a frame no plan was
    /// issued for): out-of-order, replaced or skipped input.
    kStaleFrameSequence,
    /// The same token was already submitted (double accept).
    kDuplicateSubmission,
    /// The bound ROI content changed since planning; the token must be
    /// re-planned, never silently shifted onto the new content.
    kContentMismatch,
};

/// Why a focus condition could not be resolved (DEC-026 section 2). An
/// unresolved focus is a valid, explicit outcome — the coordinator never
/// falls back to expensive full-frame recognition on its own and never
/// reuses stale coordinates for a moved or retired identity.
enum class FocusUnresolvedReason : uint8_t {
    /// The stable id is not tracked by the producing session.
    kUnknownStableId,
    /// The id exists but the bound generation is stale (RULE-09).
    kStaleGeneration,
    /// The id exists at the current generation but has no usable position
    /// (tracker lost localization).
    kMissingLocalization,
    /// A text/label condition matched nothing among existing trusted
    /// observations. Unseen text is never located by the matcher; seeing it
    /// requires a caller-budgeted bootstrap detection (DEC-026 section 2).
    kNotObserved,
};

/// Whether a plan proved complete coverage (DEC-026 section 3). kIncomplete
/// means budget or scope left parts of the frame unanalyzed: reuse guarantees
/// are suspended for those parts and "unchanged" claims are forbidden there.
enum class CoverageState : uint8_t { kComplete, kIncomplete };

/// Semantic payload used by relation decisions (DEC-026 section 4): the
/// trusted text observation and the detector label / external role of one
/// region. Empty strings mean "no such claim", not "known empty".
struct EntitySemantics {
    std::string text;
    std::string label;

    [[nodiscard]] friend bool operator==(const EntitySemantics& lhs, const EntitySemantics& rhs) noexcept {
        return lhs.text == rhs.text && lhs.label == rhs.label;
    }
};

/// One entity of the relation view: a fused region plus its provenance and
/// freshness (DEC-026 sections 3/4). `evidence_ids` keeps every contributing
/// evidence item in ascending order — a duplicate merge never drops evidence.
struct EntityRecord {
    uint64_t stable_id = 0;  ///< 0 while untracked (before stable-id assignment)
    uint64_t generation = 0;
    RectF bounds{};
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;
    uint32_t source_mask = 0;
    float confidence = 0.0F;
    Freshness freshness = Freshness::kUnvalidated;
    std::vector<uint64_t> evidence_ids;  ///< ascending, trace-preserving

    [[nodiscard]] friend bool operator==(const EntityRecord& lhs, const EntityRecord& rhs) noexcept {
        return lhs.stable_id == rhs.stable_id && lhs.generation == rhs.generation && lhs.bounds == rhs.bounds &&
               lhs.space == rhs.space && lhs.source_mask == rhs.source_mask && lhs.confidence == rhs.confidence &&
               lhs.freshness == rhs.freshness && lhs.evidence_ids == rhs.evidence_ids;
    }
};

/// One relation edge of the bounded view (DEC-026 section 4). kPartOf /
/// kOverlap / kConflict edges name two entities of the view (kPartOf:
/// first = child, second = parent). A kDuplicateEvidence record names the
/// kept entity in `first_entity` and lists the input region indexes that
/// merged into it in `merged_from`; the absorbed input is not an entity of
/// the view. The graph is acyclic by construction: part-of edges always run
/// from the smaller-area child to the larger-area parent.
struct RelationRecord {
    uint64_t relation_id = 0;  ///< 1-based, sequential within one view
    RelationKind kind = RelationKind::kOverlap;
    size_t first_entity = 0;   ///< index into RelationView::entities
    size_t second_entity = 0;  ///< index into RelationView::entities
    /// kDuplicateEvidence only: source snapshot region indexes merged into
    /// the kept entity, ascending. Empty for every other kind.
    std::vector<size_t> merged_from;
    double iou = 0.0;          ///< decision metrics, informational (DEC-026 trace)
    double containment = 0.0;  ///< intersection / min(area_a, area_b)

    [[nodiscard]] friend bool operator==(const RelationRecord& lhs, const RelationRecord& rhs) noexcept {
        return lhs.relation_id == rhs.relation_id && lhs.kind == rhs.kind && lhs.first_entity == rhs.first_entity &&
               lhs.second_entity == rhs.second_entity && lhs.merged_from == rhs.merged_from && lhs.iou == rhs.iou &&
               lhs.containment == rhs.containment;
    }
};

/// The bounded opt-in relation view beside a fused snapshot (DEC-026
/// section 4). Data model frozen at M9-01; built deterministically over a
/// `SemanticSnapshot` by `build_relation_view` (M9-02) with the fixed fusion
/// fixtures.
struct RelationView {
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;
    std::vector<EntityRecord> entities;
    std::vector<RelationRecord> relations;
    CoverageState coverage = CoverageState::kIncomplete;

    [[nodiscard]] friend bool operator==(const RelationView& lhs, const RelationView& rhs) noexcept {
        return lhs.space == rhs.space && lhs.entities == rhs.entities && lhs.relations == rhs.relations &&
               lhs.coverage == rhs.coverage;
    }
};

/// One attention condition over existing trusted observations (DEC-026
/// section 2). A condition carries a spatial ROI, a stable-id binding, or an
/// exact text/label match — at least one of them must be set. Matchers are
/// exact and bounded by policy caps; the first version has no regular
/// expressions or scripts, and unseen text is never located by matching.
struct AttentionCondition {
    /// Spatial condition: region of interest in `roi_space`. Kept as a
    /// condition (not a resolved focus) so the plan can rank work inside it.
    RectF roi{};
    CoordinateSpaceId roi_space = CoordinateSpaceId::kOriented;
    /// Identity condition: follow this stable id through the tracker's
    /// current position; `generation` must match (RULE-09). The ROI moves
    /// with the tracked entity — never with a memorized coordinate.
    bool bind_stable_id = false;
    uint64_t stable_id = 0;
    uint64_t generation = 0;
    /// Exact text condition over existing trusted observations ("" = unset).
    std::string text_equals;
    /// Exact label/role condition over existing trusted observations ("" = unset).
    std::string label_equals;

    [[nodiscard]] friend bool operator==(const AttentionCondition& lhs, const AttentionCondition& rhs) noexcept {
        return lhs.roi == rhs.roi && lhs.roi_space == rhs.roi_space && lhs.bind_stable_id == rhs.bind_stable_id &&
               lhs.stable_id == rhs.stable_id && lhs.generation == rhs.generation &&
               lhs.text_equals == rhs.text_equals && lhs.label_equals == rhs.label_equals;
    }
};

/// Declarative focus applied before expensive capabilities (DEC-026
/// section 2). Caller configuration: Mirador never infers task importance.
struct AttentionPolicy {
    /// At most `max_focus_conditions` conditions (configurable cap below
    /// kContinuousMaxFocusConditions; over-cap configs fail validation).
    std::vector<AttentionCondition> conditions;
    /// Tightened-only byte cap over all matcher text.
    int64_t max_condition_bytes = kContinuousMaxCoordinationBytes;

    [[nodiscard]] friend bool operator==(const AttentionPolicy& lhs, const AttentionPolicy& rhs) noexcept {
        return lhs.conditions == rhs.conditions && lhs.max_condition_bytes == rhs.max_condition_bytes;
    }
};

/// Importance and notification configuration (DEC-026 sections 2/6). Priority
/// orders caller work; it never multiplies into detection confidence. The
/// notification flag filters event publication only — internal confirmed
/// state and reuse state keep updating regardless (DEC-026 section 6).
struct ImportancePolicy {
    float priority = 0.0F;  ///< explicit caller priority for planned work
    /// Authorizes a bootstrap full-frame detection when a focus condition
    /// cannot be resolved from existing observations (DEC-026 section 2).
    bool allow_bootstrap_full_frame = false;
    /// Whether trusted changes may be published as events; filtering never
    /// blocks internal state evolution or stale-reference invalidation.
    bool notify_enabled = true;

    [[nodiscard]] friend bool operator==(const ImportancePolicy& lhs, const ImportancePolicy& rhs) noexcept {
        return lhs.priority == rhs.priority && lhs.allow_bootstrap_full_frame == rhs.allow_bootstrap_full_frame &&
               lhs.notify_enabled == rhs.notify_enabled;
    }
};

/// A successfully resolved focus: the tracked identity and its current
/// region (DEC-026 section 2). Coordinates live in the queried snapshot's
/// space; they are current-position facts, never remembered coordinates.
struct ResolvedFocus {
    uint64_t stable_id = 0;
    uint64_t generation = 0;
    RectF roi{};
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;

    [[nodiscard]] friend bool operator==(const ResolvedFocus& lhs, const ResolvedFocus& rhs) noexcept {
        return lhs.stable_id == rhs.stable_id && lhs.generation == rhs.generation && lhs.roi == rhs.roi &&
               lhs.space == rhs.space;
    }
};

/// Outcome of one focus resolution: either a resolved focus or one explicit
/// `FocusUnresolvedReason` (DEC-026 section 2). Unresolved is a valid result.
struct FocusResolution {
    bool resolved = false;
    FocusUnresolvedReason reason = FocusUnresolvedReason::kNotObserved;  ///< valid iff !resolved
    ResolvedFocus focus;                                                 ///< valid iff resolved

    [[nodiscard]] friend bool operator==(const FocusResolution& lhs, const FocusResolution& rhs) noexcept {
        return lhs.resolved == rhs.resolved && lhs.reason == rhs.reason && lhs.focus == rhs.focus;
    }
};

/// Kind of one planned work ROI (DEC-026 section 2).
enum class PlannedWorkKind : uint8_t {
    /// A resolved attention focus: spend expensive capability time here.
    kFocus,
    /// Caller-budgeted full-frame bootstrap for a text/label condition that
    /// matched nothing among existing observations (DEC-026 section 2:
    /// unseen text is never located by matching; the caller's importance
    /// policy authorizes the detection).
    kBootstrap,
};

/// One planned work ROI of the synchronous attention plan (DEC-026 sections
/// 1/2): where to spend expensive capability time and at what caller
/// priority. Maps directly onto caller priority entries for budgeted
/// backends (e.g. `OcrPriorityEntry`). Coordinates live in the planned
/// snapshot's space (conversions declared on `plan_attention`).
struct PlannedWork {
    PlannedWorkKind kind = PlannedWorkKind::kFocus;
    RectF roi{};
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;
    /// The `ImportancePolicy::priority` of the planning call; ordering never
    /// multiplies it into anything else (DEC-026 section 2).
    float priority = 0.0F;
    /// Identity binding of a resolved focus carrying one (stable-id, text
    /// or label resolution); both fields stay 0 for ROI-only conditions
    /// and bootstrap work.
    uint64_t stable_id = 0;
    uint64_t generation = 0;
    /// Index of the originating `AttentionPolicy` condition.
    size_t condition_index = 0;

    [[nodiscard]] friend bool operator==(const PlannedWork& lhs, const PlannedWork& rhs) noexcept {
        return lhs.kind == rhs.kind && lhs.roi == rhs.roi && lhs.space == rhs.space && lhs.priority == rhs.priority &&
               lhs.stable_id == rhs.stable_id && lhs.generation == rhs.generation &&
               lhs.condition_index == rhs.condition_index;
    }
};

/// Queryable peripheral structural-change record (DEC-026 section 2): a
/// changed region outside every resolved focus — the
/// focus-reconsideration / layout-invalidation hint. The record is a visual
/// fact; whether to notify or refresh is the caller's policy, and acting on
/// it (incremental coverage, notifications) is the M9-04/M9-05 wiring.
struct FocusReconsideration {
    RectF changed_region{};
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;
    /// True for a whole-frame layout-scale change (the snapshot's change
    /// report classified kGlobal); the region then covers the full frame.
    bool global = false;

    [[nodiscard]] friend bool operator==(const FocusReconsideration& lhs, const FocusReconsideration& rhs) noexcept {
        return lhs.changed_region == rhs.changed_region && lhs.space == rhs.space && lhs.global == rhs.global;
    }
};

/// The synchronous attention plan (DEC-026 sections 1/2): resolved focus
/// work with caller priorities, every condition's explicit resolution
/// outcome, and peripheral reconsideration records. Pure output — the
/// coordinator stores none of it, makes no coverage or reuse claim
/// (`coverage` stays kIncomplete; the M9-04 plan/accept binding owns
/// coverage proofs and work tokens), and never falls back to a full-frame
/// recognition on its own (bootstrap work appears only when the caller's
/// importance policy authorizes it).
struct AttentionPlan {
    /// The coordinator's policy revision the plan was produced under.
    uint64_t policy_revision = 0;
    /// Ordered planned work: kFocus items first, then kBootstrap; within a
    /// kind, priority descending, then condition index ascending.
    std::vector<PlannedWork> work;
    /// Per-condition resolution outcomes, parallel to the engaged policy's
    /// conditions (condition i -> resolutions[i]). A resolved ROI-only
    /// condition also yields a kFocus work item; a resolved id/text/label
    /// condition yields one with its identity echo.
    std::vector<FocusResolution> resolutions;
    /// Peripheral changed regions (snapshot change report) outside every
    /// resolved focus, in report order; kGlobal collapses to one full-frame
    /// record.
    std::vector<FocusReconsideration> reconsiderations;
    CoverageState coverage = CoverageState::kIncomplete;

    [[nodiscard]] friend bool operator==(const AttentionPlan& lhs, const AttentionPlan& rhs) noexcept {
        return lhs.policy_revision == rhs.policy_revision && lhs.work == rhs.work &&
               lhs.resolutions == rhs.resolutions && lhs.reconsiderations == rhs.reconsiderations &&
               lhs.coverage == rhs.coverage;
    }
};

/// The binding a plan issues and an accept consumes (DEC-026 section 5).
/// A token commits to the exact identity of the work: source, epoch, frame,
/// visual generation, policy revision, request/model identity, the planned
/// ROI and a digest of the ROI content it was planned against.
struct WorkToken {
    std::string source_id;
    uint64_t source_epoch = 0;
    uint64_t frame_sequence = 0;
    uint64_t generation = 0;
    uint64_t policy_revision = 0;
    uint64_t request_digest = 0;  ///< request/model/implementation identity
    RectF roi{};
    CoordinateSpaceId roi_space = CoordinateSpaceId::kOriented;
    uint64_t content_digest = 0;  ///< exact content digest of the bound ROI

    [[nodiscard]] friend bool operator==(const WorkToken& lhs, const WorkToken& rhs) noexcept {
        return lhs.source_id == rhs.source_id && lhs.source_epoch == rhs.source_epoch &&
               lhs.frame_sequence == rhs.frame_sequence && lhs.generation == rhs.generation &&
               lhs.policy_revision == rhs.policy_revision && lhs.request_digest == rhs.request_digest &&
               lhs.roi == rhs.roi && lhs.roi_space == rhs.roi_space && lhs.content_digest == rhs.content_digest;
    }
};

/// The coordinator identity a token is validated against (DEC-026
/// section 5): the current values of every bound field.
struct WorkBindingContext {
    std::string source_id;
    uint64_t source_epoch = 0;
    uint64_t frame_sequence = 0;
    uint64_t generation = 0;
    uint64_t policy_revision = 0;

    [[nodiscard]] friend bool operator==(const WorkBindingContext& lhs, const WorkBindingContext& rhs) noexcept {
        return lhs.source_id == rhs.source_id && lhs.source_epoch == rhs.source_epoch &&
               lhs.frame_sequence == rhs.frame_sequence && lhs.generation == rhs.generation &&
               lhs.policy_revision == rhs.policy_revision;
    }
};

/// Evidence provenance and time window of one event (DEC-026 section 8): the
/// observation that produced the evidence and the frame context in which the
/// change was confirmed. `observed_frame <= confirmed_frame` within one epoch.
struct ObservationWindow {
    std::string source_id;
    uint64_t source_epoch = 0;
    uint64_t observed_frame = 0;   ///< first observation backing the evidence
    uint64_t confirmed_frame = 0;  ///< frame at which the change was confirmed

    [[nodiscard]] friend bool operator==(const ObservationWindow& lhs, const ObservationWindow& rhs) noexcept {
        return lhs.source_id == rhs.source_id && lhs.source_epoch == rhs.source_epoch &&
               lhs.observed_frame == rhs.observed_frame && lhs.confirmed_frame == rhs.confirmed_frame;
    }
};

/// One structured semantic event (DEC-026 sections 6/8). Events are pulled,
/// never pushed: the coordinator exposes a bounded synchronous drain. Fields
/// frozen at M9-01 per the 2026-10-20 follow-up: provenance, instance
/// generation, evidence time window and begin/update/end semantics.
struct SemanticEvent {
    uint64_t event_sequence = 0;  ///< coordinator-wide, monotonic, gap-free
    EventSemantic semantic = EventSemantic::kBegin;
    SemanticChangeKind change_kind = SemanticChangeKind::kAppeared;
    uint64_t stable_id = 0;
    /// Lifetime generation of this stable-id instance (DEC-026 section 8):
    /// a re-appearing identity starts a new instance generation; 0 = unset.
    uint64_t instance_generation = 0;
    ObservationWindow window;
    RectF bounds{};  ///< current bounds at confirmation
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;

    [[nodiscard]] friend bool operator==(const SemanticEvent& lhs, const SemanticEvent& rhs) noexcept {
        return lhs.event_sequence == rhs.event_sequence && lhs.semantic == rhs.semantic &&
               lhs.change_kind == rhs.change_kind && lhs.stable_id == rhs.stable_id &&
               lhs.instance_generation == rhs.instance_generation && lhs.window == rhs.window &&
               lhs.bounds == rhs.bounds && lhs.space == rhs.space;
    }
};

/// Queryable per-entity temporal state (DEC-026 sections 6/8): the latest
/// observation, the confirmed state and their freshness stay available even
/// while notifications are filtered or a focus moved elsewhere. Data model
/// frozen at M9-01; the state machine producing it follows in M9-05.
struct TemporalEntityState {
    uint64_t stable_id = 0;
    uint64_t generation = 0;
    uint64_t instance_generation = 0;
    Freshness freshness = Freshness::kUnvalidated;
    ObservationState observation = ObservationState::kUnanalyzed;
    RectF bounds{};
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;
    uint64_t observed_frame = 0;   ///< latest frame with a valid observation
    uint64_t confirmed_frame = 0;  ///< frame at which the state was confirmed

    [[nodiscard]] friend bool operator==(const TemporalEntityState& lhs, const TemporalEntityState& rhs) noexcept {
        return lhs.stable_id == rhs.stable_id && lhs.generation == rhs.generation &&
               lhs.instance_generation == rhs.instance_generation && lhs.freshness == rhs.freshness &&
               lhs.observation == rhs.observation && lhs.bounds == rhs.bounds && lhs.space == rhs.space &&
               lhs.observed_frame == rhs.observed_frame && lhs.confirmed_frame == rhs.confirmed_frame;
    }
};

/// Tunables of the frozen per-pair relation rule (DEC-026 section 4).
/// Thresholds are inclusive development defaults pending the M9-06
/// calibration; the rule structure is frozen, the numbers are not.
struct RelationClassifyOptions {
    /// IoU at or above which two regions are duplicate-geometry candidates.
    double duplicate_iou = 0.80;
    /// Mutual containment (inter/min(area) in both directions) at or above
    /// which similar-size regions are duplicate-geometry candidates.
    double duplicate_containment = 0.90;
    /// Containment (inter/min(area)) at or above which a clearly smaller
    /// region inside a larger one forms a part-of candidate.
    double part_of_containment = 0.60;
    /// min/max(area) at or below which the contained region counts as
    /// "clearly smaller" (component), not as a similar-size duplicate.
    double part_of_max_size_ratio = 0.50;

    [[nodiscard]] friend bool operator==(const RelationClassifyOptions& lhs,
                                         const RelationClassifyOptions& rhs) noexcept {
        return lhs.duplicate_iou == rhs.duplicate_iou && lhs.duplicate_containment == rhs.duplicate_containment &&
               lhs.part_of_containment == rhs.part_of_containment &&
               lhs.part_of_max_size_ratio == rhs.part_of_max_size_ratio;
    }
};

/// Decision metrics of one classification, recorded on the relation edge for
/// explainability (DEC-026 section 4 trace).
struct RelationDecision {
    double iou = 0.0;          ///< intersection / union
    double containment = 0.0;  ///< intersection / min(area_a, area_b)
    double size_ratio = 0.0;   ///< min(area_a, area_b) / max(area_a, area_b)
};

/// Options of the deterministic relation-view builder (DEC-026 section 4).
/// Caps follow the tighten-only discipline of the coordination budgets: values
/// looser than the DEC-026 reference constants are rejected, never silently
/// widened.
struct RelationViewOptions {
    /// The frozen per-pair decision rule with its development-default
    /// thresholds (calibrated at M9-06; the rule structure is frozen).
    RelationClassifyOptions classify{};
    /// Reliability gate of duplicate merges (DEC-026 section 4): both regions'
    /// confidence must reach this floor for duplicate geometry to merge;
    /// otherwise duplicate geometry degrades to a plain overlap and both sides
    /// stay independent. Development default pending the M9-06 calibration.
    float min_merge_confidence = 0.5F;
    /// When set, a snapshot whose `generation` differs is rejected as a stale
    /// batch (kInvalidArgument): evidence from another visual generation must
    /// never mix into one view. Binding the coordinator's source epoch is the
    /// accept path's job (M9-04); this guard pins only the snapshot
    /// generation.
    std::optional<uint64_t> expected_generation;
    /// Tighten-only output caps (DEC-026 section 7).
    int32_t max_entities = kContinuousMaxEntities;
    int32_t max_relations = kContinuousMaxRelations;
    int64_t max_view_bytes = kContinuousMaxCoordinationBytes;

    [[nodiscard]] friend bool operator==(const RelationViewOptions& lhs, const RelationViewOptions& rhs) noexcept {
        return lhs.classify == rhs.classify && lhs.min_merge_confidence == rhs.min_merge_confidence &&
               lhs.expected_generation == rhs.expected_generation && lhs.max_entities == rhs.max_entities &&
               lhs.max_relations == rhs.max_relations && lhs.max_view_bytes == rhs.max_view_bytes;
    }
};

/// Classifies one pair of regions (DEC-026 section 4, frozen rule):
/// duplicate when duplicate-geometry holds and semantics and reliability
/// both support the merge; conflict when duplicate-geometry holds but the
/// claims contradict; part-of for a clearly smaller contained region with
/// compatible semantics; plain overlap for everything else with positive
/// intersection; none without intersection. Reliability only gates the
/// merge: without it, duplicate geometry degrades to plain overlap
/// (kept independent, nothing destroyed). Deterministic and symmetric in
/// `bounds_a`/`bounds_b` except that containment measures the smaller box.
/// Errors: kInvalidArgument for non-finite or negative-size bounds.
/// Never throws.
[[nodiscard]] Result<RelationClassification> classify_entity_relation(const RectF& bounds_a, const RectF& bounds_b,
                                                                      bool semantics_compatible,
                                                                      bool reliability_supported,
                                                                      const RelationClassifyOptions& options = {},
                                                                      RelationDecision* trace = nullptr) noexcept;

/// Class/text compatibility predicate of the relation rule (DEC-026
/// section 4): fully specified contradicting labels are incompatible, fully
/// specified contradicting texts are incompatible (exact comparison — the
/// first version has no fuzzy matching), otherwise neutral (compatible). An
/// exact match of both text and label is always compatible.
[[nodiscard]] bool entity_semantics_compatible(const EntitySemantics& a, const EntitySemantics& b) noexcept;

/// Structural invariants of a relation view: sequential relation ids from 1,
/// in-range entity indexes, part-of edges running child (smaller area) ->
/// parent (larger area), duplicate records carrying a non-empty
/// `merged_from` and no other kind doing so, ascending `merged_from`.
/// Diagnostics only — builders must construct valid views.
[[nodiscard]] bool relation_view_consistent(const RelationView& view) noexcept;

/// Bytes owned by a view's buffers (entity/relation vectors, evidence id
/// lists and merge traces), for budget enforcement (DEC-026 section 7).
/// Deterministic for a given standard library.
[[nodiscard]] int64_t relation_view_owned_bytes(const RelationView& view) noexcept;

/// Deterministically builds the bounded relation view beside a fused snapshot
/// (DEC-026 section 4, M9-02). Pure and snapshot-order-deterministic: the
/// same snapshot and options always produce an equal view; regions never
/// merge on stable ids alone.
///
/// Pipeline: validate options -> optional `expected_generation` guard ->
/// whole-batch geometry validation -> duplicate grouping -> entity emission ->
/// pairwise relation pass -> budget checks.
///
/// Duplicate grouping (star-shaped, no transitive chains): regions are scanned
/// in input order; the first unassigned region anchors a group, and a later
/// unassigned region joins only when `classify_entity_relation` on the two
/// anchor bounds returns kDuplicate — i.e. duplicate geometry plus compatible
/// anchor semantics (`entity_semantics_compatible` over the anchor's text and
/// label) plus the reliability gate (both confidences at or above
/// `min_merge_confidence`). A region duplicating an already-absorbed member
/// but not the anchor stays independent: absorption is never chained through
/// another absorbed region.
///
/// The kept entity of a group is its anchor (first member): anchor bounds,
/// anchor confidence and anchor semantics decide every later relation for it.
/// Aggregated from all members: `source_mask` is the bitwise OR,
/// `evidence_ids` the ascending duplicate-free union (nothing is dropped),
/// `stable_id` the smallest non-zero member id (0 when untracked),
/// `generation` and `space` come from the snapshot, `freshness` stays
/// kUnvalidated — the builder makes no coverage or validation claim. Absorbed
/// input indexes are listed ascending in `merged_from`.
///
/// Relation pass: every kept-entity pair is classified by the same frozen
/// rule on the anchor fields in (first_entity, second_entity) index order.
/// kDuplicate cannot occur there (any region duplicate with an anchor was
/// already absorbed; the pass replays exactly the phase-1 decisions).
/// kPartOf edges run child (smaller area) -> parent (larger area); overlap
/// and conflict keep both sides independent with their evidence preserved.
/// Every record carries the decision iou/containment (DEC-026 trace).
///
/// `coverage` stays kIncomplete: the pure builder never claims complete
/// coverage — the coordination layer (M9-04) owns coverage proofs (DEC-026
/// sections 2/3).
///
/// Errors: kInvalidArgument for a non-finite or out-of-[0,1]
/// `min_merge_confidence`, a set `expected_generation` differing from the
/// snapshot generation, or any region with non-finite/negative-size bounds —
/// the whole batch is rejected, never a partial view. kBudgetExceeded for
/// caps looser than the DEC-026 reference constants, more kept entities than
/// `max_entities`, more relations than `max_relations`, or a view owning more
/// than `max_view_bytes` bytes. kCancelled/kTimeout from `context` (polled
/// between scan stripes). Never throws.
[[nodiscard]] Result<RelationView> build_relation_view(const SemanticSnapshot& snapshot,
                                                       const RelationViewOptions& options = {},
                                                       const ExecutionContext& context = {}) noexcept;

/// Validates an attention policy (DEC-026 sections 2/7): at most
/// `kContinuousMaxFocusConditions` conditions, every condition carrying at
/// least one of ROI / stable-id binding / text or label match, finite
/// matcher bytes within `max_condition_bytes`, `max_condition_bytes` itself
/// positive and at most `kContinuousMaxCoordinationBytes`, stable-id
/// bindings carrying a non-zero id. Errors: kInvalidArgument (malformed
/// condition), kBudgetExceeded (cap or byte overflow). Never throws.
[[nodiscard]] Result<void> validate_attention_policy(const AttentionPolicy& policy) noexcept;

/// Stable digest of the policy's decision-relevant fields, used to detect
/// policy changes and derive the coordinator's policy revision (DEC-026
/// section 5). Deterministic across processes for equal inputs. Never throws.
[[nodiscard]] uint64_t attention_policy_digest(const AttentionPolicy& policy) noexcept;

/// Resolves one attention condition against a snapshot of existing trusted
/// observations (DEC-026 section 2, frozen rule). A condition names exactly
/// one focus: its criteria are evaluated in the frozen priority order
/// stable-id binding, then exact text, then exact label, then ROI — and the
/// highest-priority present criterion decides the outcome (no fall-through).
/// Stable-id resolution requires the bound generation to match the snapshot
/// (else kStaleGeneration) and the id to be present (else
/// kUnknownStableId); text/label resolution requires an exact match among
/// snapshot regions in their stored order (conjunction when both are set).
/// An unmatched condition reports kNotObserved — never a fabricated
/// location. ROI-only conditions resolve as declared, in the condition's
/// own space, without conversion. kMissingLocalization is reserved for the
/// coordinator's tracker-backed resolution (M9-03); the pure snapshot
/// lookup never produces it. Errors: kInvalidArgument for a
/// condition carrying no criterion, a non-finite ROI, or a stable-id
/// binding with a zero id. Never throws.
[[nodiscard]] Result<FocusResolution> resolve_attention_focus(const AttentionCondition& condition,
                                                              const SemanticSnapshot& snapshot) noexcept;

/// Structural validation of a work token: non-empty source id and finite,
/// non-negative-size ROI geometry. Identity staleness is checked by
/// `work_token_stale_reason`, stateful rejections (duplicate submission,
/// content mismatch) belong to the coordinator's accept (DEC-026
/// section 5). Errors: kInvalidArgument. Never throws.
[[nodiscard]] Result<void> validate_work_token(const WorkToken& token) noexcept;

/// Staleness of a token against a coordinator identity (DEC-026
/// section 5, frozen check order): foreign source, stale epoch, stale
/// policy revision, stale generation, then frame sequence. Equal fields are
/// current; nullopt means "not stale by identity" — duplicate-submission
/// and content-mismatch rejections need coordinator state and stay with
/// the accept path. Deterministic. Never throws.
[[nodiscard]] std::optional<WorkRejectionReason> work_token_stale_reason(const WorkToken& token,
                                                                         const WorkBindingContext& context) noexcept;

/// Structural invariants of one event (DEC-026 sections 6/8): positive
/// sequence, non-zero stable id, the frozen semantic/change-kind pairing
/// (begin=appeared, update=text/appearance/relation, end=disappeared or
/// source reset), a non-empty window with `observed_frame <= confirmed_frame`,
/// and zero bounds exactly for source-reset ends. Diagnostics only —
/// producers must construct valid events.
[[nodiscard]] bool semantic_event_consistent(const SemanticEvent& event) noexcept;

/// Bytes owned by one event (window source string), for the bounded queue
/// budget (DEC-026 sections 6/7). Deterministic for a given standard library.
[[nodiscard]] int64_t semantic_event_owned_bytes(const SemanticEvent& event) noexcept;

/// Configuration of one continuous-perception coordinator (DEC-026
/// sections 1/7). Defaults are the DEC-026 reference limits; every budget
/// may be tightened per instance and never loosened past the reference
/// constants. Non-positive budgets are rejected, not silently defaulted.
struct ContinuousPerceptionOptions {
    std::string source_id;  ///< identity of the tracked image source
    int64_t max_coordination_bytes = kContinuousMaxCoordinationBytes;
    int32_t max_entities = kContinuousMaxEntities;
    int32_t max_relations = kContinuousMaxRelations;
    int32_t max_events = kContinuousMaxEvents;
    int32_t max_focus_conditions = kContinuousMaxFocusConditions;

    [[nodiscard]] friend bool operator==(const ContinuousPerceptionOptions& lhs,
                                         const ContinuousPerceptionOptions& rhs) noexcept {
        return lhs.source_id == rhs.source_id && lhs.max_coordination_bytes == rhs.max_coordination_bytes &&
               lhs.max_entities == rhs.max_entities && lhs.max_relations == rhs.max_relations &&
               lhs.max_events == rhs.max_events && lhs.max_focus_conditions == rhs.max_focus_conditions;
    }
};

/// Report of one source reset (DEC-026 sections 1/8): the identity switch
/// callers must perform on window/device rebuild instead of reusing a
/// source id across generations.
struct SourceResetReport {
    uint64_t previous_epoch = 0;
    uint64_t new_epoch = 0;

    [[nodiscard]] friend bool operator==(const SourceResetReport& lhs, const SourceResetReport& rhs) noexcept {
        return lhs.previous_epoch == rhs.previous_epoch && lhs.new_epoch == rhs.new_epoch;
    }
};

/// Synchronous continuous-perception coordinator (DEC-026 section 1,
/// Experimental, name frozen at M9-01): composes the existing session,
/// tracking and cache primitives for one image source; it never re-implements
/// them in core. One coordinator belongs to one scheduling context and is
/// not thread-safe (AGENTS.md sync boundary); it creates no threads,
/// timers, executors or background work of its own.
///
/// M9-01 freezes the identity, budget and source-epoch core: creation
/// validation, the monotonic source epoch with explicit reset, and the
/// byte accounting of all owned coordination state. M9-02 adds the pure
/// relation-view builder (`build_relation_view`, a free function — the
/// coordinator holds no view state until the M9-04 wiring). M9-03 engages
/// the first coordination state: the validated attention policy with its
/// monotonic policy revision, an injected non-owning tracker for
/// tracker-backed focus resolution (id focus moves with the track; lost
/// localization is explicit), and the synchronous attention plan
/// (`plan_attention` — work ROIs with caller priorities, explicit
/// unresolved foci, peripheral reconsideration records; no coverage or
/// reuse claims). Plan / accept coverage and binding over relation views
/// (M9-04) and the temporal state / event surface (M9-05) follow in their
/// work items inside this same Experimental header, mirroring how
/// ObjectTracker grew its pipeline in M7-03..M7-08 and PerceptionSession
/// grew fuse() in M4.
class ContinuousPerception {
public:
    ContinuousPerception() noexcept = default;
    ContinuousPerception(const ContinuousPerception&) = delete;
    ContinuousPerception& operator=(const ContinuousPerception&) = delete;
    ContinuousPerception(ContinuousPerception&&) noexcept = default;
    ContinuousPerception& operator=(ContinuousPerception&&) noexcept = default;
    ~ContinuousPerception() noexcept = default;

    /// Creates a coordinator with the given options. Errors: kInvalidArgument
    /// for a budget that is non-positive or looser than the DEC-026 reference
    /// constants. Never throws.
    [[nodiscard]] static Result<ContinuousPerception> create(ContinuousPerceptionOptions options) noexcept;

    [[nodiscard]] const std::string& source_id() const noexcept { return options_.source_id; }
    [[nodiscard]] const ContinuousPerceptionOptions& options() const noexcept { return options_; }
    /// Current source epoch; starts at 1 for a fresh coordinator (DEC-026
    /// section 1: work tokens bind the epoch they were planned under).
    [[nodiscard]] uint64_t source_epoch() const noexcept { return epoch_; }

    /// Declares a source rebuild (window/device recreation, DEC-026
    /// sections 1/8): bumps the epoch. Work planned under the previous
    /// epoch becomes permanently rejectable with kStaleSourceEpoch; the
    /// old generation's unclosed events are terminated with kSourceReset
    /// end events by the M9-05 event surface. Never throws.
    [[nodiscard]] SourceResetReport notify_source_reset() noexcept;

    /// Engages the attention policy (DEC-026 section 2, M9-03): validates it
    /// (frozen `validate_attention_policy` errors; additionally every
    /// condition ROI space must be kFrame or kOriented — kDisplay condition
    /// ROIs have no planner-side conversion in M9-03), stores it and bumps
    /// the policy revision exactly when the `attention_policy_digest`
    /// changes; re-setting the identical policy is idempotent. The stored
    /// policy counts against `max_coordination_bytes` (DEC-026 section 7) —
    /// an engagement that would overflow the budget is rejected with
    /// kBudgetExceeded and keeps the previous policy and revision. Never
    /// throws.
    [[nodiscard]] Result<void> set_attention_policy(const AttentionPolicy& policy) noexcept;

    /// The engaged attention policy. Precondition: `has_attention_policy()`
    /// (asserted; calling before the first `set_attention_policy` is a
    /// contract violation). Never throws.
    [[nodiscard]] const AttentionPolicy& attention_policy() const noexcept {
        assert(has_attention_policy() && "attention_policy requires an engaged policy");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): precondition asserted above
        return *policy_;
    }

    /// True when a policy is engaged. Never throws.
    [[nodiscard]] bool has_attention_policy() const noexcept { return policy_.has_value(); }

    /// Policy revision of the frozen staleness order (DEC-026 section 5):
    /// 0 while no policy is engaged, then monotonic +1 per engaged digest
    /// change. Work tokens (M9-04) bind it. Never throws.
    [[nodiscard]] uint64_t policy_revision() const noexcept { return policy_revision_; }

    /// Injects the non-owning tracker backing tracker-backed focus
    /// resolution (DEC-026 section 2: the id focus maps to the track's
    /// current position, never a memorized coordinate). The caller owns the
    /// tracker and must keep it alive while attached and drive the
    /// coordinator from the same scheduling context (sync boundary);
    /// nullptr detaches. Never throws.
    void attach_tracker(const ObjectTracker* tracker) noexcept { tracker_ = tracker; }

    /// The attached tracker, or nullptr. Never throws.
    [[nodiscard]] const ObjectTracker* tracker() const noexcept { return tracker_; }

    /// Resolves one attention condition against the snapshot with the
    /// tracker fallback (DEC-026 section 2, M9-03). The frozen snapshot rule
    /// (`resolve_attention_focus`) decides first, unchanged; only a
    /// stable-id condition that found no snapshot region consults the
    /// attached tracker: a kTracking or kUncertain track resolves to its
    /// current pool position (`TargetTrack::last_bounds`, reported in
    /// kOriented — the pool's presented-view space) with the snapshot
    /// generation echo, a kLost track reports kMissingLocalization, and an
    /// absent or kTerminated track stays kUnknownStableId. Text, label and
    /// ROI-only conditions never consult the tracker. With no tracker
    /// attached the outcome equals the frozen rule. Errors: the frozen
    /// rule's kInvalidArgument cases. Never throws.
    [[nodiscard]] Result<FocusResolution> resolve_focus(const AttentionCondition& condition,
                                                        const SemanticSnapshot& snapshot) const noexcept;

    /// Produces the synchronous attention plan for one processed frame
    /// (DEC-026 sections 1/2, M9-03). Deterministic, pure output: for every
    /// engaged policy condition, `resolve_focus` decides (recorded in
    /// `resolutions`, parallel to the conditions); resolved conditions
    /// become kFocus work at `ImportancePolicy::priority` — snapshot-backed
    /// ROIs as-is, tracker-backed ROIs converted from kOriented into the
    /// snapshot space, ROI-only conditions converted from their declared
    /// space (the frame's rotation carries kFrame <-> kOriented). A
    /// condition unresolved with kNotObserved additionally yields one
    /// full-frame kBootstrap work item when
    /// `ImportancePolicy::allow_bootstrap_full_frame` authorizes it.
    /// Peripheral monitoring: the snapshot's change report drives
    /// `reconsiderations` — a kGlobal change collapses to one full-frame
    /// record (global), each kPartial change ROI is converted into the
    /// snapshot space and recorded when it intersects no resolved focus
    /// ROI (the plan never acts on them — coverage, notifications and
    /// invalidation are the M9-04/M9-05 wiring). `coverage` stays
    /// kIncomplete and nothing is stored: the plan makes no coverage or
    /// reuse claim (DEC-026 sections 2/3).
    ///
    /// `frame` must be the frame the snapshot was fused from: its rotation
    /// and dimensions drive every kFrame <-> kOriented conversion and the
    /// full-frame ROIs. `frame` is read for geometry only — never for
    /// pixels.
    ///
    /// Ordering: kFocus work before kBootstrap; within a kind, priority
    /// descending, then condition index ascending.
    ///
    /// Errors: kInvalidArgument for a snapshot in kDisplay space (no
    /// planner-side display conversion in M9-03), a frame with
    /// non-positive or non-finite dimensions, or a non-finite
    /// `ImportancePolicy::priority`; kCancelled/kTimeout from `context`.
    /// Never throws.
    [[nodiscard]] Result<AttentionPlan> plan_attention(const SemanticSnapshot& snapshot, const Frame& frame,
                                                       const ImportancePolicy& importance = {},
                                                       const ExecutionContext& context = {}) const noexcept;

    /// Bytes owned by all coordination state — the engaged attention
    /// policy's buffers (conditions and matcher text) since M9-03, later the
    /// M9-04 tokens/entities and the M9-05 event queue — the accounting
    /// enforced against `max_coordination_bytes` (DEC-026 section 7). An
    /// unset policy owns nothing. Deterministic for a given standard
    /// library.
    [[nodiscard]] int64_t coordination_owned_bytes() const noexcept;

private:
    explicit ContinuousPerception(ContinuousPerceptionOptions options) noexcept : options_(std::move(options)) {}

    ContinuousPerceptionOptions options_;
    uint64_t epoch_ = 1;
    /// M9-03 coordination state: the engaged attention policy with its
    /// monotonic revision, and the injected non-owning tracker. The policy
    /// counts against `max_coordination_bytes` (DEC-026 section 7); an
    /// unset policy owns nothing.
    std::optional<AttentionPolicy> policy_;
    uint64_t policy_revision_ = 0;
    const ObjectTracker* tracker_ = nullptr;
};

}  // namespace mirador

#endif  // MIRADOR_CONTINUOUS_PERCEPTION_HPP
