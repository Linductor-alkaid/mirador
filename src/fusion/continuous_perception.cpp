#include <mirador/continuous_perception.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/object_tracker.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include "rect_math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
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

/// Stage-boundary cancellation/deadline check shared by the continuous-
/// perception entry points (same contract as fuse_evidence).
[[nodiscard]] Status context_status(const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "continuous perception cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "continuous perception deadline exceeded"};
    }
    return {};
}

/// Bytes owned by an engaged attention policy: the conditions vector and
/// every matcher string (DEC-026 section 7 accounting).
[[nodiscard]] int64_t attention_policy_owned_bytes(const AttentionPolicy& policy) noexcept {
    auto bytes = static_cast<int64_t>(sizeof(AttentionPolicy));
    bytes += static_cast<int64_t>(policy.conditions.capacity() * sizeof(AttentionCondition));
    for (const AttentionCondition& condition : policy.conditions) {
        bytes += static_cast<int64_t>(sizeof(AttentionCondition));
        bytes += static_cast<int64_t>(condition.text_equals.capacity());
        bytes += static_cast<int64_t>(condition.label_equals.capacity());
    }
    return bytes;
}

/// Converts a rectangle between kFrame and kOriented through the frame's
/// rotation (DEC-016); kDisplay has no planner-side conversion in M9-03.
/// Unreachable nullopt: rotation matrices are invertible and the engaged
/// spaces are validated by the callers.
[[nodiscard]] std::optional<RectF> convert_plan_rect(const RectF& rect, CoordinateSpaceId from, CoordinateSpaceId to,
                                                     const Frame& frame) noexcept {
    if (from == to) {
        return rect;
    }
    if (from == CoordinateSpaceId::kFrame && to == CoordinateSpaceId::kOriented) {
        return transform_rect(make_rotation(frame.image.rotation, frame.image.width, frame.image.height, from, to),
                              rect);
    }
    if (from == CoordinateSpaceId::kOriented && to == CoordinateSpaceId::kFrame) {
        const Result<Transform2D> inverse_rotation =
            inverse(make_rotation(frame.image.rotation, frame.image.width, frame.image.height,
                                  CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented));
        if (!inverse_rotation.ok()) {
            return std::nullopt;
        }
        return transform_rect(inverse_rotation.value(), rect);
    }
    return std::nullopt;
}

/// Full-frame rectangle of the frame in `space`.
[[nodiscard]] RectF full_frame_rect(const Frame& frame, CoordinateSpaceId space) noexcept {
    if (space == CoordinateSpaceId::kOriented) {
        const auto [width, height] = oriented_size(frame.image.rotation, frame.image.width, frame.image.height);
        return RectF{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
    }
    return RectF{0.0F, 0.0F, static_cast<float>(frame.image.width), static_cast<float>(frame.image.height)};
}

/// True when the ROI positively intersects any resolved focus work item.
[[nodiscard]] bool intersects_any_focus(const RectF& roi, const std::vector<PlannedWork>& work) noexcept {
    return std::ranges::any_of(work, [&roi](const PlannedWork& item) {
        return item.kind == PlannedWorkKind::kFocus && fusion_internal::intersection_area(roi, item.roi) > 0.0;
    });
}

/// Structural + pixel requirements of a cycle frame: a valid single-plane
/// view with resident pixels (the exact ROI digests read them).
[[nodiscard]] Status validate_cycle_frame(const Frame& frame) noexcept {
    if (const Result<void> view = validate(frame.image); !view.ok()) {
        return view.status();
    }
    if (frame.image.data == nullptr) {
        return {ErrorCode::kInvalidArgument, "cycle frames require resident pixels for content binding"};
    }
    if (plane_count(frame.image.format) != 1) {
        return {ErrorCode::kInvalidArgument, "cycle content binding supports single-plane frames only"};
    }
    return {};
}

/// Converts a plan-space ROI into the integer frame-pixel rectangle it
/// covers; rejects anything that leaves the frame or rounds down to empty
/// (DEC-021: reject, never clamp).
[[nodiscard]] Result<RectI> frame_digest_rect(const RectF& roi, CoordinateSpaceId space, const Frame& frame) noexcept {
    const std::optional<RectF> converted = convert_plan_rect(roi, space, CoordinateSpaceId::kFrame, frame);
    if (!converted.has_value()) {
        return Result<RectI>{Status(ErrorCode::kInvalidArgument, "ROI space not convertible to the frame space")};
    }
    const bool inside = converted->x >= 0.0F && converted->y >= 0.0F &&
                        converted->x + converted->width <= static_cast<float>(frame.image.width) &&
                        converted->y + converted->height <= static_cast<float>(frame.image.height);
    const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(converted->x)));
    const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(converted->y)));
    const int32_t x1 = std::min(frame.image.width, static_cast<int32_t>(std::ceil(converted->x + converted->width)));
    const int32_t y1 = std::min(frame.image.height, static_cast<int32_t>(std::ceil(converted->y + converted->height)));
    if (!inside || x1 <= x0 || y1 <= y0) {
        return Result<RectI>{Status(ErrorCode::kInvalidArgument, "work ROI must cover pixels inside the frame")};
    }
    return Result<RectI>{RectI{x0, y0, x1 - x0, y1 - y0}};
}

