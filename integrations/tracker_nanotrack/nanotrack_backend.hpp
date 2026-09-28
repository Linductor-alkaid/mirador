#ifndef MIRADOR_INTEGRATIONS_TRACKER_NANOTRACK_BACKEND_HPP
#define MIRADOR_INTEGRATIONS_TRACKER_NANOTRACK_BACKEND_HPP

#include "ncnn_runtime.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/tracker_backend.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace mirador::integrations {

/// Model identity fields (see YoloModelIdentity): `model_id`/`model_revision`
/// flow into BackendInfo and therefore into capability cache keys (RULE-07).
struct NanoTrackModelIdentity {
    std::string model_id;
    std::string model_revision;
};

/// Options for the NanoTrack reference tracking backend (M7-12, DEC-015/
/// DEC-020). Paths come from the caller; weights are never embedded,
/// downloaded or logged (RULE-10, project standards section 9.2.4).
struct NanoTrackerOptions {
    /// Backbone ncnn model (`input` -> `output`, see the frozen model contract
    /// below). Shared by the template and the search branch, exactly like the
    /// reviewed reference port.
    std::string backbone_param_path;
    std::string backbone_bin_path;
    /// Head ncnn model (`input1`+`input2` -> `output1`+`output2`).
    std::string head_param_path;
    std::string head_bin_path;
    NanoTrackModelIdentity identity;
    /// Frozen NanoTrack geometry (the reviewed port's defaults). The template
    /// and search crops are resampled to `exemplar_size`/`instance_size`
    /// respectively; the head response maps must be `score_size` x
    /// `score_size`, one cell per `total_stride` search-crop pixels.
    int32_t exemplar_size = 127;
    int32_t instance_size = 255;
    int32_t score_size = 16;
    int32_t total_stride = 16;
    /// Context margin added around the target on both axes before the crop
    /// side is fixed (the port's `context_amount`).
    float context_amount = 0.5F;
    /// Size/ratio change penalty strength (the port's `penalty_k`).
    float penalty_k = 0.148F;
    /// Cosine window mixing weight (the port's `window_influence`).
    float window_influence = 0.462F;
    /// Per-frame reference-size learning rate (the port's `lr`).
    float size_learning_rate = 0.390F;
    /// Must stay 1: the tracker SPI requires per-sequence bit determinism
    /// (contract block 8) and ncnn only guarantees that single-threaded, so
    /// create() rejects every other value explicitly instead of quietly
    /// forfeiting the contract.
    int num_threads = 1;
    /// Byte budget for one initialize/update call, covering every allocation
    /// request the backend itself controls: the crop staging buffer, the
    /// forward output tensors, the stored template state and the decode
    /// window. Each request is checked before it happens and an overflow
    /// fails loudly with kBudgetExceeded (RULE-06). Transient allocations
    /// below the backend surface (the per-forward input copies in
    /// NcnnRuntime, its ncnn::Mat staging and ncnn's own intermediate/
    /// workspace buffers) are bounded by the frozen model contract's
    /// input/output geometry, are not separately chargeable from the backend
    /// surface, and follow the same accounting posture as the M5 reference
    /// backends.
    int64_t work_budget_bytes = int64_t{16} * 1024 * 1024;
};

/// NanoTrack reference tracking backend (M7-12, DEC-020/DEC-015): the
/// NanoTrack two-branch Siamese tracker as one ncnn model pair behind the
/// mirador::TrackerBackend SPI (M7-11, Experimental). Contract blocks 1-9 of
/// tracker_backend.hpp are implemented verbatim; where the wording below pins
/// a decision, that wording is the frozen reference decode contract.
///
/// Frozen model contract (the M5-04 YOLOv5-layout precedent: one canonical
/// layout; conversions whose raw output differs must normalize in their param
/// files, this reference does not reinvent head decoding). It is aligned with
/// the reviewed candidate port (docs/supply-chain/nanotrack.md, HonglinChu/
/// NanoTrack @ 76b1c67, Apache-2.0) at the pinned ncnn 20260526:
///   - Backbone model, standard ncnn layers only, fully convolutional (it is
///     fed `exemplar_size` crops at initialize and `instance_size` crops at
///     update): one input blob "input" (RGB, 3 x side x side, values as
///     packed 0..255 floats — the port's conversion bakes normalization into
///     the graph and applies none here), one output blob "output" (CHW float
///     feature map).
///   - Head model, standard ncnn layers only: input blobs "input1" (template
///     feature from the stored initialize forward) and "input2" (search
///     feature); output blobs "output1" (classification, 2 x score_size x
///     score_size, the foreground logit is channel 1) and "output2" (box
///     regression, 4 x score_size x score_size, channels = left/top/right/
///     bottom distances in search-crop pixels). Any other shape is a contract
///     violation and fails with kBackendFailure.
///
/// Session model (contract blocks 1/2): initialize crops
/// `initial_bounds` + context, runs the backbone once and stores the feature
/// (the template branch state) plus the reference size in the session; update
/// crops a search window around `prior_bounds`' center — the position prior
/// comes from the caller on every call and the session never fabricates one
/// — runs backbone + head and decodes:
///   1. per-cell score = sigmoid(cls foreground logit);
///   2. per-cell penalty = exp(-(size_change * ratio_change - 1) *
///      penalty_k) over the predicted ltrb box vs the reference size;
///   3. response = penalty * score * (1 - window_influence)
///      + cosine_window * window_influence; the winner is the first maximum
///      in row-major scan order (deterministic total order);
///   4. the winner's ltrb box maps back through the search crop
///      (center = prior center + (pred_center - instance_size/2)/scale_z,
///      size = pred_size/scale_z) into prepared-image pixel space, is clamped
///      into the image (a degenerate result is kBackendFailure) and reported
///      with confidence = winner score;
///   5. on success only, the stored reference size moves by
///      size_learning_rate toward the predicted size (the port's incidental
///      double application of the learning rate is normalized to this
///      canonical single update).
///
/// Frozen clamping semantics (verification-round documentation, an explicit
/// alignment point for the M7-13 position-evidence wiring): the reported
/// bounds are the predicted box clamped into the prepared image (contract
/// block 6 / DEC-021), and the reference-size update in step 5 deliberately
/// targets the clamped size — the session's scale state tracks what was
/// actually observable inside the frame, so a target partially outside the
/// prepared view converges the crop toward the visible part instead of
/// growing the search window after an unobservable prediction. The
/// alternative (feeding the unclamped predicted size into the state) is
/// deferred to the M7-13 semantic confirmation against design section 6.2;
/// changing it is a behavior change, not a wording fix.
///
/// Accepts kRgb8 only (the pipeline converts). All results live in prepared
/// pixel space; `ImageView::rotation` is never interpreted; input pixels are
/// never modified; per-sequence bit determinism holds (fixed scan orders,
/// num_threads pinned to 1, no wall clock, no randomness).
class NanoTrackerBackend final : public mirador::TrackerBackend {
public:
    static Result<NanoTrackerBackend> create(const NanoTrackerOptions& options);

    [[nodiscard]] mirador::BackendInfo info() const override;
    mirador::Result<std::unique_ptr<mirador::TrackerSession>> initialize(
        const mirador::ImageView& prepared_image, const mirador::TrackerInitRequest& request,
        const mirador::ExecutionContext& context) override;

private:
    NanoTrackerOptions options_;
    NcnnRuntime backbone_;
    NcnnRuntime head_;
    mirador::BackendInfo info_;
};

}  // namespace mirador::integrations

#endif  // MIRADOR_INTEGRATIONS_TRACKER_NANOTRACK_BACKEND_HPP
