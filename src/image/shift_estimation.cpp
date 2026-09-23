#include <mirador/shift_estimation.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

// Internal shared helper (M7-04): the square grayscale comparison thumbnail.
#include "gray_thumbnail.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace mirador {
namespace {

constexpr int32_t kMinThumbnailSize = 8;
constexpr int32_t kMaxThumbnailSize = 256;

Status validate_param_ranges(const ShiftEstimationParams& params) noexcept {
    if (params.thumbnail_size < kMinThumbnailSize || params.thumbnail_size > kMaxThumbnailSize) {
        return {ErrorCode::kInvalidArgument, "estimate_global_shift: thumbnail_size must be in [8, 256]"};
    }
    if (params.max_shift < 0 || 2 * params.max_shift > params.thumbnail_size - 1) {
        return {ErrorCode::kInvalidArgument,
                "estimate_global_shift: max_shift must be in [0, (thumbnail_size - 1) / 2]"};
    }
    if (params.work_budget_bytes <= 0) {
        return {ErrorCode::kInvalidArgument, "estimate_global_shift: work_budget_bytes must be positive"};
    }
    return {};
}

/// Structural validation of one stored change-detection signature (the
/// aggregate is publicly constructible, so the overload re-checks everything
/// the search and the frame mapping rely on).
Status validate_signature(const ChangeSignature& signature) noexcept {
    if (signature.thumbnail.empty()) {
        return {ErrorCode::kInvalidArgument, "estimate_global_shift: signature carries an empty thumbnail"};
    }
    if (signature.thumbnail.format() != PixelFormat::kGray8) {
        return {ErrorCode::kInvalidArgument, "estimate_global_shift: signature thumbnails must be kGray8"};
    }
    if (signature.thumbnail.width() != signature.thumbnail.height() ||
        signature.thumbnail.width() < kMinThumbnailSize || signature.thumbnail.width() > kMaxThumbnailSize) {
        return {ErrorCode::kInvalidArgument,
                "estimate_global_shift: signature thumbnails must be square [8, 256] change-detection thumbnails"};
    }
    if (const Result<void> valid_view = validate(signature.thumbnail.view()); !valid_view.ok()) {
        return {ErrorCode::kInvalidArgument, "estimate_global_shift: invalid stored thumbnail view"};
    }
    if (signature.frame_width < 1 || signature.frame_width > kMaxImageDimension || signature.frame_height < 1 ||
        signature.frame_height > kMaxImageDimension) {
        return {ErrorCode::kInvalidArgument,
                "estimate_global_shift: signature frame dimensions must be in [1, kMaxImageDimension]"};
    }
    return {};
}

/// Sums absolute luma differences between the previous thumbnail over the
/// central window anchored at `origin` and the current thumbnail displaced by
/// (dx, dy). Both windows stay inside the thumbnails by construction
/// (`origin == max_shift`, `side == size - 2 * max_shift`).
int64_t window_sad(const ImageView& previous_thumbnail, const ImageView& current_thumbnail, int32_t origin,
                   int32_t side, int32_t dy, int32_t dx) noexcept {
    int64_t sad = 0;
    for (int32_t y = origin; y < origin + side; ++y) {
        const std::byte* previous_row =
            previous_thumbnail.data + static_cast<int64_t>(y) * previous_thumbnail.row_stride_bytes;
        const std::byte* current_row =
            current_thumbnail.data + static_cast<int64_t>(y + dy) * current_thumbnail.row_stride_bytes;
        for (int32_t x = origin; x < origin + side; ++x) {
            const int32_t difference =
                std::to_integer<int>(previous_row[x]) - std::to_integer<int>(current_row[x + dx]);
            sad += difference < 0 ? -difference : difference;
        }
    }
    return sad;
}

/// Frozen winner order (frozen in the `ShiftEstimate` contract): SAD
/// ascending, then Chebyshev radius max(|dx|, |dy|) ascending, then dy, then
/// dx. Only called once an incumbent exists.
bool wins(int64_t sad, int32_t dx, int32_t dy, int64_t best_sad, int32_t best_dx, int32_t best_dy) noexcept {
    if (sad != best_sad) {
        return sad < best_sad;
    }
    const int32_t radius = std::max(std::abs(dx), std::abs(dy));
    const int32_t best_radius = std::max(std::abs(best_dx), std::abs(best_dy));
    if (radius != best_radius) {
        return radius < best_radius;
    }
    if (dy != best_dy) {
        return dy < best_dy;
    }
    return dx < best_dx;
}

/// Shared exhaustive translation search over two same-size square grayscale
/// thumbnails; every input has been validated by the callers. The window is
/// the central (size - 2 * max_shift)^2 region, so every candidate is scored
/// over identical pixel counts and nothing outside the thumbnails is ever
/// read. Pure integer arithmetic until the final confidence and mapping
/// evaluations (frozen in `ShiftEstimate`), so results are bit-identical
/// across platforms.
Result<ShiftEstimate> search_shift(const ImageView& previous_thumbnail, const ImageView& current_thumbnail,
                                   int32_t frame_width, int32_t frame_height, int32_t max_shift,
                                   const ExecutionContext& context) noexcept {
    const int32_t size = previous_thumbnail.width;
    const int32_t origin = max_shift;
    const int32_t side = size - 2 * max_shift;

    int32_t best_dx = 0;
    int32_t best_dy = 0;
    int64_t best_sad = 0;
    int64_t total_sad = 0;
    bool has_best = false;

    for (int32_t dy = -max_shift; dy <= max_shift; ++dy) {
        // One explicit stop per search row keeps the bounded loop responsive
        // without polling per pixel; a cancelled or timed-out call returns
        // only the Status, never a partial estimate.
        if (is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "estimate_global_shift: cancelled during the shift search");
        }
        if (deadline_reached(context)) {
            return Status(ErrorCode::kTimeout, "estimate_global_shift: deadline reached during the shift search");
        }
        for (int32_t dx = -max_shift; dx <= max_shift; ++dx) {
            const int64_t sad = window_sad(previous_thumbnail, current_thumbnail, origin, side, dy, dx);
            total_sad += sad;
            if (!has_best || wins(sad, dx, dy, best_sad, best_dx, best_dy)) {
                best_dx = dx;
                best_dy = dy;
                best_sad = sad;
                has_best = true;
            }
        }
    }

