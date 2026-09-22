// M7-03: change-gate overhead benchmark over deterministic synthetic frames,
// following the change_detection_bench.cpp protocol (the M1 baseline). For
// each scenario it times detect_change alone, evaluate_change_gate alone (on
// the scenario's deterministic report) and both combined, over a pool of nine
// adopted tracks; the classification and the gate decision counts are
// asserted so a silent level mix-up cannot distort the numbers. Prints p50/p95
// wall clock per call; numbers are only meaningful on the machine that ran the
// benchmark (DEC-011; AGENTS.md: no cross-platform claims).
#include <mirador/change_detection.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/object_tracker.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/semantic_snapshot.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using mirador::ChangeClassification;
using mirador::ChangeDetectionParams;
using mirador::ChangeGateDecision;
using mirador::ChangeGateTrace;
using mirador::ChangeReport;
using mirador::detect_change;
using mirador::ImageView;
using mirador::ObjectTracker;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::RectI;
using mirador::TrackGateDecision;
using mirador::VisualRegion;

constexpr int32_t kWidth = 1280;
constexpr int32_t kHeight = 720;
constexpr int64_t kStride = int64_t{kWidth} * 4;
constexpr int kWarmups = 20;
constexpr int kIterations = 300;

// Three-column, three-row track grid; the change rects below are placed so the
// "partial-disjoint" fill maps to change blocks clear of every track while the
// "partial-hit" fill covers exactly the left and middle tracks of the middle
// row (block-grid safe: 160x90 frame blocks at the default parameters).
constexpr int kTrackCols = 3;
constexpr int kTrackRows = 3;
constexpr float kTrackWidth = 160.0F;
constexpr float kTrackHeight = 60.0F;
constexpr std::array<float, kTrackCols> kTrackX = {100.0F, 540.0F, 980.0F};
constexpr std::array<float, kTrackRows> kTrackY = {120.0F, 330.0F, 540.0F};

/// Deterministic synthetic terminal-like scene: horizontal gradient, horizontal
/// wave bands and vertical bars; no randomness anywhere (M1 bench scene).
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

double percentile_us(const std::vector<int64_t>& samples, const double fraction) {
    std::vector<int64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]) / 1000.0;
}

struct GateCounts {
    size_t reuse = 0;
    size_t verify = 0;
    size_t inactive = 0;
};

GateCounts count_decisions(const ChangeGateTrace& trace) {
    GateCounts counts;
    for (const TrackGateDecision& entry : trace.tracks) {
        switch (entry.decision) {
            case ChangeGateDecision::kReuse:
                ++counts.reuse;
                break;
            case ChangeGateDecision::kVerify:
                ++counts.verify;
                break;
            case ChangeGateDecision::kInactive:
                ++counts.inactive;
                break;
        }
    }
    return counts;
}

void fail(const char* scenario, const std::string& message) {
    std::fprintf(stderr, "scenario %s failed: %s\n", scenario, message.c_str());
    std::exit(1);
}

/// Times one callable with the M1 bench protocol (warmups + fixed iterations,
/// steady clock); the callable returns false to abort with an error.
template <typename Fn>
std::vector<int64_t> time_samples(const char* scenario, Fn&& callable) {
    for (int i = 0; i < kWarmups; ++i) {
        if (!callable()) {
            fail(scenario, "warmup call failed");
        }
    }
    std::vector<int64_t> samples;
    samples.reserve(kIterations);
    for (int i = 0; i < kIterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = callable();
        const auto stop = std::chrono::steady_clock::now();
        if (!ok) {
            fail(scenario, "timed call failed");
        }
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }
    return samples;
}

