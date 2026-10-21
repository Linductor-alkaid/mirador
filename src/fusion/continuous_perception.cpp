#include <mirador/continuous_perception.hpp>

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>

#include "rect_math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mirador {
namespace {

[[nodiscard]] bool finite_rect(const RectF& rect) noexcept {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height);
}

[[nodiscard]] bool usable_rect(const RectF& rect) noexcept {
    return finite_rect(rect) && rect.width >= 0.0F && rect.height >= 0.0F;
}

/// FNV-1a over raw bytes; the digest primitives below feed fixed-width field
/// encodings (and length-prefixed strings) so equal inputs always agree and
/// different inputs never collide by alignment. Deterministic across
/// processes; not a cryptographic hash.
[[nodiscard]] uint64_t digest_bytes(uint64_t hash, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

[[nodiscard]] uint64_t digest_u64(uint64_t hash, uint64_t value) noexcept {
    return digest_bytes(hash, &value, sizeof(value));
}

[[nodiscard]] uint64_t digest_f32(uint64_t hash, float value) noexcept {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return digest_bytes(hash, &bits, sizeof(bits));
}

[[nodiscard]] uint64_t digest_string(uint64_t hash, const std::string& text) noexcept {
    hash = digest_u64(hash, static_cast<uint64_t>(text.size()));
    return digest_bytes(hash, text.data(), text.size());
}

/// Stage-boundary cancellation/deadline check (same contract as fuse_evidence).
[[nodiscard]] Status context_status(const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "build_relation_view cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "build_relation_view deadline exceeded"};
    }
    return {};
}

}  // namespace

Result<RelationClassification> classify_entity_relation(const RectF& bounds_a, const RectF& bounds_b,
                                                        bool semantics_compatible, bool reliability_supported,
                                                        const RelationClassifyOptions& options,
                                                        RelationDecision* trace) noexcept {
    RelationDecision decision{};
    if (!usable_rect(bounds_a) || !usable_rect(bounds_b)) {
        return Result<RelationClassification>{
            Status(ErrorCode::kInvalidArgument, "non-finite or negative-size bounds")};
    }

    const double area_a = fusion_internal::rect_area(bounds_a);
    const double area_b = fusion_internal::rect_area(bounds_b);
    const double intersection = fusion_internal::intersection_area(bounds_a, bounds_b);
    const double iou = fusion_internal::rect_iou(bounds_a, bounds_b);
    const double smaller_area = std::min(area_a, area_b);
    const double larger_area = std::max(area_a, area_b);
    const double containment = smaller_area > 0.0 ? intersection / smaller_area : 0.0;
    const double size_ratio = larger_area > 0.0 ? smaller_area / larger_area : 0.0;
    decision.iou = iou;
    decision.containment = containment;
    decision.size_ratio = size_ratio;
    if (trace != nullptr) {
        *trace = decision;
    }

    if (intersection <= 0.0) {
        return Result<RelationClassification>{RelationClassification::kNone};
    }

    // Duplicate geometry: near-identical boxes by IoU, or near-total
    // containment of similar-size boxes (the containment measure alone would
    // also fire for a small component inside a container, so the size ratio
    // separates the two shapes).
    const bool duplicate_geometry = iou >= options.duplicate_iou || (containment >= options.duplicate_containment &&
                                                                     size_ratio > options.part_of_max_size_ratio);
    // Component geometry: a clearly smaller region swallowed by a larger one.
    const bool part_of_geometry =
        containment >= options.part_of_containment && size_ratio <= options.part_of_max_size_ratio;

    if (duplicate_geometry) {
        if (!semantics_compatible) {
            // Same-entity geometry with contradicting claims: record the
            // conflict, destroy nothing (DEC-026 section 4).
            return Result<RelationClassification>{RelationClassification::kConflict};
        }
        if (reliability_supported) {
            return Result<RelationClassification>{RelationClassification::kDuplicate};
        }
        // Compatible but not reliable enough to merge: keep independent.
        return Result<RelationClassification>{RelationClassification::kOverlap};
    }
    if (part_of_geometry && semantics_compatible) {
        return Result<RelationClassification>{RelationClassification::kPartOf};
    }
    return Result<RelationClassification>{RelationClassification::kOverlap};
}