/// Exact content digest of one frame-pixel rectangle: FNV-1a over the
/// primary-plane bytes of every covered row segment. Layout-sensitive by
/// design (the digest binds the pixels the work was planned against);
/// deterministic across processes.
[[nodiscard]] uint64_t digest_frame_region(const ImageView& view, const RectI& rect) noexcept {
    const int32_t bpp = bytes_per_pixel(view.format);
    const int64_t stride = view.row_stride_bytes;
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (int32_t y = 0; y < rect.height; ++y) {
        const auto* row = view.data + static_cast<int64_t>(rect.y + y) * stride + static_cast<int64_t>(rect.x) * bpp;
        hash = digest_bytes(hash, row, static_cast<size_t>(rect.width) * static_cast<size_t>(bpp));
    }
    return hash;
}

/// True when `inner` lies fully inside `outer` (exact float containment).
[[nodiscard]] bool rect_contains(const RectF& outer, const RectF& inner) noexcept {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Identity a cycle binds and coverage entries are re-verified against
/// (DEC-026 sections 3/5).
struct CycleIdentity {
    uint64_t epoch = 0;
    uint64_t generation = 0;
    uint64_t policy_revision = 0;
    uint64_t request_digest = 0;
    CoordinateSpaceId snapshot_space = CoordinateSpaceId::kOriented;
};

/// First coverage entry whose region contains `roi`, if any.
template <typename CoverageEntries>
[[nodiscard]] std::optional<size_t> find_covering_entry(const CoverageEntries& coverage, const RectF& roi) noexcept {
    for (size_t index = 0; index < coverage.size(); ++index) {
        if (rect_contains(coverage[index].roi, roi)) {
            return index;
        }
    }
    return std::nullopt;
}

/// Re-verifies every coverage entry against the current cycle (DEC-026
/// section 3): identity components (generation, request digest, policy
/// revision, snapshot space), a kGlobal change emptying the ledger, a
/// partial change ROI intersecting the entry, and the exact content digest
/// of the entry's region against `frame`'s pixels. Every drop is recorded.
/// Works on the private CoverageEntry through a template parameter.
template <typename CoverageEntries>
[[nodiscard]] Status refresh_cycle_coverage(CoverageEntries* coverage, const SemanticSnapshot& snapshot,
                                            const Frame& frame, const CycleIdentity& identity,
                                            std::vector<WorkInvalidation>* invalidations, bool* resync_required,
                                            const ExecutionContext& context) noexcept {
    const ChangeReport& change = snapshot.change;
    if (change.classification == ChangeClassification::kGlobal) {
        if (!coverage->empty()) {
            WorkInvalidation record;
            record.roi = full_frame_rect(frame, identity.snapshot_space);
            record.space = identity.snapshot_space;
            record.reason = WorkInvalidationReason::kLayoutChange;
            record.global = true;
            invalidations->push_back(record);
            *resync_required = true;
        }
        coverage->clear();
        return {};
    }

    std::vector<RectF> changed;
    changed.reserve(change.changed_regions.size());
    for (const RectI& region : change.changed_regions) {
        const RectF frame_rect{static_cast<float>(region.x), static_cast<float>(region.y),
                               static_cast<float>(region.width), static_cast<float>(region.height)};
        const std::optional<RectF> converted =
            convert_plan_rect(frame_rect, CoordinateSpaceId::kFrame, identity.snapshot_space, frame);
        if (!converted.has_value()) {
            return {ErrorCode::kInvalidArgument, "change ROI space not convertible to the snapshot space"};
        }
        changed.push_back(*converted);
    }

    size_t kept = 0;
    for (size_t index = 0; index < coverage->size(); ++index) {
        if ((index & 63) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        auto entry = (*coverage)[index];
        std::optional<WorkInvalidationReason> reason;
        if (entry.generation != identity.generation || entry.request_digest != identity.request_digest ||
            entry.policy_revision != identity.policy_revision || entry.space != identity.snapshot_space) {
            reason = WorkInvalidationReason::kIdentityChanged;
        } else if (std::ranges::any_of(changed, [&entry](const RectF& roi) {
                       return fusion_internal::intersection_area(roi, entry.roi) > 0.0;
                   })) {
            reason = WorkInvalidationReason::kRegionChanged;
        } else {
            const Result<RectI> digest_rect = frame_digest_rect(entry.roi, entry.space, frame);
            if (!digest_rect.ok() || digest_frame_region(frame.image, digest_rect.value()) != entry.content_digest) {
                reason = WorkInvalidationReason::kContentMismatch;
            }
        }
        if (reason.has_value()) {
            WorkInvalidation record;
            record.roi = entry.roi;
            record.space = entry.space;
            record.reason = *reason;
            record.global = false;
            invalidations->push_back(record);
            continue;
        }
        (*coverage)[kept++] = entry;
    }
    coverage->resize(kept);
    return {};
}

/// Reports snapshot regions covered by a surviving coverage entry (AC9-03:
/// the validating frame stays queryable).
template <typename CoverageEntries>
[[nodiscard]] Status emit_cycle_reuse(const std::vector<VisualRegion>& regions, const CoverageEntries& coverage,
                                      CoordinateSpaceId space, std::vector<WorkReuse>* reuse,
                                      const ExecutionContext& context) noexcept {
    for (size_t index = 0; index < regions.size(); ++index) {
        if ((index & 63) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        const std::optional<size_t> covering = find_covering_entry(coverage, regions[index].bounds);
        if (!covering.has_value()) {
            continue;
        }
        WorkReuse entry;
        entry.roi = regions[index].bounds;
        entry.space = space;
        entry.generation = coverage[*covering].generation;
        entry.validated_frame = coverage[*covering].frame_sequence;
        reuse->push_back(entry);
    }
    return {};
}

/// Issues the cycle work: covered-and-verified focus candidates are
/// suppressed into reuse (the near-zero-cost path); the rest become
/// binding tokens (DEC-026 section 5).
template <typename CoverageEntries>
[[nodiscard]] Status emit_cycle_work(const std::vector<PlannedWork>& candidates, const CoverageEntries& coverage,
                                     const Frame& frame, const CycleIdentity& identity, uint64_t frame_sequence,
                                     const std::string& source_id, std::vector<PlannedWorkBinding>* work,
                                     std::vector<WorkReuse>* reuse, std::vector<WorkToken>* tokens,
                                     const ExecutionContext& context) noexcept {
    for (size_t index = 0; index < candidates.size(); ++index) {
        if ((index & 15) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        const PlannedWork& candidate = candidates[index];
        const Result<RectI> digest_rect = frame_digest_rect(candidate.roi, candidate.space, frame);
        if (!digest_rect.ok()) {
            return digest_rect.status();
        }
        if (candidate.kind == PlannedWorkKind::kFocus) {
            const std::optional<size_t> covering = find_covering_entry(coverage, candidate.roi);
            if (covering.has_value()) {
                WorkReuse entry;
                entry.roi = candidate.roi;
                entry.space = candidate.space;
                entry.generation = coverage[*covering].generation;
                entry.validated_frame = coverage[*covering].frame_sequence;
                reuse->push_back(entry);
                continue;
            }
        }
        if (coverage.size() + tokens->size() >= static_cast<size_t>(kContinuousMaxEntities)) {
            return {ErrorCode::kBudgetExceeded, "cycle work exceeds the coverage cap"};
        }
        PlannedWorkBinding binding;
        binding.work = candidate;
        binding.token.source_id = source_id;
        binding.token.source_epoch = identity.epoch;
        binding.token.frame_sequence = frame_sequence;
        binding.token.generation = identity.generation;
        binding.token.policy_revision = identity.policy_revision;
        binding.token.request_digest = identity.request_digest;
        binding.token.roi = candidate.roi;
        binding.token.roi_space = candidate.space;
        binding.token.content_digest = digest_frame_region(frame.image, digest_rect.value());
        tokens->push_back(binding.token);
        work->push_back(std::move(binding));
    }
    return {};
}

/// Per-item outcome of the accept validation pass over the current
/// in-flight set (nullopt = valid). The frozen staleness order decides
/// first; then the token must belong to the current in-flight set and be
/// unadopted; finally the exact content re-digest must match.
template <typename InFlightEntries>
[[nodiscard]] std::optional<WorkRejectionReason> classify_accept_item(const WorkToken& token,
                                                                      const InFlightEntries& inflight,
                                                                      const WorkBindingContext& binding,
                                                                      const Frame& frame) noexcept {
    if (const std::optional<WorkRejectionReason> stale = work_token_stale_reason(token, binding)) {
        return stale;
    }
    for (const auto& flight : inflight) {
        if (!(flight.token == token)) {
            continue;
        }
        if (flight.adopted) {
            return WorkRejectionReason::kDuplicateSubmission;
        }
        const Result<RectI> rect = frame_digest_rect(token.roi, token.roi_space, frame);
        if (!rect.ok() || digest_frame_region(frame.image, rect.value()) != token.content_digest) {
            return WorkRejectionReason::kContentMismatch;
        }
        return std::nullopt;
    }
    // Matching identity but absent from the in-flight set: no plan issued
    // for it under the current cycle (frozen kStaleFrameSequence case).
    return WorkRejectionReason::kStaleFrameSequence;
}

/// Input validation of one attention plan: snapshot space, frame geometry
/// and the caller's priority (DEC-026 sections 1/2).
[[nodiscard]] Status validate_plan_input(const SemanticSnapshot& snapshot, const Frame& frame,
                                         const ImportancePolicy& importance) noexcept {
    if (snapshot.coordinate_space != CoordinateSpaceId::kFrame &&
        snapshot.coordinate_space != CoordinateSpaceId::kOriented) {
        return {ErrorCode::kInvalidArgument, "attention planning supports kFrame/kOriented snapshot spaces"};
    }
    if (frame.image.width <= 0 || frame.image.height <= 0) {
        return {ErrorCode::kInvalidArgument, "attention planning requires a sized frame"};
    }
    if (!std::isfinite(importance.priority)) {
        return {ErrorCode::kInvalidArgument, "importance priority must be finite"};
    }
    return {};
}

/// Resolves every engaged policy condition and emits the focus/bootstrap
/// work items (DEC-026 section 2). Resolutions are recorded parallel to the
/// conditions; nothing falls back to full-frame work on its own — bootstrap
/// appears only under the caller's importance authorization.
[[nodiscard]] Status append_focus_work(const std::vector<AttentionCondition>& conditions,
                                       const SemanticSnapshot& snapshot, const Frame& frame,
                                       const ImportancePolicy& importance, const ContinuousPerception& coordinator,
                                       AttentionPlan* plan, const ExecutionContext& context) noexcept {
    for (size_t index = 0; index < conditions.size(); ++index) {
        if ((index & 63) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        const Result<FocusResolution> resolved = coordinator.resolve_focus(conditions[index], snapshot);
        if (!resolved.ok()) {
            return resolved.status();
        }
        plan->resolutions.push_back(resolved.value());
        if (resolved.value().resolved) {
            const ResolvedFocus& focus = resolved.value().focus;
            const std::optional<RectF> converted =
                convert_plan_rect(focus.roi, focus.space, snapshot.coordinate_space, frame);
            if (!converted.has_value()) {
                return {ErrorCode::kInvalidArgument, "focus space not convertible to the snapshot space"};
            }
            PlannedWork work;
            work.kind = PlannedWorkKind::kFocus;
            work.roi = *converted;
            work.space = snapshot.coordinate_space;
            work.priority = importance.priority;
            // Identity binding only for a focus that carries one (stable-id
            // / text / label resolution); ROI-only work reports none.
            work.stable_id = focus.stable_id;
            work.generation = focus.stable_id != 0 ? focus.generation : 0;
            work.condition_index = index;
            plan->work.push_back(work);
        } else if (resolved.value().reason == FocusUnresolvedReason::kNotObserved &&
                   importance.allow_bootstrap_full_frame) {
            PlannedWork work;
            work.kind = PlannedWorkKind::kBootstrap;
            work.roi = full_frame_rect(frame, snapshot.coordinate_space);
            work.space = snapshot.coordinate_space;
            work.priority = importance.priority;
            work.condition_index = index;
            plan->work.push_back(work);
        }
    }
    return {};
}

/// Peripheral monitoring (DEC-026 section 2): the snapshot's change report
/// yields queryable reconsideration records for changed regions outside
/// every resolved focus. Recording is a visual fact; acting on it belongs
/// to the caller policy and the M9-04/M9-05 wiring.
[[nodiscard]] Status append_reconsiderations(const SemanticSnapshot& snapshot, const Frame& frame, AttentionPlan* plan,
                                             const ExecutionContext& context) noexcept {
    const ChangeReport& change = snapshot.change;
    if (change.classification == ChangeClassification::kGlobal) {
        FocusReconsideration record;
        record.changed_region = full_frame_rect(frame, snapshot.coordinate_space);
        record.space = snapshot.coordinate_space;
        record.global = true;
        plan->reconsiderations.push_back(record);
        return {};
    }
    if (change.classification != ChangeClassification::kPartial) {
        return {};
    }
    for (size_t index = 0; index < change.changed_regions.size(); ++index) {
        if ((index & 63) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        const RectI& region = change.changed_regions[index];
        const RectF frame_rect{static_cast<float>(region.x), static_cast<float>(region.y),
                               static_cast<float>(region.width), static_cast<float>(region.height)};
        const std::optional<RectF> converted =
            convert_plan_rect(frame_rect, CoordinateSpaceId::kFrame, snapshot.coordinate_space, frame);
        if (!converted.has_value()) {
            return {ErrorCode::kInvalidArgument, "change ROI space not convertible to the snapshot space"};
        }
        if (!intersects_any_focus(*converted, plan->work)) {
            FocusReconsideration record;
            record.changed_region = *converted;
            record.space = snapshot.coordinate_space;
            record.global = false;
            plan->reconsiderations.push_back(record);
        }
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
    // The rebuild ends the old generation's cycle state (DEC-026 section
    // 8): in-flight descriptors and coverage never re-validate under the
    // new epoch.
    inflight_.clear();
    coverage_.clear();
    resync_pending_ = true;
    return report;
}

Result<WorkPlan> ContinuousPerception::plan_cycle(const SemanticSnapshot& snapshot, const Frame& frame,
                                                  const ImportancePolicy& importance, uint64_t request_digest,
                                                  const ExecutionContext& context) noexcept {
    if (const Status validated = validate_plan_input(snapshot, frame, importance); !validated.ok()) {
        return Result<WorkPlan>{validated};
    }
    if (const Status usable = validate_cycle_frame(frame); !usable.ok()) {
        return Result<WorkPlan>{usable};
    }
    if (options_.source_id.empty()) {
        // Tokens bind the source identity; a coordinator created without
        // one could plan work its own accept would always reject.
        return Result<WorkPlan>{Status(ErrorCode::kInvalidArgument, "cycle planning requires a non-empty source id")};
    }

    Result<AttentionPlan> attention = plan_attention(snapshot, frame, importance, context);
    if (!attention.ok()) {
        return Result<WorkPlan>{attention.status()};
    }
    WorkPlan plan;
    plan.attention = attention.take_value();

    const CycleIdentity identity{epoch_, snapshot.generation, policy_revision_, request_digest,
                                 snapshot.coordinate_space};
    if (const Status refreshed = refresh_cycle_coverage(&coverage_, snapshot, frame, identity, &plan.invalidations,
                                                        &resync_pending_, context);
        !refreshed.ok()) {
        return Result<WorkPlan>{refreshed};
    }
    if (const Status reused =
            emit_cycle_reuse(snapshot.regions, coverage_, snapshot.coordinate_space, &plan.reuse, context);
        !reused.ok()) {
        return Result<WorkPlan>{reused};
    }

    // The previous in-flight descriptors not adopted are replaced (the
    // caller's one-in-flight + one-latest-pending policy keeps this
    // bounded; skipped work is never silently carried over).
    plan.replaced_inflight = static_cast<uint64_t>(
        std::count_if(inflight_.begin(), inflight_.end(), [](const InFlightWork& flight) { return !flight.adopted; }));
    std::vector<WorkToken> tokens;
    tokens.reserve(plan.attention.work.size());
    if (const Status issued = emit_cycle_work(plan.attention.work, coverage_, frame, identity, frame.sequence,
                                              options_.source_id, &plan.work, &plan.reuse, &tokens, context);
        !issued.ok()) {
        return Result<WorkPlan>{issued};
    }

    // Budget gate before any state mutation (DEC-026 section 7): the cycle
    // state with the new in-flight set must fit max_coordination_bytes.
    int64_t projected = coordination_owned_bytes();
    projected -= static_cast<int64_t>(inflight_.size() * sizeof(InFlightWork));
    for (const InFlightWork& flight : inflight_) {
        projected -= static_cast<int64_t>(flight.token.source_id.capacity());
    }
    projected += static_cast<int64_t>(tokens.capacity() * sizeof(InFlightWork));
    for (const WorkToken& token : tokens) {
        projected += static_cast<int64_t>(token.source_id.capacity());
    }
    if (projected > options_.max_coordination_bytes) {
        return Result<WorkPlan>{Status(ErrorCode::kBudgetExceeded, "cycle state exceeds max_coordination_bytes")};
    }

    inflight_.assign(tokens.size(), InFlightWork{WorkToken{}, false});
    for (size_t index = 0; index < tokens.size(); ++index) {
        inflight_[index].token = tokens[index];
    }
    planned_frame_sequence_ = frame.sequence;
    planned_generation_ = snapshot.generation;
    plan.resync_required = resync_pending_;
    resync_pending_ = false;
    plan.coverage = CoverageState::kIncomplete;
    if (!coverage_.empty() && std::ranges::all_of(snapshot.regions, [this](const VisualRegion& region) {
            return find_covering_entry(coverage_, region.bounds).has_value();
        })) {
        plan.coverage = CoverageState::kComplete;
    }
    return Result<WorkPlan>{std::move(plan)};
}

Result<WorkAcceptReport> ContinuousPerception::accept_work(std::span<const WorkToken> tokens, const Frame& frame,
                                                           const ExecutionContext& context) noexcept {
    if (const Status usable = validate_cycle_frame(frame); !usable.ok()) {
        return Result<WorkAcceptReport>{usable};
    }

    WorkAcceptReport report;
    report.item_rejections.resize(tokens.size());
    const WorkBindingContext binding{options_.source_id, epoch_, planned_frame_sequence_, planned_generation_,
                                     policy_revision_};

    // Pass 0 — structure and geometry abort the whole call (DEC-021).
    for (const WorkToken& token : tokens) {
        if (const Result<void> structural = validate_work_token(token); !structural.ok()) {
            return Result<WorkAcceptReport>{structural.status()};
        }
        if (!frame_digest_rect(token.roi, token.roi_space, frame).ok()) {
            return Result<WorkAcceptReport>{
                Status(ErrorCode::kInvalidArgument, "token ROI must cover pixels inside the frame")};
        }
    }

    // Pass 1 — per-item binding and content validation.
    bool rejected = false;
    for (size_t index = 0; index < tokens.size(); ++index) {
        if ((index & 63) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return Result<WorkAcceptReport>{stage};
            }
        }
        const std::optional<WorkRejectionReason> reason =
            classify_accept_item(tokens[index], inflight_, binding, frame);
        if (!reason.has_value()) {
            continue;
        }
        report.item_rejections[index] = reason;
        ++rejection_counts_[static_cast<size_t>(*reason)];
        rejected = true;
    }
    if (rejected) {
        return Result<WorkAcceptReport>{report};
    }

    // Pass 2 — atomic commit into the coverage ledger (refresh-on-same-
    // region keeps the ledger duplicate-free and bounded).
    if (coverage_.size() + tokens.size() > static_cast<size_t>(kContinuousMaxEntities)) {
        return Result<WorkAcceptReport>{Status(ErrorCode::kBudgetExceeded, "accept exceeds the coverage cap")};
    }
    int64_t projected = coordination_owned_bytes();
    projected += static_cast<int64_t>(tokens.size() * sizeof(CoverageEntry));
    if (projected > options_.max_coordination_bytes) {
        return Result<WorkAcceptReport>{Status(ErrorCode::kBudgetExceeded, "accept exceeds max_coordination_bytes")};
    }
    for (const WorkToken& token : tokens) {
        std::erase_if(coverage_, [&token](const CoverageEntry& entry) {
            return entry.space == token.roi_space && entry.roi == token.roi;
        });
        CoverageEntry entry;
        entry.roi = token.roi;
        entry.space = token.roi_space;
        entry.generation = token.generation;
        entry.frame_sequence = token.frame_sequence;
        entry.policy_revision = token.policy_revision;
        entry.request_digest = token.request_digest;
        entry.content_digest = token.content_digest;
        coverage_.push_back(entry);
        for (InFlightWork& flight : inflight_) {
            if (flight.token == token) {
                flight.adopted = true;
            }
        }
    }
    report.committed = true;
    report.adopted_count = static_cast<uint64_t>(tokens.size());
    return Result<WorkAcceptReport>{report};
}

uint64_t ContinuousPerception::rejection_count(WorkRejectionReason reason) const noexcept {
    const auto index = static_cast<size_t>(reason);
    if (index >= rejection_counts_.size()) {
        return 0;
    }
    return rejection_counts_[index];
}

Result<void> ContinuousPerception::set_attention_policy(const AttentionPolicy& policy) noexcept {
    if (Result<void> validated = validate_attention_policy(policy); !validated.ok()) {
        return validated;
    }
    for (const AttentionCondition& condition : policy.conditions) {
        const bool has_roi = condition.roi.width > 0.0F && condition.roi.height > 0.0F;
        if (has_roi && condition.roi_space != CoordinateSpaceId::kFrame &&
            condition.roi_space != CoordinateSpaceId::kOriented) {
            return Result<void>{
                Status(ErrorCode::kInvalidArgument, "attention condition ROI spaces support kFrame/kOriented only")};
        }
    }
    const int64_t engaged_bytes = attention_policy_owned_bytes(policy);
    if (engaged_bytes > options_.max_coordination_bytes) {
        return Result<void>{Status(ErrorCode::kBudgetExceeded, "attention policy exceeds max_coordination_bytes")};
    }
    // Revision moves exactly on a digest change: re-setting the identical
    // policy is idempotent, a first engagement always bumps from 0.
    if (!policy_.has_value() || attention_policy_digest(policy) != attention_policy_digest(*policy_)) {
        ++policy_revision_;
    }
    policy_ = policy;
    return Result<void>{};
}

Result<FocusResolution> ContinuousPerception::resolve_focus(const AttentionCondition& condition,
                                                            const SemanticSnapshot& snapshot) const noexcept {
    Result<FocusResolution> resolution = resolve_attention_focus(condition, snapshot);
    if (!resolution.ok() || resolution.value().resolved) {
        return resolution;
    }
    // Tracker fallback (DEC-026 section 2, M9-03): only a stable-id
    // condition the snapshot cannot see consults the tracker — a stale
    // generation short-circuits above (frozen order) and text/label/ROI
    // conditions are snapshot-only observations.
    if (!condition.bind_stable_id || tracker_ == nullptr ||
        resolution.value().reason != FocusUnresolvedReason::kUnknownStableId) {
        return resolution;
    }
    const TargetTrack* track = tracker_->find_track(condition.stable_id);
    if (track == nullptr || track->state == TrackState::kTerminated) {
        // The identity is not tracked (anymore): kUnknownStableId stands.
        return resolution;
    }
    if (track->state == TrackState::kLost) {
        return Result<FocusResolution>{FocusResolution{false, FocusUnresolvedReason::kMissingLocalization, {}}};
    }
    // kTracking or kUncertain: the pool's current position is usable — the
    // focus moves with the track, never with a memorized coordinate.
    ResolvedFocus focus;
    focus.stable_id = condition.stable_id;
    focus.generation = snapshot.generation;
    focus.roi = track->last_bounds;
    focus.space = CoordinateSpaceId::kOriented;  // the pool's presented-view space
    return Result<FocusResolution>{FocusResolution{true, FocusUnresolvedReason::kNotObserved, focus}};
}

Result<AttentionPlan> ContinuousPerception::plan_attention(const SemanticSnapshot& snapshot, const Frame& frame,
                                                           const ImportancePolicy& importance,
                                                           const ExecutionContext& context) const noexcept {
    if (const Status validated = validate_plan_input(snapshot, frame, importance); !validated.ok()) {
        return Result<AttentionPlan>{validated};
    }

    AttentionPlan plan;
    plan.policy_revision = policy_revision_;
    plan.coverage = CoverageState::kIncomplete;

    const std::vector<AttentionCondition> unset_conditions;
    const std::vector<AttentionCondition>& conditions = policy_.has_value() ? policy_->conditions : unset_conditions;
    if (const Status planned = append_focus_work(conditions, snapshot, frame, importance, *this, &plan, context);
        !planned.ok()) {
        return Result<AttentionPlan>{planned};
    }
    std::sort(plan.work.begin(), plan.work.end(), [](const PlannedWork& lhs, const PlannedWork& rhs) {
        if (lhs.kind != rhs.kind) {
            return lhs.kind < rhs.kind;
        }
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        return lhs.condition_index < rhs.condition_index;
    });
    if (const Status monitored = append_reconsiderations(snapshot, frame, &plan, context); !monitored.ok()) {
        return Result<AttentionPlan>{monitored};
    }

    return Result<AttentionPlan>{std::move(plan)};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): frozen public member of the M9-01 contract surface
int64_t ContinuousPerception::coordination_owned_bytes() const noexcept {
    // M9-03 owns the engaged attention policy (conditions and matcher
    // text); M9-04 owns the in-flight descriptor set (with each token's
    // source-id string) and the coverage ledger; the rejection counters are
    // fixed-width. The tracker is injected and caller-owned — the pointer
    // is not accounted. An unset policy and empty cycle state own nothing.
    // Events land with the M9-05 queue; the formula grows with each
    // surface; configuration itself is caller-owned and never counted
    // (DEC-026 section 7).
    int64_t bytes = 0;
    if (policy_.has_value()) {
        bytes += attention_policy_owned_bytes(*policy_);
    }
    bytes += static_cast<int64_t>(inflight_.capacity() * sizeof(InFlightWork));
    for (const InFlightWork& flight : inflight_) {
        bytes += static_cast<int64_t>(flight.token.source_id.capacity());
    }
    bytes += static_cast<int64_t>(coverage_.capacity() * sizeof(CoverageEntry));
    // The rejection counters are fixed-width and cannot grow: like the
    // injected tracker pointer they carry no budget-relevant capacity and
    // are not accounted (DEC-026 section 7).
    return bytes;
}

}  // namespace mirador
