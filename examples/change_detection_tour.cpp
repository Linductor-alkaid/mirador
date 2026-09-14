// M1-10: runtime-free example walking the core perception loop from design
// section 2 - capture two frames, fingerprint them, run layered change
// detection with an ignored region, and cache the frame artifacts in a
// bounded FrameCache. Everything is deterministic, in-memory and offline:
// no model runtime, no threads, no network, no disk (privacy defaults).
#include <mirador/change_detection.hpp>
#include <mirador/fingerprint.hpp>
#include <mirador/frame_cache.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/pixel_format.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using mirador::ChangeClassification;
using mirador::ChangeDetectionParams;
using mirador::ChangeReason;
using mirador::detect_change;
using mirador::fingerprint;
using mirador::fingerprint_similarity;
using mirador::FrameCache;
using mirador::ImageBuffer;
using mirador::PixelFormat;
using mirador::RectI;

constexpr int32_t kWidth = 640;
constexpr int32_t kHeight = 480;
constexpr int64_t kBudget = int64_t{8} * 1024 * 1024;

/// Deterministic synthetic "settings screen": gradient background, a header
/// band and two card rectangles. No randomness, no external resources.
uint8_t screen_value(int32_t x, int32_t y) {
    const int32_t gradient = x * 120 / (kWidth - 1);
    const int32_t header = y < 64 ? 60 : 0;
    const int32_t card = ((y / 120) % 2 == 0 && x > 40 && x < 600) ? 30 : 0;
    return static_cast<uint8_t>(gradient / 2 + header + card + 40);
}

/// Captures a frame and optionally paints one "progress bar" and one
/// "spinner" rectangle, as a UI would between two captures.
ImageBuffer capture_frame(bool progress_grown, bool spinner_on) {
    ImageBuffer frame = ImageBuffer::create(PixelFormat::kRgba8, kWidth, kHeight, kBudget).take_value();
    std::byte* bytes = frame.data();
    for (int32_t y = 0; y < kHeight; ++y) {
        std::byte* row = bytes + static_cast<int64_t>(y) * frame.row_stride_bytes();
        for (int32_t x = 0; x < kWidth; ++x) {
            const int32_t progress = progress_grown && y >= 200 && y < 216 && x >= 80 && x < 560 ? 230 : 0;
            const int32_t spinner = spinner_on && y >= 400 && y < 430 && x >= 80 && x < 110 ? 240 : 0;
            const uint8_t value =
                static_cast<uint8_t>(std::min(255, static_cast<int>(screen_value(x, y)) + progress + spinner));
            row[static_cast<int64_t>(x) * 4 + 0] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 1] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 2] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 3] = static_cast<std::byte>(255);
        }
    }
    return frame;
}

const char* classification_name(ChangeClassification classification) {
    switch (classification) {
        case ChangeClassification::kNone:
            return "none";
        case ChangeClassification::kPartial:
            return "partial";
        case ChangeClassification::kGlobal:
            return "global";
    }
    return "unknown";
}

void print_report(const char* label, const mirador::ChangeReport& report) {
    std::printf("%s:\n  classification=%s reason=%s similarity=%.3f changed_ratio=%.4f\n", label,
                classification_name(report.classification),
                report.reason == ChangeReason::kFingerprintEarlyExit ? "fingerprint-early-exit" : "block-diff",
                report.frame_similarity, report.changed_area_ratio);
    for (const RectI& region : report.changed_regions) {
        std::printf("  changed ROI: x=%d y=%d w=%d h=%d\n", region.x, region.y, region.width, region.height);
    }
}

}  // namespace

int main() {
    // Capture 1 -> capture 2: the progress bar grows (a real change worth
    // invalidating), the spinner animates (ignored: no invalidation wanted).
    const ImageBuffer first = capture_frame(false, true);
    const ImageBuffer second = capture_frame(true, false);

    // Layer 1 on its own: fingerprints and their similarity.
    const auto first_hash = fingerprint(first.view());
    const auto second_hash = fingerprint(second.view());
    if (!first_hash.ok() || !second_hash.ok()) {
        std::fprintf(stderr, "example: fingerprint failed\n");
        return 1;
    }
    std::printf("fingerprints: %016llx vs %016llx (similarity %.3f)\n",
                static_cast<unsigned long long>(first_hash.value()),
                static_cast<unsigned long long>(second_hash.value()),
                fingerprint_similarity(first_hash.value(), second_hash.value()));

    // The spinner sits inside a block-aligned ignored region (blocks are
    // 80x60 frame pixels on a 640x480 frame with the default grid); the caller
    // marks it because spinner animations must not invalidate OCR results.
    ChangeDetectionParams params;
    params.fingerprint_similarity_threshold = 1.0;  // force the block layer for the demo
    params.ignored_regions.push_back(RectI{80, 360, 80, 120});

    const auto report = detect_change(first.view(), second.view(), params);
    if (!report.ok()) {
        std::fprintf(stderr, "example: detect_change failed: %s\n", report.status().message().c_str());
        return 1;
    }
    print_report("progress bar grown, spinner toggled", report.value());

    // Identical captures short-circuit at the fingerprint layer.
    const ImageBuffer again = capture_frame(true, false);
    const auto same_params = ChangeDetectionParams{};
    const auto unchanged = detect_change(again.view(), second.view(), same_params);
    if (!unchanged.ok()) {
        std::fprintf(stderr, "example: detect_change failed\n");
        return 1;
    }
    print_report("identical recapture", unchanged.value());

    // Frame-level artifacts (here: the fingerprints) live in a bounded LRU
    // cache; a session would carry `current_fingerprint` forward via this.
    FrameCache frames = FrameCache::create(FrameCache::kEntryOverheadBytes * 8).take_value();
    const auto as_bytes = [](uint64_t hash) {
        std::vector<std::byte> bytes(sizeof(hash));
        for (size_t i = 0; i < sizeof(hash); ++i) {
            bytes[i] = static_cast<std::byte>((hash >> (i * 8)) & 0xFF);
        }
        return bytes;
    };
    if (!frames.insert(1, as_bytes(first_hash.value())).ok() || !frames.insert(2, as_bytes(second_hash.value())).ok()) {
        std::fprintf(stderr, "example: frame cache insert failed\n");
        return 1;
    }
    const auto cached = frames.lookup(1);
    std::printf("frame cache: %zu entries, %lld/%lld bytes, lookup(1) hit=%s\n", frames.entry_count(),
                static_cast<long long>(frames.byte_size()), static_cast<long long>(frames.max_bytes()),
                cached.ok() && cached.value() != nullptr ? "yes" : "no");
    return 0;
}