bool entity_semantics_compatible(const EntitySemantics& a, const EntitySemantics& b) noexcept {
    if (!a.label.empty() && !b.label.empty() && a.label != b.label) {
        return false;
    }
    if (!a.text.empty() && !b.text.empty() && a.text != b.text) {
        return false;
    }
    return true;
}

bool relation_view_consistent(const RelationView& view) noexcept {
    for (size_t index = 0; index < view.relations.size(); ++index) {
        const RelationRecord& relation = view.relations[index];
        if (relation.relation_id != index + 1) {
            return false;
        }
        if (relation.first_entity >= view.entities.size() || relation.second_entity >= view.entities.size()) {
            return false;
        }
        switch (relation.kind) {
            case RelationKind::kDuplicateEvidence:
                if (relation.merged_from.empty()) {
                    return false;
                }
                break;
            case RelationKind::kPartOf:
                if (!relation.merged_from.empty() || relation.first_entity == relation.second_entity) {
                    return false;
                }
                // Acyclic by construction: the child is the smaller area.
                if (fusion_internal::rect_area(view.entities[relation.first_entity].bounds) >
                    fusion_internal::rect_area(view.entities[relation.second_entity].bounds)) {
                    return false;
                }
                break;
            case RelationKind::kOverlap:
            case RelationKind::kConflict:
                if (!relation.merged_from.empty() || relation.first_entity == relation.second_entity) {
                    return false;
                }
                break;
        }
        if (!std::is_sorted(relation.merged_from.begin(), relation.merged_from.end())) {
            return false;
        }
    }
    return true;
}

int64_t relation_view_owned_bytes(const RelationView& view) noexcept {
    auto bytes = static_cast<int64_t>(sizeof(RelationView));
    for (const EntityRecord& entity : view.entities) {
        bytes += static_cast<int64_t>(sizeof(EntityRecord));
        bytes += static_cast<int64_t>(entity.evidence_ids.capacity() * sizeof(uint64_t));
    }
    for (const RelationRecord& relation : view.relations) {
        bytes += static_cast<int64_t>(sizeof(RelationRecord));
        bytes += static_cast<int64_t>(relation.merged_from.capacity() * sizeof(size_t));
    }
    return bytes;
}

/// Options, generation-binding and whole-batch geometry validation of the
/// relation-view builder (DEC-026 sections 4/7). A single illegal region
/// rejects the snapshot instead of producing a partial view (DEC-021
/// discipline).
[[nodiscard]] Status validate_relation_view_input(const RelationViewOptions& options,
                                                  const SemanticSnapshot& snapshot) noexcept {
    if (options.max_entities <= 0 || options.max_relations <= 0 || options.max_view_bytes <= 0) {
        return {ErrorCode::kInvalidArgument, "relation-view budgets must be positive"};
    }
    if (options.max_entities > kContinuousMaxEntities || options.max_relations > kContinuousMaxRelations ||
        options.max_view_bytes > kContinuousMaxCoordinationBytes) {
        return {ErrorCode::kBudgetExceeded, "relation-view caps looser than DEC-026"};
    }
    if (!std::isfinite(options.min_merge_confidence) || options.min_merge_confidence < 0.0F ||
        options.min_merge_confidence > 1.0F) {
        return {ErrorCode::kInvalidArgument, "min_merge_confidence must be finite in [0, 1]"};
    }
    if (options.expected_generation.has_value() && *options.expected_generation != snapshot.generation) {
        return {ErrorCode::kInvalidArgument, "snapshot generation differs from the bound generation"};
    }
    for (const VisualRegion& region : snapshot.regions) {
        if (!usable_rect(region.bounds)) {
            return {ErrorCode::kInvalidArgument, "non-finite or negative-size region bounds"};
        }
    }
    return {};
}

