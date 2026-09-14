// M1-08: change-detection benchmark over deterministic synthetic frames.
// Four scenarios from the M1 plan (unchanged, partial change, rotation,
// dynamic region inside an ignored region) on a 1280x720 RGBA frame with the
// default parameters (the ignored-region scenario adds one region). Prints
// p50/p95 wall-clock per detect_change call; numbers are only meaningful on
// the machine that ran the benchmark (AGENTS.md: no cross-platform claims).
#include <mirador/change_detection.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using mirador::ChangeClassification;
using mirador::ChangeDetectionParams;
using mirador::detect_change;
using mirador::ImageView;
using mirador::PixelFormat;
using mirador::RectI;

constexpr int32_t kWidth = 1280;
constexpr int32_t kHeight = 720;
constexpr int64_t kStride = int64_t{kWidth} * 4;
constexpr int kWarmups = 20;
constexpr int kIterations = 300;

/// Deterministic synthetic terminal-like scene: horizontal gradient, horizontal
/// wave bands and vertical bars; no randomness anywhere.
uint8_t scene_value(int32_t x, int32_t y) {
    const int32_t gradient = x * 180 / (kWidth - 1);
    const int32_t wave = (y / 40) % 2 == 0 ? 40 : 0;
    const int32_t bar = (x / 160) % 2 == 0 ? 20 : 0;
    return static_cast<uint8_t>((gradient + wave + bar) / 2 + 20);
}

struct BenchImage {
    std::vector<std::byte> bytes;
    ImageView view;
};

BenchImage make_frame(bool rotate180) {
    BenchImage image;
    image.bytes.assign(static_cast<size_t>(kStride) * kHeight, std::byte{0});
    for (int32_t y = 0; y < kHeight; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * kStride;
        for (int32_t x = 0; x < kWidth; ++x) {
            const uint8_t value = rotate180 ? scene_value(kWidth - 1 - x, kHeight - 1 - y) : scene_value(x, y);
            row[static_cast<int64_t>(x) * 4 + 0] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 1] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 2] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 3] = static_cast<std::byte>(255);
        }
    }
    image.view.data = image.bytes.data();
    image.view.width = kWidth;
    image.view.height = kHeight;
    image.view.row_stride_bytes = kStride;
    image.view.format = PixelFormat::kRgba8;
    return image;
}

void fill_rect(BenchImage& image, const RectI& rect, const uint8_t value) {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * kStride;
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            row[static_cast<int64_t>(x) * 4 + 0] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 1] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 2] = static_cast<std::byte>(value);
        }
    }
}

double percentile_us(std::vector<int64_t>& samples, const double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<size_t>(fraction * static_cast<double>(samples.size() - 1));
    return static_cast<double>(samples[index]) / 1000.0;
}

void run_scenario(const char* name, const ImageView& previous, const ImageView& current,
                  const ChangeDetectionParams& params) {
    for (int i = 0; i < kWarmups; ++i) {
        if (!detect_change(previous, current, params).ok()) {
            std::fprintf(stderr, "scenario %s failed\n", name);
            std::exit(1);
        }
    }
    std::vector<int64_t> samples;
    samples.reserve(kIterations);
    ChangeClassification classification = ChangeClassification::kNone;
    for (int i = 0; i < kIterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto report = detect_change(previous, current, params);
        const auto stop = std::chrono::steady_clock::now();
        if (!report.ok()) {
            std::fprintf(stderr, "scenario %s failed\n", name);
            std::exit(1);
        }
        classification = report.value().classification;
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }
    std::printf("%-18s classification=%d p50=%9.2fus p95=%9.2fus\n", name, static_cast<int>(classification),
                percentile_us(samples, 0.50), percentile_us(samples, 0.95));
}

}  // namespace

int main() {
    const BenchImage base = make_frame(false);
    const BenchImage identical = make_frame(false);
    BenchImage partial = make_frame(false);
    fill_rect(partial, RectI{160, 120, 320, 240}, 250);
    const BenchImage rotated = make_frame(true);
    BenchImage animated = make_frame(false);
    fill_rect(animated, RectI{880, 520, 160, 120}, 250);

    ChangeDetectionParams ignored_params;
    ignored_params.ignored_regions.push_back(RectI{840, 480, 240, 200});

    std::printf("detect_change %dx%d RGBA, %d iterations after %d warmups\n", kWidth, kHeight, kIterations, kWarmups);
    run_scenario("unchanged", base.view, identical.view, ChangeDetectionParams{});
    run_scenario("partial-change", base.view, partial.view, ChangeDetectionParams{});
    run_scenario("rotation-180", base.view, rotated.view, ChangeDetectionParams{});
    run_scenario("dynamic-ignored", base.view, animated.view, ignored_params);
    return 0;
}
