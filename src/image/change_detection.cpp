#include <mirador/change_detection.hpp>

#include <mirador/color_convert.hpp>
#include <mirador/fingerprint.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirador {
namespace {

// Fixed internal budget (RULE-06): two <=256x256 thumbnails plus block bookkeeping
// stay far below it regardless of frame size.
constexpr int64_t kChangeDetectionBudgetBytes = int64_t{512} * 1024;

constexpr int32_t kMinThumbnailSize = 8;
constexpr int32_t kMaxThumbnailSize = 256;
constexpr int32_t kMaxBlocksPerSide = 64;

Result<ImageBuffer> gray_thumbnail(const ImageView& src, int32_t size) noexcept {
    Result<ImageBuffer> small = resize_area(src, size, size, kChangeDetectionBudgetBytes);
    if (!small.ok()) {
        return small.status();
    }
    ImageBuffer thumb = small.take_value();
    if (thumb.format() == PixelFormat::kGray8) {
        return thumb;
    }
    return convert_color(thumb.view(), PixelFormat::kGray8, kChangeDetectionBudgetBytes);
}

Status validate_param_ranges(const ChangeDetectionParams& params) {
    if (!std::isfinite(params.fingerprint_similarity_threshold) || params.fingerprint_similarity_threshold < 0.0 ||
        params.fingerprint_similarity_threshold > 1.0) {
        return {ErrorCode::kInvalidArgument, "detect_change: fingerprint_similarity_threshold must be in [0, 1]"};
    }
    if (params.thumbnail_size < kMinThumbnailSize || params.thumbnail_size > kMaxThumbnailSize) {
        return {ErrorCode::kInvalidArgument, "detect_change: thumbnail_size must be in [8, 256]"};
    }
    if (params.block_size < 1 || params.block_size > params.thumbnail_size) {
        return {ErrorCode::kInvalidArgument, "detect_change: block_size must be in [1, thumbnail_size]"};
    }
    if ((params.thumbnail_size + params.block_size - 1) / params.block_size > kMaxBlocksPerSide) {
        return {ErrorCode::kInvalidArgument, "detect_change: block grid exceeds 64 blocks per side"};
    }
    if (params.block_diff_threshold < 0 || params.block_diff_threshold > 255) {
        return {ErrorCode::kInvalidArgument, "detect_change: block_diff_threshold must be in [0, 255]"};
    }
    if (!std::isfinite(params.global_area_ratio) || params.global_area_ratio <= 0.0 || params.global_area_ratio > 1.0) {
        return {ErrorCode::kInvalidArgument, "detect_change: global_area_ratio must be in (0, 1]"};
    }
    return Status::success();
}

Status validate_ignored_regions(const ChangeDetectionParams& params, int32_t frame_width, int32_t frame_height) {
    for (const RectI& region : params.ignored_regions) {
        if (!is_valid(region) || region.x < 0 || region.y < 0 || region.x + region.width > frame_width ||
            region.y + region.height > frame_height) {
            return {ErrorCode::kInvalidArgument,
                    "detect_change: ignored regions must be valid rects inside the current frame"};
        }
    }
    return Status::success();
}

bool is_ignored(const RectI& block_rect, const std::vector<RectI>& ignored_regions) noexcept {
    return std::ranges::any_of(ignored_regions,
                               [block_rect](const RectI& region) { return contains(region, block_rect); });
}

/// Maps a thumbnail-space span to the current-frame rectangle that covers it:
/// floor on the leading edge, ceil on the trailing edge (integer-exact).
RectI frame_rect_for_span(int64_t tx0, int64_t tx1, int64_t ty0, int64_t ty1, int32_t frame_width, int32_t frame_height,
                          int32_t thumbnail_size) noexcept {
    const auto tw = static_cast<int64_t>(thumbnail_size);
    const auto fw = static_cast<int64_t>(frame_width);
    const auto fh = static_cast<int64_t>(frame_height);
    const auto fx0 = tx0 * fw / tw;
    const auto fx1 = (tx1 * fw + tw - 1) / tw;
    const auto fy0 = ty0 * fh / tw;
    const auto fy1 = (ty1 * fh + tw - 1) / tw;
    return RectI{static_cast<int32_t>(fx0), static_cast<int32_t>(fy0), static_cast<int32_t>(fx1 - fx0),
                 static_cast<int32_t>(fy1 - fy0)};
}

/// Sums absolute luma differences over the thumbnail rect [x0, x1) x [y0, y1).
int64_t region_diff_sum(const ImageView& previous, const ImageView& current, int32_t x0, int32_t y0, int32_t x1,
                        int32_t y1) noexcept {
    int64_t diff_sum = 0;
    for (int32_t y = y0; y < y1; ++y) {
        const std::byte* previous_row = previous.data + static_cast<int64_t>(y) * previous.row_stride_bytes;
        const std::byte* current_row = current.data + static_cast<int64_t>(y) * current.row_stride_bytes;
        for (int32_t x = x0; x < x1; ++x) {
            const int32_t difference = std::to_integer<int>(previous_row[x]) - std::to_integer<int>(current_row[x]);
            diff_sum += difference < 0 ? -difference : difference;
        }
    }
    return diff_sum;
}

/// Marks every non-ignored block whose mean absolute luma difference reaches the
/// threshold (integer-exact: sum >= threshold * count); returns the number of
/// marked blocks. `thumbnail_size` is the stored thumbnails' edge length;
/// `frame_width`/`frame_height` are the current frame's presented dimensions
/// (the block rects used for the ignore check live in frame space).
int64_t mark_changed_blocks(const ImageView& previous_thumbnail, const ImageView& current_thumbnail,
                            const ChangeDetectionParams& params, int32_t thumbnail_size, int32_t blocks_side,
                            int32_t frame_width, int32_t frame_height, std::vector<uint8_t>& changed) {
    const int32_t block_size = params.block_size;
    int64_t changed_blocks = 0;
    for (int32_t by = 0; by < blocks_side; ++by) {
        for (int32_t bx = 0; bx < blocks_side; ++bx) {
            const int32_t x0 = bx * block_size;
            const int32_t y0 = by * block_size;
            const int32_t x1 = std::min(x0 + block_size, thumbnail_size);
            const int32_t y1 = std::min(y0 + block_size, thumbnail_size);
            const RectI block_rect = frame_rect_for_span(x0, x1, y0, y1, frame_width, frame_height, thumbnail_size);
            if (is_ignored(block_rect, params.ignored_regions)) {
                continue;
            }
            const auto pixels = static_cast<int64_t>(y1 - y0) * static_cast<int64_t>(x1 - x0);
            if (region_diff_sum(previous_thumbnail, current_thumbnail, x0, y0, x1, y1) >=
                static_cast<int64_t>(params.block_diff_threshold) * pixels) {
                changed[static_cast<size_t>(by) * blocks_side + bx] = 1;
                ++changed_blocks;
            }
        }
    }
    return changed_blocks;
}

/// Flood-fills the 8-connected component of changed blocks containing
/// `start_index` and widens the component's block bounds. The center neighbor
/// needs no special case: it is already visited.
void visit_component(const std::vector<uint8_t>& changed, std::vector<uint8_t>& visited, int64_t start_index,
                     int32_t blocks_side, int32_t& min_bx, int32_t& max_bx, int32_t& min_by, int32_t& max_by) {
    std::vector<int64_t> stack;
    stack.push_back(start_index);
    visited[static_cast<size_t>(start_index)] = 1;
    while (!stack.empty()) {
        const int64_t index = stack.back();
        stack.pop_back();
        const auto bx = static_cast<int32_t>(index % blocks_side);
        const auto by = static_cast<int32_t>(index / blocks_side);
        min_bx = std::min(min_bx, bx);
        max_bx = std::max(max_bx, bx);
        min_by = std::min(min_by, by);
        max_by = std::max(max_by, by);
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const int32_t neighbor_bx = bx + dx;
                const int32_t neighbor_by = by + dy;
                if (neighbor_bx < 0 || neighbor_by < 0 || neighbor_bx >= blocks_side || neighbor_by >= blocks_side) {
                    continue;
                }
                const auto neighbor_index = static_cast<size_t>(neighbor_by) * blocks_side + neighbor_bx;
                if (changed[neighbor_index] != 0 && visited[neighbor_index] == 0) {
                    visited[neighbor_index] = 1;
                    stack.push_back(static_cast<int64_t>(neighbor_index));
                }
            }
        }
    }
}

