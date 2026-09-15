// M5-06 (SCOPE-08): capability cache hit path and backend outer-cost
// benchmark (design section 20). Measures, on deterministic synthetic frames:
//   1. CapabilityResultCache lookup: hit and miss latency (raw cache layer).
//   2. PerceptionSession::run_ocr outer wall time on the cache-hit path
//      (backend must NOT be re-invoked: the "avoid work" productization path).
//   3. PerceptionSession::run_ocr outer wall time on the miss path with a
//      counting stub backend (preprocess + coordinate recovery + cache store
//      + stub recognize; the stub adds no model cost by construction).
// Peak RSS is printed at the end (Linux /proc/self/status).
// Numbers are only meaningful on the machine that ran the benchmark
// (AGENTS.md: no cross-platform claims); report environment per DEC-011.
#include <mirador/backend_info.hpp>
#include <mirador/cache_policy.hpp>
#include <mirador/capability_cache.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::CachedCapabilityResult;
using mirador::CacheKeyDigest;
using mirador::capability_cache_digest;
using mirador::CapabilityKeyFields;
using mirador::CapabilityResultCache;
using mirador::ExecutionContext;
using mirador::Frame;
using mirador::ImageView;
using mirador::OcrBackend;
using mirador::OcrRequest;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::TextRegion;
using Clock = std::chrono::steady_clock;

constexpr int32_t kWidth = 1280;
constexpr int32_t kHeight = 720;
constexpr int64_t kStride = int64_t{kWidth} * 4;
constexpr int kRawEntries = 1000;
constexpr int kRawWarmups = 500;
constexpr int kRawIterations = 20000;
constexpr int kSessionWarmups = 50;
constexpr int kHitIterations = 2000;
constexpr int kMissIterations = 300;

/// Deterministic synthetic terminal-like scene (same family as
/// change_detection_bench: no randomness anywhere).
uint8_t scene_value(int32_t x, int32_t y) {
    const int32_t gradient = x * 180 / (kWidth - 1);
    const int32_t wave = (y / 40) % 2 == 0 ? 40 : 0;
    const int32_t bar = (x / 160) % 2 == 0 ? 20 : 0;
    return static_cast<uint8_t>((gradient + wave + bar) / 2 + 20);
}

/// p50/p95 over sorted nanosecond samples.
void print_percentiles(const char* label, std::vector<int64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double fraction) {
        const auto index = static_cast<size_t>(fraction * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[index]) / 1000.0;  // us
    };
    std::printf("  %-42s p50=%9.3f us  p95=%9.3f us  (n=%zu)\n", label, at(0.50), at(0.95), samples.size());
}

/// Counting stub backend: one text region over the central half of the
/// prepared view, deterministic, no model cost; call count is part of the
/// report (unchanged frames must not re-invoke it).
class CountingOcrBackend final : public OcrBackend {
public:
    [[nodiscard]] mirador::BackendInfo info() const override { return info_; }

    mirador::Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest& request,
                                                       const ExecutionContext& /*context*/) override {
        ++calls_;
        if (request.min_confidence > 0.8F) {
            return std::vector<TextRegion>{};
        }
        TextRegion line;
        line.bounds = RectF{static_cast<float>(prepared.width) / 4.0F, static_cast<float>(prepared.height) / 4.0F,
                            static_cast<float>(prepared.width) / 2.0F, static_cast<float>(prepared.height) / 2.0F};
        line.utf8_text = "bench line";
        line.confidence = 0.8F;
        return std::vector<TextRegion>{std::move(line)};
    }

    [[nodiscard]] int64_t calls() const noexcept { return calls_; }

private:
    mirador::BackendInfo info_{"bench-ocr", "1.0.0", "bench-model", "rev-1", {PixelFormat::kRgba8}, true};
    int64_t calls_ = 0;
};