void run_scenario(const char* name, const ImageView& previous, const ImageView& current,
                  const ChangeDetectionParams& params, const ChangeClassification expected_classification,
                  const size_t expected_verify, const ObjectTracker& tracker) {
    const auto probe = detect_change(previous, current, params);
    if (!probe.ok()) {
        fail(name, "detect_change failed");
    }
    if (probe.value().classification != expected_classification) {
        fail(name, "unexpected classification (change rect drifted across the block grid)");
    }
    const auto probe_trace = tracker.evaluate_change_gate(probe.value());
    if (!probe_trace.ok()) {
        fail(name, "evaluate_change_gate failed");
    }
    const GateCounts actual_counts = count_decisions(probe_trace.value());
    if (actual_counts.verify != expected_verify) {
        fail(name, "unexpected verify count (change rect drifted across the block grid)");
    }

    const ChangeReport& report = probe.value();
    const auto detect_samples = time_samples(name, [&] { return detect_change(previous, current, params).ok(); });
    const auto gate_samples = time_samples(name, [&] { return tracker.evaluate_change_gate(report).ok(); });
    const auto both_samples = time_samples(name, [&] {
        auto result = detect_change(previous, current, params);
        if (!result.ok()) {
            return false;
        }
        return tracker.evaluate_change_gate(result.value()).ok();
    });

    std::printf(
        "%-17s classification=%d verify=%zu reuse=%zu | detect p50=%9.2fus p95=%9.2fus | gate p50=%7.3fus "
        "p95=%7.3fus | detect+gate p50=%9.2fus p95=%9.2fus\n",
        name, static_cast<int>(expected_classification), actual_counts.verify, actual_counts.reuse,
        percentile_us(detect_samples, 0.50), percentile_us(detect_samples, 0.95), percentile_us(gate_samples, 0.50),
        percentile_us(gate_samples, 0.95), percentile_us(both_samples, 0.50), percentile_us(both_samples, 0.95));
}

}  // namespace

int main() {
    const BenchImage base = make_frame(false);
    const BenchImage identical = make_frame(false);
    // Maps to change block (col 2, row 7) only, clear of every track rect.
    BenchImage disjoint = make_frame(false);
    fill_rect(disjoint, RectI{340, 640, 120, 60}, 250);
    // Covers change blocks (cols 1-3, rows 3-4): exactly the (100,330) and
    // (540,330) tracks intersect the mapped ROI.
    BenchImage hit = make_frame(false);
    fill_rect(hit, RectI{160, 280, 480, 160}, 250);
    const BenchImage rotated = make_frame(true);

    auto tracker_result = ObjectTracker::create();
    if (!tracker_result.ok()) {
        std::fprintf(stderr, "tracker create failed\n");
        std::exit(1);
    }
    ObjectTracker pool = tracker_result.take_value();
    for (int row = 0; row < kTrackRows; ++row) {
        for (int col = 0; col < kTrackCols; ++col) {
            VisualRegion region;
            region.stable_id = static_cast<uint64_t>(row) * kTrackCols + col + 1;
            region.bounds = RectF{kTrackX[col], kTrackY[row], kTrackWidth, kTrackHeight};
            region.label = "button";
            region.confidence = 0.9F;
            if (!pool.adopt_track(region, base.view, 1).ok()) {
                std::fprintf(stderr, "adopt_track failed\n");
                std::exit(1);
            }
        }
    }

    std::printf("change gate %dx%d RGBA, %d tracks, %d iterations after %d warmups\n", kWidth, kHeight,
                kTrackCols * kTrackRows, kIterations, kWarmups);
    run_scenario("unchanged", base.view, identical.view, ChangeDetectionParams{}, ChangeClassification::kNone, 0, pool);
    run_scenario("partial-disjoint", base.view, disjoint.view, ChangeDetectionParams{}, ChangeClassification::kPartial,
                 0, pool);
    run_scenario("partial-hit", base.view, hit.view, ChangeDetectionParams{}, ChangeClassification::kPartial, 2, pool);
    run_scenario("global-rot180", base.view, rotated.view, ChangeDetectionParams{}, ChangeClassification::kGlobal,
                 static_cast<size_t>(kTrackCols) * static_cast<size_t>(kTrackRows), pool);
    return 0;
}
