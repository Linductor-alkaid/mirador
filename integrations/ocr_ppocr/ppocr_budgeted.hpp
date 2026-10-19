#ifndef MIRADOR_INTEGRATIONS_PPOCR_BUDGETED_HPP
#define MIRADOR_INTEGRATIONS_PPOCR_BUDGETED_HPP

#include "ncnn_runtime.hpp"
#include "ppocr_backend.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/result.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace mirador::integrations {

/// Nanosecond stage accounting of the last `recognize_budgeted` call on this
/// backend (M8-01 benchmark diagnostics per the OCR budget protocol; never
/// part of the contract result, never logged by the library). Single-threaded
/// like the backend itself.
struct PpOcrStageTimings {
    int64_t det_canvas_ns = 0;         ///< det canvas build (area resize + pad)
    int64_t det_pack_normalize_ns = 0; ///< det tensor pack + plane swap + normalize
    int64_t det_forward_ns = 0;        ///< det ncnn forward
    int64_t det_postprocess_ns = 0;    ///< probability byte conversion + DB postprocess + unpad
    int64_t select_ns = 0;             ///< reading order, priorities, execution order
    int64_t crop_ns = 0;               ///< line crops
    int64_t rec_preprocess_ns = 0;     ///< sum over rec units
    int64_t rec_forward_ns = 0;        ///< sum over rec units
    int64_t rec_decode_ns = 0;         ///< sum over rec units
    int64_t finalize_ns = 0;           ///< counting + report assembly
};

/// Options of the budgeted PP-OCR pipeline (DEC-025, M8-03): the frozen det/rec
/// reference stages plus the selection/budget calibration knobs. Model paths
/// come from the caller; weights are never embedded or downloaded (DEC-015).
struct PpOcrBudgetedOptions {
    PpOcrDetOptions det;
    PpOcrRecOptions rec;
    /// Measured upper bound of one rec atomic unit (crop + preprocess +
    /// forward + decode) used together with a request's `finalize_reserve`:
    /// a unit is not started when the remaining time no longer fits
    /// `rec_unit_upper_bound + finalize_reserve` (DEC-025 section 3). The
    /// initial value 0 disables the unit-bound part of the guard; M8-01
    /// measures and M8-06 freezes a reference value. UNVALIDATED placeholder.
    std::chrono::steady_clock::duration rec_unit_upper_bound{0};
};

/// Budgeted PP-OCR detection + recognition reference backend (M8-03, DEC-025):
/// the frozen det stage followed by bounded recognition — deterministic
/// reading order, caller-priority aware selection of at most `max_rec_lines`
/// lines, soft-budget stops that return partial reports, per-line completion
/// states and explicit stop diagnostics. Deferred lines keep queryable det
/// geometry; the all-or-nothing `PpOcrBackend` and its semantics stay
/// unchanged. Accepts kRgb8 only. Determinism: equal inputs with equal fixed
/// configuration (K/selection/priorities, no time stop) select, execute and
/// report identically; time-stopped runs keep the priority prefix.
class PpOcrBudgetedBackend final : public BudgetedOcrBackend {
public:
    static Result<PpOcrBudgetedBackend> create(const PpOcrBudgetedOptions& options);

    [[nodiscard]] BackendInfo info() const override;
    Result<OcrBatchResult> recognize_budgeted(const ImageView& prepared_image, const OcrBatchRequest& request,
                                              const ExecutionContext& context) override;

    /// Stage split of the most recent `recognize_budgeted` call (diagnostics).
    [[nodiscard]] const PpOcrStageTimings& last_stage_timings() const noexcept { return timings_; }

private:
    PpOcrBudgetedOptions options_;
    NcnnRuntime det_runtime_;
    NcnnRuntime rec_runtime_;
    std::vector<std::string> charset_;  ///< class 1 .. N (class 0 is the blank)
    BackendInfo info_;
    PpOcrStageTimings timings_;
};

}  // namespace mirador::integrations

#endif  // MIRADOR_INTEGRATIONS_PPOCR_BUDGETED_HPP
