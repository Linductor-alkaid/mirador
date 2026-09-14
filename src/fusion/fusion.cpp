#include <mirador/fusion.hpp>

#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include "rect_math.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mirador {
namespace {

// Stage-boundary cancellation/deadline check (same contract as the session).
Status context_status(const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "fuse_evidence cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "fuse_evidence deadline exceeded"};
    }
    return {};
}

Status validate_options(const FusionOptions& options) noexcept {
    if (!std::isfinite(options.iou_threshold) || options.iou_threshold < 0.0 || options.iou_threshold > 1.0) {
        return {ErrorCode::kInvalidArgument, "iou_threshold must be finite in [0, 1]"};
    }
    if (!std::isfinite(options.containment_threshold) || options.containment_threshold < 0.0 ||
        options.containment_threshold > 1.0) {
        return {ErrorCode::kInvalidArgument, "containment_threshold must be finite in [0, 1]"};
    }
    for (const float weight :
         {options.external_weight, options.ocr_weight, options.detector_weight, options.template_weight}) {
        if (!std::isfinite(weight) || weight < 0.0F) {
            return {ErrorCode::kInvalidArgument, "source weights must be finite and non-negative"};
        }
    }
    if (options.max_regions <= 0) {
        return {ErrorCode::kInvalidArgument, "max_regions must be positive"};
    }
    return {};
}

/// Evidence item moved into the fusion target space.
struct WorkingItem {
    const EvidenceItem* item = nullptr;
    RectF bounds;
    PointF center;
};

/// Raw-frame dimensions behind an oriented view (inverse of oriented_size).
std::pair<int32_t, int32_t> raw_dimensions(Rotation rotation, int32_t oriented_width,
                                           int32_t oriented_height) noexcept {
    if (rotation == Rotation::k90 || rotation == Rotation::k270) {
        return {oriented_height, oriented_width};
    }
    return {oriented_width, oriented_height};
}

