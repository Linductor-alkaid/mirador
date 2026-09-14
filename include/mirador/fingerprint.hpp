#ifndef MIRADOR_FINGERPRINT_HPP
#define MIRADOR_FINGERPRINT_HPP

#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador {

/// 64-bit difference hash (dHash) over a 9x8 Gray8 view (design section 11 first
/// layer). Bit index `row * 8 + col` (bit 0 = top-left comparison) is set when the
/// pixel at column `col + 1` is strictly brighter than the pixel at column `col`
/// in the same row. The definition is exact, so equal content yields equal hashes.
///
/// Errors: kInvalidArgument when the view is invalid, not `kGray8`, or not
/// exactly 9x8. Never throws.
[[nodiscard]] Result<uint64_t> dhash_9x8(const ImageView& gray) noexcept;

/// Hamming distance between two 64-bit fingerprints (0..64).
[[nodiscard]] int32_t hamming_distance(uint64_t first, uint64_t second) noexcept;

/// Fingerprint similarity in [0.0, 1.0]: `1 - distance / 64`.
[[nodiscard]] double fingerprint_similarity(uint64_t first, uint64_t second) noexcept;

/// Fingerprint of any valid view: area-resize to 9x8 (NV12 via its luma plane,
/// interleaved formats directly), convert to gray when needed, then `dhash_9x8`.
/// Composition of deterministic steps with bounded internal buffers (well under
/// 1 KiB regardless of input size, RULE-06), so equal content yields equal
/// fingerprints independent of stride or exact pixel layout.
///
/// Errors: those of `resize_area` (invalid view, kBudgetExceeded — the internal
/// budget is a fixed constant) and `convert_color`/`dhash_9x8`. Never throws.
[[nodiscard]] Result<uint64_t> fingerprint(const ImageView& image) noexcept;

}  // namespace mirador

#endif  // MIRADOR_FINGERPRINT_HPP