/// Appends the frame-space bounding rect of every 8-connected changed-block
/// component, in scan order of the component's first block.
void append_component_rects(const std::vector<uint8_t>& changed, std::vector<uint8_t>& visited, int32_t blocks_side,
                            const ChangeDetectionParams& params, int32_t thumbnail_size, int32_t frame_width,
                            int32_t frame_height, std::vector<RectI>& regions) {
    const int32_t block_size = params.block_size;
    for (int32_t by = 0; by < blocks_side; ++by) {
        for (int32_t bx = 0; bx < blocks_side; ++bx) {
            const auto index = static_cast<size_t>(by) * blocks_side + bx;
            if (changed[index] == 0 || visited[index] != 0) {
                continue;
            }
            int32_t min_bx = bx;
            int32_t max_bx = bx;
            int32_t min_by = by;
            int32_t max_by = by;
            visit_component(changed, visited, static_cast<int64_t>(index), blocks_side, min_bx, max_bx, min_by, max_by);
            regions.push_back(frame_rect_for_span(
                static_cast<int64_t>(min_bx) * block_size,
                std::min(static_cast<int64_t>(max_bx + 1) * block_size, static_cast<int64_t>(thumbnail_size)),
                static_cast<int64_t>(min_by) * block_size,
                std::min(static_cast<int64_t>(max_by + 1) * block_size, static_cast<int64_t>(thumbnail_size)),
                frame_width, frame_height, thumbnail_size));
        }
    }
}

