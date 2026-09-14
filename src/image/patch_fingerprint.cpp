#include <cstddef>
#include <mirador/patch_fingerprint.hpp>

#include <mirador/color_convert.hpp>
#include <mirador/fingerprint.hpp>
#include <mirador/resize.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include "mirador/image_view.hpp"
#include "mirador/pixel_format.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"
#include "mirador/visual_fingerprint.hpp"

namespace mirador {
namespace {

/// FNV-1a 64 over raw bytes (platform-stable, allocation free).
uint64_t fnv1a_64(const std::byte* data, size_t size) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= std::to_integer<uint64_t>(data[i]) & 0xFFULL;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

Result<VisualPatchFingerprint> make_visual_patch_fingerprint(const ImageView& patch,
                                                             const PatchFingerprintParams& params,
                                                             int64_t max_bytes) noexcept {
    if (!validate(patch).ok()) {
        return Status(ErrorCode::kInvalidArgument, "invalid patch view");
    }
    if (params.thumb_side < 8 || params.thumb_side > 64) {
        return Status(ErrorCode::kInvalidArgument, "thumb_side must lie in [8, 64]");
    }
    const int64_t side = params.thumb_side;
    // Both dimensions are bounded by kMaxImageDimension, so the byte math
    // below cannot overflow int64.
    const int64_t gray_bytes = static_cast<int64_t>(patch.width) * patch.height;
    const int64_t thumb_bytes = side * side;
    if (gray_bytes + thumb_bytes > max_bytes) {
        return Status(ErrorCode::kBudgetExceeded, "patch fingerprint buffers exceed the byte budget");
    }

    auto gray = convert_color(patch, PixelFormat::kGray8, max_bytes);
    if (!gray.ok()) {
        return Status(gray.status().code(), std::string("patch gray conversion failed: ") + gray.status().message());
    }
    auto thumbnail_result = resize_area(gray.value().view(), params.thumb_side, params.thumb_side, max_bytes);
    if (!thumbnail_result.ok()) {
        return Status(thumbnail_result.status().code(),
                      std::string("patch thumbnail failed: ") + thumbnail_result.status().message());
    }
    ImageBuffer thumbnail = thumbnail_result.take_value();

    VisualPatchFingerprint result;
    result.thumb_width = params.thumb_side;
    result.thumb_height = params.thumb_side;
    const auto bytes = static_cast<size_t>(thumb_bytes);
    result.thumbnail_gray.resize(bytes);
    std::memcpy(result.thumbnail_gray.data(), thumbnail.data(), bytes);
    result.content_hash = fnv1a_64(result.thumbnail_gray.data(), bytes);
    // The public fingerprint() composes the deterministic 9x8 area resize with
    // dhash_9x8, exactly the documented meaning of the `dhash` field.
    const auto dhash = fingerprint(thumbnail.view());
    if (!dhash.ok()) {
        return Status(dhash.status().code(), std::string("patch dhash failed: ") + dhash.status().message());
    }
    result.dhash = dhash.value();
    return result;
}

}  // namespace mirador
