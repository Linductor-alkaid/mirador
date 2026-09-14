#ifndef MIRADOR_CROP_REFINE_HPP
#define MIRADOR_CROP_REFINE_HPP

#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mirador {

/// Small-target crop-refine combinator parameters (design section 14, DEC-014
/// reference adaptation). A candidate is refined when
/// `confidence < refine_confidence_below` or its box area divided by the
/// source area is below `small_box_area_ratio` (0 disables the area rule).
/// Refinement expands the box by `expand_ratio` (relative to the box size,
/// clamped to the source), crops it, optionally resamples it so the long side
/// reaches `refine_target_side` (0 keeps the crop size), and re-runs the
/// backend on the prepared crop.
struct CropRefineParams {
    /// Per-side expansion relative to the box size, in [0, 4].
    float expand_ratio = 0.25F;
    /// Long side of the prepared crop; 0 keeps the crop size. In [0, 4096].
    int32_t refine_target_side = 0;
    /// Refine proposals below this confidence, in [0, 1].
    double refine_confidence_below = 0.5;
    /// Also refine proposals smaller than this fraction of the source area,
    /// in [0, 1].
    double small_box_area_ratio = 0.0;
    /// At most this many candidates are refined, chosen by ascending
    /// confidence (ties by input index); the rest pass through untouched —
    /// an explicit, deterministic drop (RULE-06). >= 0.
    int32_t max_refine_candidates = 32;
    /// Refined results below this confidence are dropped, in [0, 1).
    float min_refined_confidence = 0.0F;
    /// Forwarded to the backend as the detection request's `min_confidence`.
    float backend_min_confidence = 0.0F;
    /// Forwarded verbatim into `DetectionRequest::backend_params`.
    std::string backend_params;
    /// Budget for the per-candidate crop/resize/convert buffers (RULE-06).
    int64_t per_crop_budget_bytes = int64_t{4} * 1024 * 1024;
};

/// Refines small or low-confidence detections by re-running a detector on
/// high-resolution crops (design section 14: the coarse-candidate / refine
/// combinator; M3-08). The output preserves the input order: untouched
/// proposals pass through unchanged, and each refined candidate is replaced
/// by the backend's results mapped back to source-pixel space through the
/// inverse of the exact crop+resize chain (RULE-05). NV12 sources are rejected
/// for now (chroma is dropped by resampling). The input view and span are
/// never modified (RULE-04).
///
/// Coordinate spaces: the source view is treated as presented space
/// (kOriented), crops are kCropped, the resampled crop is kModelInput.
///
/// Errors: kInvalidArgument (invalid view or parameters, NV12 source),
/// kUnsupportedFormat (NV12 source, or no backend-accepted format reachable),
/// kBackendUnavailable (null backend or invalid `info()`), kBackendFailure /
/// kCancelled / kTimeout from the backend or context, kBudgetExceeded
/// (per-candidate buffers). Never throws.
[[nodiscard]] Result<std::vector<DetectionRegion>>
refine_small_detections(const ImageView& source, DetectorBackend* backend, std::span<const DetectionRegion> initial,
                        const CropRefineParams& params, const ExecutionContext& context = {});

}  // namespace mirador

#endif  // MIRADOR_CROP_REFINE_HPP