/// Layer-1 early-exit report: shared by both entry points so the echoed
/// fingerprints, similarity and thresholds stay bit-identical.
ChangeReport make_early_exit_report(uint64_t previous_fingerprint, uint64_t current_fingerprint,
                                    const ChangeDetectionParams& params) noexcept {
    ChangeReport report;
    report.previous_fingerprint = previous_fingerprint;
    report.current_fingerprint = current_fingerprint;
    report.frame_similarity = fingerprint_similarity(previous_fingerprint, current_fingerprint);
    report.thresholds = ChangeThresholds{params.fingerprint_similarity_threshold, params.block_diff_threshold,
                                         params.global_area_ratio};
    report.reason = ChangeReason::kFingerprintEarlyExit;
    return report;
}

/// Shared layer-1 + layer-2 comparison over two built signatures. Callers have
/// validated parameters, thumbnail agreement and ignored regions; the view-based
/// entry point delegates the block-diff path here so both paths stay
/// bit-identical. Layer-1 fingerprints are already stored in the signatures.
Result<ChangeReport> detect_change_signatures(const ChangeSignature& previous, const ChangeSignature& current,
                                              const ChangeDetectionParams& params) noexcept {
    // Layer 1: frames indistinguishable at fingerprint resolution keep every
    // cached result and skip the block diff entirely.
    if (const double similarity = fingerprint_similarity(previous.fingerprint, current.fingerprint);
        similarity >= params.fingerprint_similarity_threshold) {
        return make_early_exit_report(previous.fingerprint, current.fingerprint, params);
    }

    // Layer 2: compare square grayscale thumbnails block by block.
    ChangeReport report;
    report.previous_fingerprint = previous.fingerprint;
    report.current_fingerprint = current.fingerprint;
    report.frame_similarity = fingerprint_similarity(previous.fingerprint, current.fingerprint);
    report.thresholds = ChangeThresholds{params.fingerprint_similarity_threshold, params.block_diff_threshold,
                                         params.global_area_ratio};
    report.reason = ChangeReason::kBlockDiff;
    const int32_t thumbnail_size = previous.thumbnail.width();
    const int32_t blocks_side = (thumbnail_size + params.block_size - 1) / params.block_size;
    try {
        const auto block_count = static_cast<size_t>(blocks_side) * static_cast<size_t>(blocks_side);
        std::vector<uint8_t> changed(block_count, 0);
        std::vector<uint8_t> visited(block_count, 0);
        const int64_t changed_blocks =
            mark_changed_blocks(previous.thumbnail.view(), current.thumbnail.view(), params, thumbnail_size,
                                blocks_side, current.frame_width, current.frame_height, changed);
        append_component_rects(changed, visited, blocks_side, params, thumbnail_size, current.frame_width,
                               current.frame_height, report.changed_regions);

        report.changed_area_ratio = static_cast<double>(changed_blocks) / static_cast<double>(block_count);
        if (changed_blocks == 0) {
            report.classification = ChangeClassification::kNone;
        } else if (report.changed_area_ratio >= params.global_area_ratio) {
            report.classification = ChangeClassification::kGlobal;
        } else {
            report.classification = ChangeClassification::kPartial;
        }
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "detect_change: internal allocation failed");
    }
    return report;
}

}  // namespace

