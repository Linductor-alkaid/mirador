#ifndef MIRADOR_VISUAL_FINGERPRINT_HPP
#define MIRADOR_VISUAL_FINGERPRINT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirador {

/// Normalized fingerprint of one visual patch, the shared data contract
/// between the image module (extraction, M3-09) and the cache module's
/// `VisualIndex` (storage and matching, M3-10). Pure data, no logic: the
/// bytes are a packed grayscale thumbnail (`thumb_width` x `thumb_height`,
/// tight stride), `content_hash` is the FNV-1a 64 digest over those bytes and
/// `dhash` is the 9x8 difference hash of the same thumbnail. Consumers read
/// the fields; only the image module builds them (DEC-014).
struct VisualPatchFingerprint {
    uint64_t content_hash = 0;
    uint64_t dhash = 0;
    int32_t thumb_width = 0;
    int32_t thumb_height = 0;
    std::vector<std::byte> thumbnail_gray;

    /// Component equality (test convenience; exact byte comparison).
    [[nodiscard]] friend bool operator==(const VisualPatchFingerprint& lhs,
                                         const VisualPatchFingerprint& rhs) noexcept {
        return lhs.content_hash == rhs.content_hash && lhs.dhash == rhs.dhash &&
               lhs.thumb_width == rhs.thumb_width && lhs.thumb_height == rhs.thumb_height &&
               lhs.thumbnail_gray == rhs.thumbnail_gray;
    }
};

}  // namespace mirador

#endif  // MIRADOR_VISUAL_FINGERPRINT_HPP
