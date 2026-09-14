#ifndef MIRADOR_PATCH_FINGERPRINT_HPP
#define MIRADOR_PATCH_FINGERPRINT_HPP

#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/visual_fingerprint.hpp>

#include <cstdint>

namespace mirador {

/// Extraction parameters: the normalized square thumbnail side, in [8, 64]
/// (a 64x64 thumb keeps one index entry at 4 KiB + hashes, DEC-014).
struct PatchFingerprintParams {
    int32_t thumb_side = 32;
};

/// Builds the normalized fingerprint of a visual patch (design section 12;
/// M3-09): converts the patch to grayscale (NV12 through its luma plane),
/// area-resamples it to `thumb_side` x `thumb_side` with the deterministic
/// kernel, and fills the `VisualPatchFingerprint` contract — FNV-1a 64
/// content hash over the thumbnail bytes, the 9x8 dHash of the same
/// thumbnail, and the thumbnail bytes themselves. Equal pixels produce equal
/// fingerprints regardless of source format or stride. The input is never
/// modified (RULE-04).
///
/// Errors: kInvalidArgument (invalid view, thumb_side out of range),
/// kUnsupportedFormat (unreachable gray conversion), kBudgetExceeded
/// (`max_bytes` does not cover the gray intermediate and the thumbnail).
/// Never throws.
[[nodiscard]] Result<VisualPatchFingerprint> make_visual_patch_fingerprint(const ImageView& patch,
                                                                           const PatchFingerprintParams& params,
                                                                           int64_t max_bytes) noexcept;

}  // namespace mirador

#endif  // MIRADOR_PATCH_FINGERPRINT_HPP