Result<ChangeSignature> make_change_signature(const ImageView& frame, const ChangeDetectionParams& params) noexcept {
    if (const Result<void> valid_frame = validate(frame); !valid_frame.ok()) {
        return valid_frame.status();
    }
    if (const Status ranges_status = validate_param_ranges(params); !ranges_status.ok()) {
        return ranges_status;
    }

    const Result<uint64_t> frame_fingerprint = fingerprint(frame);
    if (!frame_fingerprint.ok()) {
        return frame_fingerprint.status();
    }
    Result<ImageBuffer> thumbnail = gray_thumbnail(frame, params.thumbnail_size);
    if (!thumbnail.ok()) {
        return thumbnail.status();
    }

    ChangeSignature signature;
    signature.frame_width = frame.width;
    signature.frame_height = frame.height;
    signature.fingerprint = frame_fingerprint.value();
    signature.thumbnail = thumbnail.take_value();
    return signature;
}

Result<ChangeReport> detect_change(const ChangeSignature& previous, const ChangeSignature& current,
                                   const ChangeDetectionParams& params) noexcept {
    if (const Status ranges_status = validate_param_ranges(params); !ranges_status.ok()) {
        return ranges_status;
    }
    if (previous.thumbnail.empty() || current.thumbnail.empty()) {
        return Status(ErrorCode::kInvalidArgument, "detect_change: signatures carry empty thumbnails");
    }
    if (previous.thumbnail.width() != current.thumbnail.width() ||
        previous.thumbnail.height() != current.thumbnail.height()) {
        return Status(ErrorCode::kInvalidArgument, "detect_change: signature thumbnails differ in size");
    }
    const int32_t thumbnail_size = current.thumbnail.width();
    if (params.block_size > thumbnail_size) {
        return Status(ErrorCode::kInvalidArgument, "detect_change: block_size exceeds the signatures' thumbnail");
    }
    if ((thumbnail_size + params.block_size - 1) / params.block_size > kMaxBlocksPerSide) {
        return Status(ErrorCode::kInvalidArgument, "detect_change: block grid exceeds 64 blocks per side");
    }
    if (const Status ignored_status = validate_ignored_regions(params, current.frame_width, current.frame_height);
        !ignored_status.ok()) {
        return ignored_status;
    }
    return detect_change_signatures(previous, current, params);
}

Result<ChangeReport> detect_change(const ImageView& previous, const ImageView& current,
                                   const ChangeDetectionParams& params) noexcept {
    if (const Result<void> valid_previous = validate(previous); !valid_previous.ok()) {
        return valid_previous.status();
    }
    if (const Result<void> valid_current = validate(current); !valid_current.ok()) {
        return valid_current.status();
    }
    if (const Status params_status = validate_param_ranges(params); !params_status.ok()) {
        return params_status;
    }
    if (const Status ignored_status = validate_ignored_regions(params, current.width, current.height);
        !ignored_status.ok()) {
        return ignored_status;
    }

    // Fingerprints come first so a layer-1 early exit skips the thumbnail
    // resampling cost entirely (ChangeReason contract).
    const Result<uint64_t> previous_fingerprint = fingerprint(previous);
    if (!previous_fingerprint.ok()) {
        return previous_fingerprint.status();
    }
    const Result<uint64_t> current_fingerprint = fingerprint(current);
    if (!current_fingerprint.ok()) {
        return current_fingerprint.status();
    }
    if (fingerprint_similarity(previous_fingerprint.value(), current_fingerprint.value()) >=
        params.fingerprint_similarity_threshold) {
        return make_early_exit_report(previous_fingerprint.value(), current_fingerprint.value(), params);
    }

    // Layer 2: build the comparison signatures and run the shared block diff.
    const Result<ChangeSignature> previous_signature = make_change_signature(previous, params);
    if (!previous_signature.ok()) {
        return previous_signature.status();
    }
    const Result<ChangeSignature> current_signature = make_change_signature(current, params);
    if (!current_signature.ok()) {
        return current_signature.status();
    }
    return detect_change_signatures(previous_signature.value(), current_signature.value(), params);
}

}  // namespace mirador