/// Phase 1 — star-shaped duplicate grouping (DEC-026 section 4): anchors are
/// first-seen regions; a region joins an anchor only on a direct kDuplicate
/// decision against the anchor, never chained through another absorbed member
/// and never on stable ids alone. Each group lists its anchor first, then the
/// absorbed input indexes ascending.
[[nodiscard]] Status group_duplicates(const SemanticSnapshot& snapshot, const RelationViewOptions& options,
                                      const ExecutionContext& context,
                                      std::vector<std::vector<size_t>>* members) noexcept {
    const size_t region_count = snapshot.regions.size();
    const size_t unassigned = region_count;
    std::vector<size_t> anchor_of(region_count, unassigned);
    for (size_t i = 0; i < region_count; ++i) {
        if (anchor_of[i] != unassigned) {
            continue;
        }
        anchor_of[i] = i;
        members->emplace_back(1, i);
        const VisualRegion& anchor = snapshot.regions[i];
        const EntitySemantics anchor_semantics{anchor.text, anchor.label};
        for (size_t j = i + 1; j < region_count; ++j) {
            if ((j & 63) == 0) {
                if (const Status stage = context_status(context); !stage.ok()) {
                    return stage;
                }
            }
            if (anchor_of[j] != unassigned) {
                continue;
            }
            const VisualRegion& region = snapshot.regions[j];
            const bool reliable =
                anchor.confidence >= options.min_merge_confidence && region.confidence >= options.min_merge_confidence;
            const bool compatible =
                entity_semantics_compatible(anchor_semantics, EntitySemantics{region.text, region.label});
            const RelationClassification decision =
                classify_entity_relation(anchor.bounds, region.bounds, compatible, reliable, options.classify)
                    // Unreachable fallback: every bounds pair passed the
                    // whole-batch validation above.
                    .value_or(RelationClassification::kNone);
            if (decision == RelationClassification::kDuplicate) {
                anchor_of[j] = i;
                members->back().push_back(j);
            }
        }
    }
    return {};
}

/// Kept entity per group: the anchor's geometry, confidence and semantics
/// decide every later relation; identity, sources and evidence aggregate over
/// all members so nothing is dropped (DEC-026 section 4 trace).
void emit_kept_entities(const SemanticSnapshot& snapshot, const std::vector<std::vector<size_t>>& members,
                        std::vector<size_t>* anchor_index_of_entity, RelationView* view) noexcept {
    view->entities.reserve(members.size());
    anchor_index_of_entity->reserve(members.size());
    std::vector<uint64_t> evidence_scratch;
    for (const std::vector<size_t>& group : members) {
        const VisualRegion& anchor = snapshot.regions[group.front()];
        EntityRecord record;
        record.generation = snapshot.generation;
        record.bounds = anchor.bounds;
        record.space = snapshot.coordinate_space;
        record.confidence = anchor.confidence;
        record.freshness = Freshness::kUnvalidated;
        evidence_scratch.clear();
        for (const size_t index : group) {
            const VisualRegion& member = snapshot.regions[index];
            if (member.stable_id != 0 && (record.stable_id == 0 || member.stable_id < record.stable_id)) {
                record.stable_id = member.stable_id;
            }
            record.source_mask |= member.source_mask;
            evidence_scratch.insert(evidence_scratch.end(), member.evidence_ids.begin(), member.evidence_ids.end());
        }
        std::sort(evidence_scratch.begin(), evidence_scratch.end());
        evidence_scratch.erase(std::unique(evidence_scratch.begin(), evidence_scratch.end()), evidence_scratch.end());
        record.evidence_ids = evidence_scratch;
        view->entities.push_back(std::move(record));
        anchor_index_of_entity->push_back(group.front());
    }
}

