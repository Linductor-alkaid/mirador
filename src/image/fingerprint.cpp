#include <mirador/fingerprint.hpp>

#include <mirador/color_convert.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cstddef>
#include <cstdint>

namespace mirador {
namespace {

// A 9x8 RGBA thumbnail is 288 bytes, the gray one 72 bytes; 512 covers both.
constexpr int64_t kFingerprintBudgetBytes = 512;

constexpr int32_t kHashWidth = 9;
constexpr int32_t kHashHeight = 8;

}  // namespace

Result<uint64_t> dhash_9x8(const ImageView& gray) noexcept {
    if (const auto valid = validate(gray); !valid.ok()) {
        return valid.status();
    }
    if (gray.format != PixelFormat::kGray8 || gray.width != kHashWidth || gray.height != kHashHeight) {
        return Status(ErrorCode::kInvalidArgument, "dhash_9x8: requires a 9x8 kGray8 view");
    }
    uint64_t hash = 0;
    for (int32_t y = 0; y < kHashHeight; ++y) {
        const std::byte* row = gray.data + static_cast<int64_t>(y) * gray.row_stride_bytes;
        for (int32_t x = 0; x < kHashWidth - 1; ++x) {
            if (row[x + 1] > row[x]) {
                hash |= (uint64_t{1} << (y * (kHashWidth - 1) + x));
            }
        }
    }
    return {hash};
}

int32_t hamming_distance(uint64_t first, uint64_t second) noexcept {
    uint64_t x = first ^ second;
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<int32_t>((x * 0x0101010101010101ULL) >> 56);
}

double fingerprint_similarity(uint64_t first, uint64_t second) noexcept {
    return static_cast<double>(64 - hamming_distance(first, second)) / 64.0;
}

Result<uint64_t> fingerprint(const ImageView& image) noexcept {
    auto small = resize_area(image, kHashWidth, kHashHeight, kFingerprintBudgetBytes);
    if (!small.ok()) {
        return small.status();
    }
    const ImageBuffer thumbnail = small.take_value();
    if (thumbnail.format() == PixelFormat::kGray8) {
        return dhash_9x8(thumbnail.view());
    }
    auto gray = convert_color(thumbnail.view(), PixelFormat::kGray8, kFingerprintBudgetBytes);
    if (!gray.ok()) {
        return gray.status();
    }
    const ImageBuffer gray_buffer = gray.take_value();
    return dhash_9x8(gray_buffer.view());
}

}  // namespace mirador