/// Transform from an item's space into the target space; nullopt when spaces
/// already match. kFrame <-> kOriented goes through the view rotation.
Result<std::optional<Transform2D>> space_conversion(CoordinateSpaceId item_space, const Frame& frame,
                                                    const FusionOptions& options) noexcept {
    if (item_space == options.target_space) {
        return std::optional<Transform2D>{};
    }
    const ImageView& view = frame.image;
    const auto [raw_width, raw_height] = raw_dimensions(view.rotation, view.width, view.height);
    const Transform2D frame_to_oriented =
        make_rotation(view.rotation, raw_width, raw_height, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
    if (options.target_space == CoordinateSpaceId::kOriented && item_space == CoordinateSpaceId::kFrame) {
        return std::optional<Transform2D>{frame_to_oriented};
    }
    if (options.target_space == CoordinateSpaceId::kFrame && item_space == CoordinateSpaceId::kOriented) {
        Result<Transform2D> oriented_to_frame = inverse(frame_to_oriented);
        if (!oriented_to_frame.ok()) {
            return oriented_to_frame.status();
        }
        return std::optional<Transform2D>{oriented_to_frame.take_value()};
    }
    return Status(ErrorCode::kInvalidArgument, "evidence space must be kFrame or kOriented");
}

Result<std::vector<WorkingItem>> working_items(const EvidenceSet& evidence, const Frame& frame,
                                               const FusionOptions& options) noexcept {
    const std::vector<EvidenceItem>& items = evidence.items();
    std::vector<WorkingItem> working;
    working.reserve(items.size());
    for (const EvidenceItem& item : items) {
        const Result<std::optional<Transform2D>> conversion = space_conversion(item.space, frame, options);
        if (!conversion.ok()) {
            return conversion.status();
        }
        WorkingItem moved;
        moved.item = &item;
        moved.bounds = conversion.value().has_value() ? transform_rect(conversion.value().value(), item.bounds())
                                                      : item.bounds();
        moved.center = fusion_internal::rect_center(moved.bounds);
        working.push_back(std::move(moved));
    }
    return working;
}

struct GateMetrics {
    double iou = 0.0;
    double containment = 0.0;
    double center_distance = 0.0;
};

GateMetrics gate_metrics(const WorkingItem& a, const WorkingItem& b) noexcept {
    GateMetrics metrics;
    metrics.iou = fusion_internal::rect_iou(a.bounds, b.bounds);
    const double inter = fusion_internal::intersection_area(a.bounds, b.bounds);
    const double smaller = std::min(fusion_internal::rect_area(a.bounds), fusion_internal::rect_area(b.bounds));
    if (smaller > 0.0) {
        metrics.containment = inter / smaller;
    }
    metrics.center_distance = fusion_internal::center_distance(a.bounds, b.bounds);
    return metrics;
}

/// Class-compatibility predicate (design section 16): two detections with
/// contradicting, fully specified classes never merge; everything else is
/// left to the gates.
bool compatible(const WorkingItem& a, const WorkingItem& b) noexcept {
    const EvidenceItem* first = a.item;
    const EvidenceItem* second = b.item;
    if (first->kind != EvidenceKind::kDetection || second->kind != EvidenceKind::kDetection) {
        return true;
    }
    const DetectionRegion& left = first->detection;
    const DetectionRegion& right = second->detection;
    const bool left_known = left.class_id >= 0 && !left.label.empty();
    const bool right_known = right.class_id >= 0 && !right.label.empty();
    if (left_known && right_known) {
        return left.class_id == right.class_id && left.label == right.label;
    }
    return true;
}

std::optional<AssociationRule> associated_by(const GateMetrics& metrics, const FusionOptions& options) noexcept {
    if (metrics.iou >= options.iou_threshold) {
        return AssociationRule::kIou;
    }
    if (metrics.containment >= options.containment_threshold) {
        return AssociationRule::kContainment;
    }
    return std::nullopt;
}

/// One recorded merge decision plus the index that produced it (evidence ids
/// alone do not identify the working item).
struct RecordedMerge {
    AssociationObservation observation;
    size_t first_index = 0;
};

/// Disjoint-set over item indices; `find` returns the smallest index of the
/// cluster so output ordering stays deterministic.
class Clusters {
public:
    explicit Clusters(size_t count) : parent_(count) {
        for (size_t index = 0; index < count; ++index) {
            parent_[index] = index;
        }
    }

    size_t find(size_t index) noexcept {
        while (parent_[index] != index) {
            parent_[index] = parent_[parent_[index]];
            index = parent_[index];
        }
        return index;
    }

    void unite(size_t first, size_t second) noexcept {
        const size_t root_first = find(first);
        const size_t root_second = find(second);
        if (root_first == root_second) {
            return;
        }
        if (root_first < root_second) {
            parent_[root_second] = root_first;
        } else {
            parent_[root_first] = root_second;
        }
    }

private:
    std::vector<size_t> parent_;
};

/// Deterministic id-ordered pair scan; merges are recorded when a pair joins
/// two distinct clusters (transitive membership is implied, not re-recorded).
/// Polls `context` every 64 outer stripes.
Result<std::vector<RecordedMerge>> scan_pairs(const std::vector<WorkingItem>& working, Clusters& clusters,
                                              const FusionOptions& options, const ExecutionContext& context) {
    std::vector<RecordedMerge> merges;
    merges.reserve(working.size());
    for (size_t first = 0; first < working.size(); ++first) {
        if ((first % 64) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        for (size_t second = first + 1; second < working.size(); ++second) {
            if (clusters.find(first) == clusters.find(second) || !compatible(working[first], working[second])) {
                continue;
            }
            const GateMetrics metrics = gate_metrics(working[first], working[second]);
            const std::optional<AssociationRule> rule = associated_by(metrics, options);
            if (!rule.has_value()) {
                continue;
            }
            clusters.unite(first, second);
            AssociationObservation observation;
            observation.first_evidence_id = working[first].item->evidence_id;
            observation.second_evidence_id = working[second].item->evidence_id;
            observation.rule = *rule;
            observation.iou = metrics.iou;
            observation.containment = metrics.containment;
            observation.center_distance = metrics.center_distance;
            merges.push_back(RecordedMerge{observation, first});
        }
    }
    return merges;
}

float source_weight(const EvidenceItem& item, const FusionOptions& options) noexcept {
    if (has_source(item.source, RegionSource::kExternal)) {
        return options.external_weight;
    }
    if (has_source(item.source, RegionSource::kOcr)) {
        return options.ocr_weight;
    }
    if (has_source(item.source, RegionSource::kDetector)) {
        return options.detector_weight;
    }
    return options.template_weight;
}

void append_text(VisualRegion& region, const EvidenceItem& item) {
    std::string_view text;
    if (item.kind == EvidenceKind::kText) {
        text = item.text.utf8_text;
    } else if (item.kind == EvidenceKind::kExternal) {
        text = item.external.text;
    } else {
        return;
    }
    if (text.empty()) {
        return;
    }
    if (!region.text.empty()) {
        region.text.push_back('\n');
    }
    region.text.append(text);
}

void set_label(VisualRegion& region, const EvidenceItem& item) {
    if (!region.label.empty()) {
        return;
    }
    if (item.kind == EvidenceKind::kDetection) {
        region.label = item.detection.label;
    } else if (item.kind == EvidenceKind::kExternal) {
        region.label = item.external.role;
    }
}

RectF union_bounds(const RectF& a, const RectF& b) noexcept {
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x + a.width, b.x + b.width);
    const float bottom = std::max(a.y + a.height, b.y + b.height);
    return RectF{left, top, right - left, bottom - top};
}

VisualRegion assemble_region(const std::vector<size_t>& members, const std::vector<WorkingItem>& working,
                             const FusionOptions& options, RegionTrace& trace) {
    VisualRegion region;
    float weighted_sum = 0.0F;
    float weight_sum = 0.0F;
    bool bounds_initialized = false;
    for (const size_t index : members) {
        const EvidenceItem& item = *working[index].item;
        region.bounds =
            bounds_initialized ? union_bounds(region.bounds, working[index].bounds) : working[index].bounds;
        bounds_initialized = true;
        region.source_mask |= item.source;
        append_text(region, item);
        set_label(region, item);
        const float weight = source_weight(item, options);
        const float confidence = item.confidence();
        weighted_sum += weight * confidence;
        weight_sum += weight;
        trace.evidence_ids.push_back(item.evidence_id);
        trace.confidence_contributions.push_back(ConfidenceContribution{item.evidence_id, weight, confidence});
    }
    region.anchor = fusion_internal::rect_center(region.bounds);
    region.confidence = weight_sum > 0.0F ? weighted_sum / weight_sum : 0.0F;
    trace.source_mask = region.source_mask;
    return region;
}

}  // namespace