/// Duplicate merges as records of the frozen self-referential shape
/// (first_entity == second_entity == kept entity, absorbed inputs in
/// merged_from), emitted in kept-entity order before the pairwise pass.
[[nodiscard]] Status append_duplicate_records(const std::vector<std::vector<size_t>>& members,
                                              const RelationViewOptions& options, RelationView* view) noexcept {
    for (size_t entity_index = 0; entity_index < members.size(); ++entity_index) {
        const std::vector<size_t>& group = members[entity_index];
        if (group.size() < 2) {
            continue;
        }
        if (static_cast<int32_t>(view->relations.size()) >= options.max_relations) {
            return {ErrorCode::kBudgetExceeded, "relation view exceeds max_relations"};
        }
        RelationRecord record;
        record.relation_id = view->relations.size() + 1;
        record.kind = RelationKind::kDuplicateEvidence;
        record.first_entity = entity_index;
        record.second_entity = entity_index;
        record.merged_from.assign(group.begin() + 1, group.end());
        view->relations.push_back(std::move(record));
    }
    return {};
}

/// Edge kind of one pairwise classification; empty for the two non-edge
/// outcomes (kNone, and the phase-1-invariant kDuplicate that must never
/// become an edge because it would violate the view's data model).
[[nodiscard]] std::optional<RelationKind> relation_edge_kind(RelationClassification classification) noexcept {
    switch (classification) {
        case RelationClassification::kPartOf:
            return RelationKind::kPartOf;
        case RelationClassification::kOverlap:
            return RelationKind::kOverlap;
        case RelationClassification::kConflict:
            return RelationKind::kConflict;
        case RelationClassification::kNone:
        case RelationClassification::kDuplicate:
            return std::nullopt;
    }
    return std::nullopt;
}

