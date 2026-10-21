#include <mirador/continuous_perception.hpp>

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
    // M9-01 stores no coordination state yet: entities land with the M9-02
    // relation view, tokens with the M9-04 accept, events with the M9-05
    // queue. The formula grows with each surface; configuration itself is
    // caller-owned and never counted (DEC-026 section 7).
    return 0;
}

}  // namespace mirador
