#ifndef MIRADOR_CHANGE_DETECTION_HPP
#define MIRADOR_CHANGE_DETECTION_HPP

#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <vector>

namespace mirador {

/// Decision of the change detector (design section 11): whether existing visual
/// results for an image source remain usable, and at which granularity they must
/// be invalidated.
enum class ChangeClassification : uint8_t {
    kNone,     ///< no change detected; cached results stay valid
    kPartial,  ///< changed blocks below the global ratio; invalidate by ROI intersection
    kGlobal,   ///< changed-area ratio reached the global threshold; invalidate everything
};

/// Which detection layer produced the decision (design section 11 layering).
enum class ChangeReason : uint8_t {
    /// Layer 1: frame fingerprint similarity reached the threshold; the block diff
    /// (and its thumbnail resampling cost) was skipped entirely.
    kFingerprintEarlyExit,
    /// Layer 2: the decision comes from the thumbnail block diff.
    kBlockDiff,
    /// Stateful consumers only (sessions, M2): no previous signature exists for
    /// this source yet, so everything counts as changed (classification kGlobal,
    /// similarity 1.0, no comparison was run).
    kFirstFrame,
};

/// Thresholds actually applied during a detection, echoed in every ChangeReport
/// so a report stays interpretable on its own (design section 11: report carries
/// thresholds and reason).
struct ChangeThresholds {
    double fingerprint_similarity = 0.0;
    int32_t block_diff = 0;
    double global_area_ratio = 0.0;
};

/// Tunables of the layered change detection. Inclusive threshold semantics are
/// documented per field; the defaults target terminal-capture scenarios.
struct ChangeDetectionParams {
    /// Layer 1 early exit: frame fingerprint similarity at or above this value in
    /// [0, 1] classifies the frame as unchanged without running the block diff.
    double fingerprint_similarity_threshold = 0.98;
    /// Edge length of the square layer-2 comparison thumbnails in [8, 256]; both
    /// frames are resampled to this size.
    int32_t thumbnail_size = 64;
    /// Edge length of one comparison block in thumbnail pixels, in
    /// [1, thumbnail_size]; at most 64 blocks per side.
    int32_t block_size = 8;
    /// Mean absolute luma difference per block in [0, 255] at or above which the
    /// block counts as changed (exact integer comparison against the block sum).
    int32_t block_diff_threshold = 8;
    /// Changed-block ratio in (0, 1] at or above which the classification is
    /// kGlobal instead of kPartial.
    double global_area_ratio = 0.5;
    /// Frame regions (presented coordinates of the current view) whose fully
    /// covered blocks never count as changed; animations confined to ignored
    /// regions therefore do not invalidate results. Blocks only partially covered
    /// are still detected (conservative). Every region must be a valid RectI
    /// inside the current frame bounds.
    std::vector<RectI> ignored_regions;
};

/// Outcome of one change detection (design section 11: similarity, changed-area
/// ratio, change ROIs, applied thresholds, reason). ROIs are in the presented
/// coordinate space of the `current` view; they are the frame-space bounding
/// rectangles of connected changed-block components (8-connectivity), listed in
/// deterministic scan order (top-to-bottom, left-to-right by first block).
struct ChangeReport {
    ChangeClassification classification = ChangeClassification::kNone;
    ChangeReason reason = ChangeReason::kFingerprintEarlyExit;
    /// Fingerprint similarity between the two frames in [0, 1] (1.0 for equal
    /// fingerprints).
    double frame_similarity = 1.0;
    /// Changed blocks divided by all comparison blocks in [0, 1]; 0 when the
    /// block diff did not run.
    double changed_area_ratio = 0.0;
    /// Frame-space change ROIs; empty unless classification is kPartial or
    /// kGlobal.
    std::vector<RectI> changed_regions;
    /// The per-frame fingerprints layer 1 computed, so callers driving sessions
    /// can carry `current_fingerprint` into the next comparison without
    /// recomputing it.
    uint64_t previous_fingerprint = 0;
    uint64_t current_fingerprint = 0;
    /// The thresholds this decision used.
    ChangeThresholds thresholds;
};

/// Layered change detection between two frames of one image source (design
/// section 11). Layer 1 compares the frames' dHash fingerprints (the public
/// `fingerprint()` values) and short-circuits to kNone when their similarity
/// reaches the threshold. Layer 2 resamples both frames to square grayscale
/// thumbnails, splits them into `block_size` blocks, and marks a block changed
/// when its mean absolute luma difference reaches `block_diff_threshold`
/// (integer-exact: `sum >= threshold * count`). Changed blocks inside ignored
/// regions (fully covered, current frame coordinates) are excluded, remaining
/// blocks are merged into connected ROI components, and component rectangles are
/// mapped back to current-frame coordinates (floor on the leading edge, ceil on
/// the trailing edge, so ROIs always cover their blocks).
///
/// The two views may differ in size and format (rotations and resizes are real
/// changes); ROIs always refer to the current frame. The result is deterministic:
/// equal inputs produce bit-identical reports across platforms. All internal
/// buffers are bounded by a fixed budget regardless of frame size (RULE-06).
///
/// Errors: kInvalidArgument for invalid views or parameters (thresholds outside
/// their documented ranges, more than 64 blocks per side, ignored regions not
/// inside the current frame), kBudgetExceeded for internal allocation failure.
/// Never throws.
[[nodiscard]] Result<ChangeReport> detect_change(const ImageView& previous, const ImageView& current,
                                                 const ChangeDetectionParams& params) noexcept;

/// Bounded per-frame state for stateful consumers (design section 18: the
/// session keeps the previous frame's compact signature, never the full
/// frame): frame dimensions, the layer-1 dHash fingerprint and the layer-2
/// square grayscale thumbnail (`params.thumbnail_size` pixels per side, ≤ 64
/// KiB). Move-only through its ImageBuffer member.
struct ChangeSignature {
    /// Presented dimensions of the source view the signature was built from;
    /// ROI mapping for `detect_change` uses the current signature's values.
    int32_t frame_width = 0;
    int32_t frame_height = 0;
    uint64_t fingerprint = 0;
    /// Grayscale comparison thumbnail (kGray8, square, tight stride).
    ImageBuffer thumbnail;
};

/// Builds the compact signature of one frame with the same layer semantics as
/// `detect_change` (fingerprint + square gray thumbnail). Parameter ranges
/// involving `thumbnail_size`/`block_size` are validated here so any signature
/// is usable later; ignored regions are a detection-time concern and are not
/// checked.
///
/// Errors: kInvalidArgument for invalid views or out-of-range parameters,
/// kBudgetExceeded for internal allocation failure. Never throws.
[[nodiscard]] Result<ChangeSignature> make_change_signature(const ImageView& frame,
                                                            const ChangeDetectionParams& params) noexcept;

/// Layered change detection between two previously built signatures. Bit-identical
/// to the view-based `detect_change` on the same frames: the comparison grid comes
/// from the signatures' stored thumbnails (both must share one thumbnail size,
/// otherwise kInvalidArgument), so `params.thumbnail_size` is unused here, and
/// `params.block_size` must fit that stored size. ROIs refer to the current
/// signature's frame dimensions.
///
/// Errors: kInvalidArgument for out-of-range thresholds, block_size larger than
/// the signatures' thumbnail, mismatched thumbnail sizes or ignored regions not
/// inside the current frame; kBudgetExceeded for internal allocation failure.
/// Never throws.
[[nodiscard]] Result<ChangeReport> detect_change(const ChangeSignature& previous, const ChangeSignature& current,
                                                 const ChangeDetectionParams& params) noexcept;

}  // namespace mirador

#endif  // MIRADOR_CHANGE_DETECTION_HPP
