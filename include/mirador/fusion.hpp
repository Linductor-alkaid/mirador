#ifndef MIRADOR_FUSION_HPP
#define MIRADOR_FUSION_HPP

#include <mirador/evidence.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>

#include <cstdint>
#include <vector>

namespace mirador {

/// Which gate produced one association (design section 16; DEC-010 section 4).
/// Phase-1 gates: IoU overlap and containment. Center distance is recorded in
/// every observation for explainability but never associates by itself —
/// proximity alone must not merge semantically different evidence.
enum class AssociationRule : uint8_t {
    kIou,          ///< intersection-over-union reached `FusionOptions::iou_threshold`
    kContainment,  ///< intersection / smaller area reached `containment_threshold`
};

/// Explainability record of one merge decision (design section 16: which
/// evidence was merged, by which rule). `center_distance` is informational.
struct AssociationObservation {
    uint64_t first_evidence_id = 0;
    uint64_t second_evidence_id = 0;
    AssociationRule rule = AssociationRule::kIou;
    double iou = 0.0;
    double containment = 0.0;   ///< intersection / min(area_a, area_b)
    double center_distance = 0.0;  ///< pixels in the fusion target space
};

/// How one output region's confidence was produced (design section 16: source
/// weights and geometric agreement, an explicit rule — not a probability).
struct ConfidenceContribution {
    uint64_t evidence_id = 0;
    float weight = 0.0F;      ///< source weight from `FusionOptions`
    float confidence = 0.0F;  ///< raw evidence confidence
};

/// Per-output-region trace, parallel to `FusionOutput::regions`.
struct RegionTrace {
    std::vector<uint64_t> evidence_ids;  ///< contributing evidence, ascending
    uint32_t source_mask = 0;
    std::vector<ConfidenceContribution> confidence_contributions;
    /// Union events that grew this cluster, in the deterministic order they
    /// were applied (first two items associate by rule, later ones join).
    std::vector<AssociationObservation> associations;
};

/// Explainability report of one fusion, parallel to the output regions
/// (design section 16). Never logged by default (RULE-10); callers own it.
struct FusionTrace {
    std::vector<RegionTrace> regions;
    size_t input_evidence_count = 0;
};

/// Fused regions without stable identities yet (`stable_id` stays 0); assign
/// identities through `StableIdTracker` or `PerceptionSession::fuse`.
struct FusionOutput {
    std::vector<VisualRegion> regions;
    FusionTrace trace;
};

/// Tunables of deterministic evidence fusion (design section 16: determined,
/// configurable, explainable). All thresholds are inclusive.
struct FusionOptions {
    /// Coordinate space all evidence is converted into and outputs live in.
    CoordinateSpaceId target_space = CoordinateSpaceId::kOriented;
    /// Gate: pairs with IoU >= this value associate (subject to compatibility).
    double iou_threshold = 0.5;
    /// Gate: pairs whose intersection covers >= this fraction of the smaller
    /// area associate (subject to compatibility).
    double containment_threshold = 0.8;
    /// Source weights of the explicit confidence rule
    /// `confidence = sum(w*c) / sum(w)` over contributing evidence.
    float external_weight = 1.0F;
    float ocr_weight = 0.9F;
    float detector_weight = 0.8F;
    float template_weight = 0.7F;
    /// Output budget (RULE-06): more clusters fail with kBudgetExceeded.
    int32_t max_regions = 1024;
};

/// Deterministically fuses evidence in `target_space` (design section 16).
/// Pipeline: validate -> convert every item to the target space (frame
/// rotation when needed) -> gate and union compatible pairs (id-ordered scan,
/// observations recorded on actual merges) -> emit one `VisualRegion` per
/// cluster, ordered by smallest member evidence id, fields aggregated
/// deterministically. `frame` provides the rotation between kFrame and
/// kOriented; its other fields are unused.
///
/// Errors: kInvalidArgument (invalid thresholds/weights, item space outside
/// kFrame/kOriented, target space not in kFrame/kOriented), kCancelled/
/// kTimeout from `context` (polled between pair-scan stripes),
/// kBudgetExceeded for more clusters than `max_regions`. Never throws.
[[nodiscard]] Result<FusionOutput> fuse_evidence(const EvidenceSet& evidence, const Frame& frame,
                                                 const FusionOptions& options = {},
                                                 const ExecutionContext& context = {}) noexcept;

}  // namespace mirador

#endif  // MIRADOR_FUSION_HPP