    // Frozen confidence (ShiftEstimate::confidence): scale-free peak
    // prominence over the SAD surface,
    // (mean_others - best) / (mean_others + best) evaluated as
    // (S_others - (C - 1) * best) / (S_others + (C - 1) * best). The value
    // only ever exists as integer sums until the single double division,
    // keeping it bit-identical across platforms; the denominator vanishes
    // exactly when every candidate ties (featureless frames, or the single
    // max_shift == 0 candidate), which the contract defines as confidence 0.
    float confidence = 0.0F;
    const int64_t candidate_total = static_cast<int64_t>(2 * max_shift + 1) * static_cast<int64_t>(2 * max_shift + 1);
    const int64_t others_tail = (candidate_total - 1) * best_sad;
    const int64_t others_sum = total_sad - best_sad;
    const int64_t denominator = others_sum + others_tail;
    if (denominator > 0) {
        const double prominence = static_cast<double>(others_sum - others_tail) / static_cast<double>(denominator);
        confidence = static_cast<float>(std::clamp(prominence, 0.0, 1.0));
    }

    // Frozen precision rule (ShiftEstimate::dx/dy): the exact rational
    // thumbnail_shift * frame_dimension / thumbnail_size, evaluated in double
    // and narrowed to float.
    ShiftEstimate estimate;
    estimate.thumbnail_dx = best_dx;
    estimate.thumbnail_dy = best_dy;
    estimate.dx =
        static_cast<float>(static_cast<double>(best_dx) * static_cast<double>(frame_width) / static_cast<double>(size));
    estimate.dy = static_cast<float>(static_cast<double>(best_dy) * static_cast<double>(frame_height) /
                                     static_cast<double>(size));
    estimate.confidence = confidence;
    return estimate;
}

}  // namespace