/// Phase 2 — pairwise relations between kept entities (DEC-026 section 4).
/// The pass replays exactly the phase-1 decisions (anchor bounds, semantics
/// and reliability), so kDuplicate cannot occur here: every region duplicate
/// with an anchor was already absorbed in phase 1. Part-of edges run child
/// (smaller area) -> parent (larger area), keeping the view acyclic by
/// construction.
[[nodiscard]] Status append_pair_relations(const SemanticSnapshot& snapshot,
                                           const std::vector<size_t>& anchor_index_of_entity,
                                           const RelationViewOptions& options, const ExecutionContext& context,
                                           RelationView* view) noexcept {
    for (size_t p = 0; p < view->entities.size(); ++p) {
        if ((p & 63) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        const VisualRegion& anchor_p = snapshot.regions[anchor_index_of_entity[p]];
        const EntitySemantics semantics_p{anchor_p.text, anchor_p.label};
        for (size_t q = p + 1; q < view->entities.size(); ++q) {
            const VisualRegion& anchor_q = snapshot.regions[anchor_index_of_entity[q]];
            const bool reliable = anchor_p.confidence >= options.min_merge_confidence &&
                                  anchor_q.confidence >= options.min_merge_confidence;
            RelationDecision decision{};
            const RelationClassification classification =
                classify_entity_relation(
                    view->entities[p].bounds, view->entities[q].bounds,
                    entity_semantics_compatible(semantics_p, EntitySemantics{anchor_q.text, anchor_q.label}), reliable,
                    options.classify, &decision)
                    .value_or(RelationClassification::kNone);
            const std::optional<RelationKind> kind = relation_edge_kind(classification);
            if (!kind.has_value()) {
                continue;
            }
            RelationRecord record;
            record.relation_id = view->relations.size() + 1;
            record.kind = *kind;
            // Equal areas cannot classify as part-of (size ratio gate), so
            // the child is always the strictly smaller side here.
            if (*kind == RelationKind::kPartOf && fusion_internal::rect_area(view->entities[q].bounds) <
                                                      fusion_internal::rect_area(view->entities[p].bounds)) {
                record.first_entity = q;
                record.second_entity = p;
            } else {
                record.first_entity = p;
                record.second_entity = q;
            }
            record.iou = decision.iou;
            record.containment = decision.containment;
            if (static_cast<int32_t>(view->relations.size()) >= options.max_relations) {
                return {ErrorCode::kBudgetExceeded, "relation view exceeds max_relations"};
            }
            view->relations.push_back(std::move(record));
        }
    }
    return {};
}

Result<RelationView> build_relation_view(const SemanticSnapshot& snapshot, const RelationViewOptions& options,
                                         const ExecutionContext& context) noexcept {
    if (const Status validated = validate_relation_view_input(options, snapshot); !validated.ok()) {
        return Result<RelationView>{validated};
    }

    std::vector<std::vector<size_t>> members;  // per group: anchor first, then absorbed ascending
    if (const Status grouped = group_duplicates(snapshot, options, context, &members); !grouped.ok()) {
        return Result<RelationView>{grouped};
    }
    if (members.size() > static_cast<size_t>(options.max_entities)) {
        return Result<RelationView>{Status(ErrorCode::kBudgetExceeded, "relation view exceeds max_entities")};
    }

    RelationView view;
    view.space = snapshot.coordinate_space;
    view.coverage = CoverageState::kIncomplete;
    std::vector<size_t> anchor_index_of_entity;
    emit_kept_entities(snapshot, members, &anchor_index_of_entity, &view);
    if (const Status duplicates = append_duplicate_records(members, options, &view); !duplicates.ok()) {
        return Result<RelationView>{duplicates};
    }
    if (const Status paired = append_pair_relations(snapshot, anchor_index_of_entity, options, context, &view);
        !paired.ok()) {
        return Result<RelationView>{paired};
    }

    if (relation_view_owned_bytes(view) > options.max_view_bytes) {
        return Result<RelationView>{Status(ErrorCode::kBudgetExceeded, "relation view exceeds max_view_bytes")};
    }
    return Result<RelationView>{std::move(view)};
}

Result<void> validate_attention_policy(const AttentionPolicy& policy) noexcept {
    if (static_cast<int32_t>(policy.conditions.size()) > kContinuousMaxFocusConditions) {
        return Result<void>{Status(ErrorCode::kBudgetExceeded, "attention policy exceeds the focus condition cap")};
    }
    if (policy.max_condition_bytes <= 0) {
        return Result<void>{Status(ErrorCode::kInvalidArgument, "max_condition_bytes must be positive")};
    }
    if (policy.max_condition_bytes > kContinuousMaxCoordinationBytes) {
        return Result<void>{Status(ErrorCode::kBudgetExceeded, "max_condition_bytes looser than the reference cap")};
    }

    int64_t matcher_bytes = 0;
    for (const AttentionCondition& condition : policy.conditions) {
        const bool has_roi = condition.roi.width > 0.0F && condition.roi.height > 0.0F;
        const bool has_id = condition.bind_stable_id;
        const bool has_text = !condition.text_equals.empty();
        const bool has_label = !condition.label_equals.empty();
        if (!has_roi && !has_id && !has_text && !has_label) {
            return Result<void>{Status(ErrorCode::kInvalidArgument, "attention condition carries no criterion")};
        }
        if (!usable_rect(condition.roi)) {
            return Result<void>{Status(ErrorCode::kInvalidArgument, "non-finite or negative-size condition ROI")};
        }
        if (has_id && condition.stable_id == 0) {
            return Result<void>{Status(ErrorCode::kInvalidArgument, "stable-id binding requires a non-zero id")};
        }
        matcher_bytes += static_cast<int64_t>(condition.text_equals.size());
        matcher_bytes += static_cast<int64_t>(condition.label_equals.size());
    }
    if (matcher_bytes > policy.max_condition_bytes) {
        return Result<void>{Status(ErrorCode::kBudgetExceeded, "matcher text exceeds max_condition_bytes")};
    }
    return Result<void>{};
}

uint64_t attention_policy_digest(const AttentionPolicy& policy) noexcept {
    uint64_t hash = 0xcbf29ce484222325ULL;
    hash = digest_u64(hash, static_cast<uint64_t>(policy.conditions.size()));
    hash = digest_u64(hash, static_cast<uint64_t>(policy.max_condition_bytes));
    for (const AttentionCondition& condition : policy.conditions) {
        hash = digest_f32(hash, condition.roi.x);
        hash = digest_f32(hash, condition.roi.y);
        hash = digest_f32(hash, condition.roi.width);
        hash = digest_f32(hash, condition.roi.height);
        hash = digest_u64(hash, static_cast<uint64_t>(condition.roi_space));
        hash = digest_u64(hash, condition.bind_stable_id ? 1ULL : 0ULL);
        hash = digest_u64(hash, condition.stable_id);
        hash = digest_u64(hash, condition.generation);
        hash = digest_string(hash, condition.text_equals);
        hash = digest_string(hash, condition.label_equals);
    }
    return hash;
}

Result<FocusResolution> resolve_attention_focus(const AttentionCondition& condition,
                                                const SemanticSnapshot& snapshot) noexcept {
    const bool has_roi = condition.roi.width > 0.0F && condition.roi.height > 0.0F;
    const bool has_text = !condition.text_equals.empty();
    const bool has_label = !condition.label_equals.empty();
    if (!condition.bind_stable_id && !has_roi && !has_text && !has_label) {
        return Result<FocusResolution>{Status(ErrorCode::kInvalidArgument, "attention condition carries no criterion")};
    }
    if (!usable_rect(condition.roi)) {
        return Result<FocusResolution>{
            Status(ErrorCode::kInvalidArgument, "non-finite or negative-size condition ROI")};
    }
    if (condition.bind_stable_id && condition.stable_id == 0) {
        return Result<FocusResolution>{Status(ErrorCode::kInvalidArgument, "stable-id binding requires a non-zero id")};
    }

    // Fixed criterion priority, no fall-through: a condition names one focus,
    // and the highest-priority present criterion decides the outcome.
    if (condition.bind_stable_id) {
        if (snapshot.generation != condition.generation) {
            return Result<FocusResolution>{FocusResolution{false, FocusUnresolvedReason::kStaleGeneration, {}}};
        }
        for (const VisualRegion& region : snapshot.regions) {
            if (region.stable_id == condition.stable_id) {
                ResolvedFocus focus;
                focus.stable_id = region.stable_id;
                focus.generation = snapshot.generation;
                focus.roi = region.bounds;
                focus.space = snapshot.coordinate_space;
                return Result<FocusResolution>{FocusResolution{true, FocusUnresolvedReason::kNotObserved, focus}};
            }
        }
        return Result<FocusResolution>{FocusResolution{false, FocusUnresolvedReason::kUnknownStableId, {}}};
    }
    if (has_text || has_label) {
        for (const VisualRegion& region : snapshot.regions) {
            const bool text_ok = !has_text || region.text == condition.text_equals;
            const bool label_ok = !has_label || region.label == condition.label_equals;
            if (text_ok && label_ok) {
                ResolvedFocus focus;
                focus.stable_id = region.stable_id;
                focus.generation = snapshot.generation;
                focus.roi = region.bounds;
                focus.space = snapshot.coordinate_space;
                return Result<FocusResolution>{FocusResolution{true, FocusUnresolvedReason::kNotObserved, focus}};
            }
        }
        return Result<FocusResolution>{FocusResolution{false, FocusUnresolvedReason::kNotObserved, {}}};
    }

    // ROI-only condition: resolves as declared, in its own space, without
    // conversion — the plan converts with the frame available (DEC-016).
    ResolvedFocus focus;
    focus.stable_id = 0;
    focus.generation = snapshot.generation;
    focus.roi = condition.roi;
    focus.space = condition.roi_space;
    return Result<FocusResolution>{FocusResolution{true, FocusUnresolvedReason::kNotObserved, focus}};
}

Result<void> validate_work_token(const WorkToken& token) noexcept {
    if (token.source_id.empty()) {
        return Result<void>{Status(ErrorCode::kInvalidArgument, "work token requires a source id")};
    }
    if (!usable_rect(token.roi)) {
        return Result<void>{Status(ErrorCode::kInvalidArgument, "non-finite or negative-size token ROI")};
    }
    return Result<void>{};
}

std::optional<WorkRejectionReason> work_token_stale_reason(const WorkToken& token,
                                                           const WorkBindingContext& context) noexcept {
    if (token.source_id != context.source_id) {
        return WorkRejectionReason::kForeignSource;
    }
    if (token.source_epoch != context.source_epoch) {
        return WorkRejectionReason::kStaleSourceEpoch;
    }
    if (token.policy_revision != context.policy_revision) {
        return WorkRejectionReason::kStalePolicyRevision;
    }
    if (token.generation != context.generation) {
        return WorkRejectionReason::kStaleGeneration;
    }
    // The coordinator plans exactly one frame at a time: an older frame was
    // replaced or skipped, a newer one has no plan. Both are stale.
    if (token.frame_sequence != context.frame_sequence) {
        return WorkRejectionReason::kStaleFrameSequence;
    }
    return std::nullopt;
}

bool semantic_event_consistent(const SemanticEvent& event) noexcept {
    if (event.event_sequence == 0 || event.stable_id == 0) {
        return false;
    }
    if (event.window.source_id.empty() || event.window.observed_frame > event.window.confirmed_frame) {
        return false;
    }
    bool pairing_ok = false;
    bool bounds_ok = false;
    switch (event.semantic) {
        case EventSemantic::kBegin:
            pairing_ok = event.change_kind == SemanticChangeKind::kAppeared;
            break;
        case EventSemantic::kUpdate:
            pairing_ok = event.change_kind == SemanticChangeKind::kTextChanged ||
                         event.change_kind == SemanticChangeKind::kAppearanceChanged ||
                         event.change_kind == SemanticChangeKind::kRelationChanged;
            break;
        case EventSemantic::kEnd:
            pairing_ok = event.change_kind == SemanticChangeKind::kDisappeared ||
                         event.change_kind == SemanticChangeKind::kSourceReset;
            break;
    }
    if (!pairing_ok) {
        return false;
    }
    if (event.change_kind == SemanticChangeKind::kSourceReset) {
        // The old epoch's geometry is meaningless after a reset: reset ends
        // carry zero bounds instead of a stale position claim.
        bounds_ok = event.bounds == RectF{};
    } else {
        bounds_ok = usable_rect(event.bounds) && event.bounds.width > 0.0F && event.bounds.height > 0.0F;
    }
    return bounds_ok;
}

int64_t semantic_event_owned_bytes(const SemanticEvent& event) noexcept {
    auto bytes = static_cast<int64_t>(sizeof(SemanticEvent));
    bytes += static_cast<int64_t>(event.window.source_id.capacity());
    return bytes;
}

Result<ContinuousPerception> ContinuousPerception::create(ContinuousPerceptionOptions options) noexcept {
    if (options.max_coordination_bytes <= 0 || options.max_entities <= 0 || options.max_relations <= 0 ||
        options.max_events <= 0 || options.max_focus_conditions <= 0) {
        return Result<ContinuousPerception>{
            Status(ErrorCode::kInvalidArgument, "coordination budgets must be positive")};
    }
    if (options.max_coordination_bytes > kContinuousMaxCoordinationBytes) {
        return Result<ContinuousPerception>{
            Status(ErrorCode::kBudgetExceeded, "max_coordination_bytes looser than the reference cap")};
    }
    if (options.max_entities > kContinuousMaxEntities || options.max_relations > kContinuousMaxRelations ||
        options.max_events > kContinuousMaxEvents || options.max_focus_conditions > kContinuousMaxFocusConditions) {
        return Result<ContinuousPerception>{
            Status(ErrorCode::kBudgetExceeded, "coordination caps looser than DEC-026")};
    }
    return Result<ContinuousPerception>{ContinuousPerception{std::move(options)}};
}

SourceResetReport ContinuousPerception::notify_source_reset() noexcept {
    SourceResetReport report;
    report.previous_epoch = epoch_;
    ++epoch_;
    report.new_epoch = epoch_;
    return report;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): frozen public member of the M9-01 contract surface
int64_t ContinuousPerception::coordination_owned_bytes() const noexcept {
    // M9-01 stores no coordination state yet. The M9-02 relation-view builder
    // is a pure free function and holds nothing: the stateful view lands with
    // the M9-04 coordination wiring, tokens with the M9-04 accept, events
    // with the M9-05 queue. The formula grows with each surface;
    // configuration itself is caller-owned and never counted (DEC-026
    // section 7).
    return 0;
}

}  // namespace mirador
