#ifndef MIRADOR_SHIFT_ESTIMATION_HPP
#define MIRADOR_SHIFT_ESTIMATION_HPP

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador {

/// Experimental (M7, DEC-019): signatures may change within M7 until the
/// go/no-go freeze (the DEC-017/DEC-018 promotion pattern); the contract is
/// not covered by the compatibility promise until then. Global shift
/// estimation primitive of the tracking pipeline (object-tracking design
/// section 6.3, M7-04): estimates the global translation between two frames
/// of one image source at low resolution, pure CPU and deterministic. Pure
/// function: it never touches tracker state, never decides a
/// layout-generation advance and never classifies change — applying the
/// reported shift (motion compensation, generation decisions) belongs to the
/// M7-07 pipeline. No threads, no timers, no filesystem, no network, no
/// logging of pixel content (RULE-03, RULE-10); image data stays in memory.

/// Tunables of the global shift estimator. The defaults are development
/// smoke values and are calibrated against the M7-09 harness (DEC-019
/// section 5); any change afterwards is recorded there. Invalid values fail
/// with kInvalidArgument.
struct ShiftEstimationParams {
    /// Edge length of the square grayscale comparison thumbnails in [8, 256].
    /// Both views are resampled with the exact deterministic pipeline of the
    /// change-detection thumbnails (integer area resample + BT.601 luma), so
    /// a caller running `detect_change` at the same size compares identical
    /// pixel data. Default 64 — the `ChangeDetectionParams::thumbnail_size`
    /// default: the search resolution matches the existing block-diff
    /// resolution.
    int32_t thumbnail_size = 64;
    /// Inclusive search bound in thumbnail pixels: every integer shift
    /// (dx, dy) with |dx| <= max_shift and |dy| <= max_shift is evaluated.
    /// [0, (thumbnail_size - 1) / 2] so the fixed central comparison window
    /// (see `estimate_global_shift`) stays at least one pixel wide. Default
    /// 16 = a quarter of the default thumbnail edge: displacements up to a
    /// quarter of the frame extent per step, far above per-frame terminal
    /// scroll/pan, at roughly 10^6 pixel operations per call.
    int32_t max_shift = 16;
    /// Byte budget that every internal image allocation request of this
    /// primitive must fit (RULE-06): the resampled intermediate and the
    /// grayscale thumbnail of each frame are checked against it before
    /// allocation, and the search itself allocates nothing. Must be > 0. The
    /// default accommodates a 256 x 256 thumbnail from any single-plane format
    /// (worst-case request 256^2 * 4 bytes). Accounting note: the underlying
    /// deterministic area resample additionally allocates its fixed
    /// box-weight tables sized by the source dimensions (at most two int64
    /// entries per source row/column, bounded by kMaxImageDimension, M1
    /// behavior shared with `detect_change`); those tables are not counted
    /// against this budget — only allocation failure surfaces as
    /// kBudgetExceeded there.
    int64_t work_budget_bytes = int64_t{512} * 1024;
};

/// Outcome of one global shift estimation. The primitive is stateless, so
/// everything it concluded is in this record.
struct ShiftEstimate {
    /// Winning integer shift in thumbnail pixels, in
    /// [-max_shift, max_shift] on both axes.
    int32_t thumbnail_dx = 0;
    int32_t thumbnail_dy = 0;
    /// Reported shift in frame coordinates (frozen precision rule): the exact
    /// rational `thumbnail_dx * frame_width / thumbnail_size` (respectively
    /// `thumbnail_dy * frame_height / thumbnail_size`), evaluated in double
    /// and narrowed to float (round to nearest). Direction: the shift
    /// translates previous-frame coordinates into current-frame coordinates,
    /// p_current = p_previous + (dx, dy) — the displacement the frame content
    /// moved by. Compose it into a coordinate chain with
    /// `make_translation(dx, dy, from, to)` and recover the opposite mapping
    /// with `inverse` (RULE-05).
    float dx = 0.0F;
    float dy = 0.0F;
    /// Peak-prominence confidence in [0, 1] (frozen semantics): how far the
    /// winning alignment stands out from the rest of the searched shift
    /// surface — `confidence = (mean_SAD_others - SAD_best) /
    /// (mean_SAD_others + SAD_best)`, where SAD_best is the winner's sum of
    /// absolute luma differences over the comparison window and
    /// mean_SAD_others averages the SAD of every other candidate (equivalently
    /// `(S_others - (C - 1) * SAD_best) / (S_others + (C - 1) * SAD_best)`
    /// with C the candidate count, the form the implementation evaluates).
    /// Built from integer sums and one final double division, narrowed to
    /// float. It is exactly 0 when a single candidate exists
    /// (`max_shift == 0`) or when every candidate ties (featureless frames —
    /// the denominator vanishes only there), and it is exactly 1 when the
    /// winner needs no comparison to stand out (SAD_best == 0, e.g.
    /// pixel-identical frames at thumbnail resolution). Tied-zero caveat:
    /// candidates that also score SAD 0 count into mean_SAD_others without
    /// lowering the value, so periodic content displaced by an exact period
    /// (many zero-SAD alignments) still reports confidence == 1.0 — do not
    /// read confidence == 1.0 as an unambiguous peak; the winner's total
    /// order (smallest Chebyshev radius, then dy, then dx) is what keeps the
    /// reported shift deterministic there. It measures distinctiveness of
    /// the alignment, not scene quality; consumers gate compensation on it
    /// (M7-07) and calibrate thresholds in M7-09.
    float confidence = 0.0F;
};

/// Estimates the global translation between two frames of one image source
/// (object-tracking design section 6.3, M7-04). Both views are resampled to
/// square grayscale thumbnails (`params.thumbnail_size` per side, the exact
/// change-detection pipeline), and every integer thumbnail shift (dx, dy)
/// with |dx|, |dy| <= `params.max_shift` is scored by the sum of absolute
/// luma differences (SAD) over a fixed central window of
/// (thumbnail_size - 2 * max_shift)^2 thumbnail pixels — every candidate is
/// therefore scored over identical pixel counts, keeping scores directly
/// comparable and the work bounded regardless of frame size (RULE-06).
///
/// The winner is the minimum-SAD candidate under the total order (SAD, then
/// Chebyshev radius max(|dx|, |dy|), then dy, then dx): ties prefer the
/// smallest motion, and equal scores at equal radius resolve to the smallest
/// (dy, dx). The search itself is pure integer arithmetic; see
/// `ShiftEstimate` for the frozen precision and confidence rules.
///
/// The views must share their presented dimensions (`width`/`height`): the
/// frame-space mapping is defined for one shared scale only. Rotation and
/// format metadata never affect the comparison — it always runs on presented
/// pixels, the same convention as `detect_change`; when frames are presented
/// rotated, compose the reported shift with `make_rotation` chains as usual
/// (RULE-05).
///
/// Determinism: equal inputs produce bit-identical results across platforms.
///
/// Errors: kInvalidArgument for invalid views, views with different presented
/// dimensions, or out-of-range parameters (thumbnail_size outside [8, 256],
/// max_shift outside [0, (thumbnail_size - 1) / 2], work_budget_bytes <= 0);
/// kBudgetExceeded when an internal allocation request exceeds
/// `params.work_budget_bytes` or the allocation fails; kCancelled/kTimeout
/// from `context` — checked at entry and once per search row, because the
/// search is a bounded but non-trivial loop (same explicit conversion as the
/// M7-03 gate); a cancelled or timed-out call returns only the Status, never
/// a partial estimate. Validation errors take precedence over cancellation.
/// Never throws.
[[nodiscard]] Result<ShiftEstimate> estimate_global_shift(const ImageView& previous, const ImageView& current,
                                                          const ShiftEstimationParams& params,
                                                          const ExecutionContext& context = {}) noexcept;

/// Same estimation over two previously built change-detection signatures (the
/// M2 overload pattern of `detect_change`): the search runs directly on the
/// stored grayscale thumbnails, so nothing is resampled or allocated and the
/// result is bit-identical to the view-based overload on the same frames at
/// the same thumbnail size. The stored thumbnails define the search
/// resolution; `params.thumbnail_size` and `params.work_budget_bytes` stay
/// unused by the search but must still hold documented values (one params
/// struct stays well-formed for both overloads, mirroring `detect_change`),
/// and `params.max_shift` must additionally fit the stored thumbnails.
/// `dx`/`dy` are mapped with the signatures' stored frame dimensions.
///
/// Errors: kInvalidArgument for out-of-range parameters, empty or non-square
/// stored thumbnails, stored thumbnails that are not kGray8, differ in size
/// or fall outside the [8, 256] change-detection range, invalid stored
/// thumbnail views, or signatures with different or non-positive stored frame
/// dimensions; kCancelled/kTimeout from `context` as above, with the same
/// precedence. Never throws.
[[nodiscard]] Result<ShiftEstimate> estimate_global_shift(const ChangeSignature& previous,
                                                          const ChangeSignature& current,
                                                          const ShiftEstimationParams& params,
                                                          const ExecutionContext& context = {}) noexcept;

}  // namespace mirador

#endif  // MIRADOR_SHIFT_ESTIMATION_HPP