Result<ShiftEstimate> estimate_global_shift(const ImageView& previous, const ImageView& current,
                                            const ShiftEstimationParams& params,
                                            const ExecutionContext& context) noexcept {
    if (const Result<void> valid_previous = validate(previous); !valid_previous.ok()) {
        return valid_previous.status();
    }
    if (const Result<void> valid_current = validate(current); !valid_current.ok()) {
        return valid_current.status();
    }
    if (previous.width != current.width || previous.height != current.height) {
        return Status(ErrorCode::kInvalidArgument,
                      "estimate_global_shift: views must share their presented dimensions");
    }
    if (const Status ranges = validate_param_ranges(params); !ranges.ok()) {
        return ranges;
    }
    // Entry stop: argument validity takes precedence over cancellation.
    if (is_cancelled(context)) {
        return Status(ErrorCode::kCancelled, "estimate_global_shift: cancelled before the shift search");
    }
    if (deadline_reached(context)) {
        return Status(ErrorCode::kTimeout, "estimate_global_shift: deadline reached before the shift search");
    }

    const Result<ImageBuffer> previous_thumbnail =
        image_internal::gray_thumbnail(previous, params.thumbnail_size, params.work_budget_bytes);
    if (!previous_thumbnail.ok()) {
        return previous_thumbnail.status();
    }
    const Result<ImageBuffer> current_thumbnail =
        image_internal::gray_thumbnail(current, params.thumbnail_size, params.work_budget_bytes);
    if (!current_thumbnail.ok()) {
        return current_thumbnail.status();
    }
    return search_shift(previous_thumbnail.value().view(), current_thumbnail.value().view(), previous.width,
                        previous.height, params.max_shift, context);
}

Result<ShiftEstimate> estimate_global_shift(const ChangeSignature& previous, const ChangeSignature& current,
                                            const ShiftEstimationParams& params,
                                            const ExecutionContext& context) noexcept {
    if (const Status ranges = validate_param_ranges(params); !ranges.ok()) {
        return ranges;
    }
    if (const Status previous_valid = validate_signature(previous); !previous_valid.ok()) {
        return previous_valid;
    }
    if (const Status current_valid = validate_signature(current); !current_valid.ok()) {
        return current_valid;
    }
    if (previous.frame_width != current.frame_width || previous.frame_height != current.frame_height) {
        return Status(ErrorCode::kInvalidArgument,
                      "estimate_global_shift: signatures must share their stored frame dimensions");
    }
    // The search derives every window index from the previous thumbnail's
    // edge, so differently sized stored thumbnails would read past the
    // current thumbnail's allocation (or silently misalign the reverse way);
    // both directions are rejected before any pixel is read (the
    // detect_change overload's matching check).
    if (previous.thumbnail.width() != current.thumbnail.width() ||
        previous.thumbnail.height() != current.thumbnail.height()) {
        return Status(ErrorCode::kInvalidArgument, "estimate_global_shift: signature thumbnails differ in size");
    }
    const int32_t size = previous.thumbnail.width();
    if (2 * params.max_shift > size - 1) {
        return Status(ErrorCode::kInvalidArgument,
                      "estimate_global_shift: max_shift exceeds the signatures' stored thumbnails");
    }
    // Entry stop: argument validity takes precedence over cancellation.
    if (is_cancelled(context)) {
        return Status(ErrorCode::kCancelled, "estimate_global_shift: cancelled before the shift search");
    }
    if (deadline_reached(context)) {
        return Status(ErrorCode::kTimeout, "estimate_global_shift: deadline reached before the shift search");
    }
    // The search allocates nothing, so this path has no budget failure mode.
    return search_shift(previous.thumbnail.view(), current.thumbnail.view(), previous.frame_width,
                        previous.frame_height, params.max_shift, context);
}

}  // namespace mirador