Frame frame_over(const std::vector<std::byte>& bytes) {
    Frame frame;
    frame.image.data = bytes.data();
    frame.image.width = kWidth;
    frame.image.height = kHeight;
    frame.image.row_stride_bytes = kStride;
    frame.image.format = PixelFormat::kRgba8;
    frame.sequence = 1;
    frame.source_id = "bench";
    frame.owner = std::make_shared<const std::vector<std::byte>>(bytes);
    return frame;
}

void bench_raw_cache() {
    CapabilityResultCache cache = CapabilityResultCache::create(int64_t{16} * 1024 * 1024).take_value();
    for (int i = 0; i < kRawEntries; ++i) {
        CapabilityKeyFields fields;
        fields.image_fingerprint = static_cast<uint64_t>(i) + 1;
        fields.source_id = "bench";
        fields.kind = mirador::CapabilityKind::kOcr;
        CachedCapabilityResult payload;
        payload.kind = mirador::CapabilityKind::kOcr;
        if (!cache.insert(capability_cache_digest(fields), payload).ok()) {
            std::fprintf(stderr, "raw cache: insert failed at %d\n", i);
            std::exit(1);
        }
    }

    const auto key_of = [](int i) {
        CapabilityKeyFields fields;
        fields.image_fingerprint = static_cast<uint64_t>(i) + 1;
        fields.source_id = "bench";
        fields.kind = mirador::CapabilityKind::kOcr;
        return capability_cache_digest(fields);
    };

    for (int i = 0; i < kRawWarmups; ++i) {
        (void)cache.lookup(key_of(i % kRawEntries));
    }
    std::vector<int64_t> hit_samples;
    hit_samples.reserve(static_cast<size_t>(kRawIterations));
    for (int i = 0; i < kRawIterations; ++i) {
        const CacheKeyDigest key = key_of(i % kRawEntries);
        const auto start = Clock::now();
        const auto hit = cache.lookup(key);
        const auto end = Clock::now();
        if (!hit.ok() || hit.value() == nullptr) {
            std::fprintf(stderr, "raw cache: unexpected miss at %d\n", i);
            std::exit(1);
        }
        hit_samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    print_percentiles("raw cache lookup hit", hit_samples);

    std::vector<int64_t> miss_samples;
    miss_samples.reserve(static_cast<size_t>(kRawIterations));
    for (int i = 0; i < kRawIterations; ++i) {
        const CacheKeyDigest key = key_of(kRawEntries + i);
        const auto start = Clock::now();
        const auto miss = cache.lookup(key);
        const auto end = Clock::now();
        if (!miss.ok() || miss.value() != nullptr) {
            std::fprintf(stderr, "raw cache: unexpected hit at %d\n", i);
            std::exit(1);
        }
        miss_samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    print_percentiles("raw cache lookup miss", miss_samples);
}

void bench_session_hit_path(PerceptionSession& session, CountingOcrBackend& backend,
                            const std::vector<std::byte>& bytes) {
    const Frame frame = frame_over(bytes);
    const OcrRequest request;

    for (int i = 0; i < kSessionWarmups; ++i) {
        session.run_ocr(frame, &backend, request).take_value();
    }
    const int64_t calls_after_warmup = backend.calls();
    std::vector<int64_t> samples;
    samples.reserve(static_cast<size_t>(kHitIterations));
    for (int i = 0; i < kHitIterations; ++i) {
        const auto start = Clock::now();
        const auto regions = session.run_ocr(frame, &backend, request);
        const auto end = Clock::now();
        if (!regions.ok() || regions.value().size() != 1) {
            std::fprintf(stderr, "session hit: unexpected result at %d\n", i);
            std::exit(1);
        }
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    print_percentiles("session run_ocr cache-hit outer cost", samples);
    if (backend.calls() != calls_after_warmup) {
        std::fprintf(stderr, "session hit: backend re-invoked on identical frames (%lld -> %lld)\n",
                     static_cast<long long>(calls_after_warmup), static_cast<long long>(backend.calls()));
        std::exit(1);
    }
    std::printf("  %-42s backend calls=%lld (identical frames never re-invoke)\n", "backend invocation check",
                static_cast<long long>(backend.calls()));
}

void bench_session_miss_path(PerceptionSession& session, CountingOcrBackend& backend,
                             const std::vector<std::byte>& bytes) {
    // kRefresh bypasses the cache read and runs the full pipeline (fingerprint,
    // crop/convert, backend, coordinate recovery, cache store) every call —
    // exactly the miss-path cost this scenario must measure. Mutating pixels
    // instead would make the miss rate depend on the dHash quantization of the
    // scene, which is change-detection territory, not this benchmark's.
    const OcrRequest request = [] {
        OcrRequest refresh;
        refresh.cache_policy = mirador::CachePolicy::kRefresh;
        return refresh;
    }();
    const Frame frame = frame_over(bytes);
    const int64_t calls_before = backend.calls();
    std::vector<int64_t> samples;
    samples.reserve(static_cast<size_t>(kMissIterations));
    for (int i = 0; i < kMissIterations; ++i) {
        const auto start = Clock::now();
        const auto regions = session.run_ocr(frame, &backend, request);
        const auto end = Clock::now();
        if (!regions.ok() || regions.value().size() != 1) {
            std::fprintf(stderr, "session miss: unexpected result at %d\n", i);
            std::exit(1);
        }
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    print_percentiles("session run_ocr miss outer cost", samples);
    if (backend.calls() - calls_before != kMissIterations) {
        std::fprintf(stderr, "session miss: expected %d backend calls, saw %lld\n", kMissIterations,
                     static_cast<long long>(backend.calls() - calls_before));
        std::exit(1);
    }
}

void print_peak_rss() {
#ifdef __linux__
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        std::printf("  peak RSS: unavailable\n");
        return;
    }
    const std::array<char, 256> line_buffer{};
    char* line = const_cast<char*>(line_buffer.data());
    while (std::fgets(line, static_cast<int>(line_buffer.size()), status) != nullptr) {
        int hwm_kb = 0;
        if (std::sscanf(line, "VmHWM: %d kB", &hwm_kb) == 1) {
            std::printf("  %-42s %.2f MiB\n", "peak RSS (VmHWM)", static_cast<double>(hwm_kb) / 1024.0);
            break;
        }
    }
    std::fclose(status);
#else
    std::printf("  peak RSS: not implemented on this platform\n");
#endif
}

}  // namespace

int main() {
    std::printf("mirador cache/backend benchmark: %dx%d RGBA8 frame, deterministic scene\n", kWidth, kHeight);
    std::printf("  warmups/iterations: raw %d/%d, session hit %d/%d, session miss %d/%d\n", kRawWarmups, kRawIterations,
                kSessionWarmups, kHitIterations, kSessionWarmups, kMissIterations);

    bench_raw_cache();

    PerceptionSessionOptions options;
    options.source_id = "bench";
    PerceptionSession session = PerceptionSession::create(options).take_value();
    CountingOcrBackend backend;

    std::vector<std::byte> bytes(static_cast<size_t>(kStride) * kHeight, std::byte{0});
    for (int32_t y = 0; y < kHeight; ++y) {
        std::byte* row = bytes.data() + static_cast<int64_t>(y) * kStride;
        for (int32_t x = 0; x < kWidth; ++x) {
            const uint8_t value = scene_value(x, y);
            row[static_cast<int64_t>(x) * 4 + 0] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 1] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 2] = static_cast<std::byte>(value);
            row[static_cast<int64_t>(x) * 4 + 3] = static_cast<std::byte>(255);
        }
    }

    bench_session_hit_path(session, backend, bytes);
    bench_session_miss_path(session, backend, bytes);
    print_peak_rss();
    std::printf("note: numbers are valid only for the machine and build that produced them (DEC-011)\n");
    return 0;
}