Result<FusionOutput> fuse_evidence(const EvidenceSet& evidence, const Frame& frame, const FusionOptions& options,
                                   const ExecutionContext& context) noexcept {
    try {
        if (const Status stage = context_status(context); !stage.ok()) {
            return stage;
        }
        if (const Status valid = validate_options(options); !valid.ok()) {
            return valid;
        }
        const Result<std::vector<WorkingItem>> working = working_items(evidence, frame, options);
        if (!working.ok()) {
            return working.status();
        }
        const size_t count = working.value().size();

        Clusters clusters(count);
        const Result<std::vector<RecordedMerge>> merges = scan_pairs(working.value(), clusters, options, context);
        if (!merges.ok()) {
            return merges.status();
        }

        // Cluster membership in id order, keyed by cluster root (= smallest
        // member index), so output regions come out ordered by smallest
        // evidence id.
        std::vector<std::vector<size_t>> members_by_root(count);
        size_t cluster_count = 0;
        for (size_t index = 0; index < count; ++index) {
            std::vector<size_t>& members = members_by_root[clusters.find(index)];
            if (members.empty()) {
                ++cluster_count;
            }
            members.push_back(index);
        }
        if (static_cast<int64_t>(cluster_count) > static_cast<int64_t>(options.max_regions)) {
            return Status(ErrorCode::kBudgetExceeded, "fused region count exceeds max_regions");
        }

        // Bucket the recorded merges per cluster root, preserving their global
        // application order within each region.
        std::vector<std::vector<const AssociationObservation*>> merges_by_root(count);
        for (const RecordedMerge& merge : merges.value()) {
            merges_by_root[clusters.find(merge.first_index)].push_back(&merge.observation);
        }

        FusionOutput output;
        output.regions.reserve(cluster_count);
        output.trace.regions.reserve(cluster_count);
        for (size_t root = 0; root < count; ++root) {
            const std::vector<size_t>& members = members_by_root[root];
            if (members.empty()) {
                continue;
            }
            RegionTrace trace;
            output.regions.push_back(assemble_region(members, working.value(), options, trace));
            trace.associations.reserve(merges_by_root[root].size());
            for (const AssociationObservation* observation : merges_by_root[root]) {
                trace.associations.push_back(*observation);
            }
            output.trace.regions.push_back(std::move(trace));
        }
        output.trace.input_evidence_count = count;
        return output;
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "fuse_evidence: internal allocation failed");
    }
}

}  // namespace mirador
