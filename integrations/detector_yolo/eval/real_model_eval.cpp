// Explicit offline evaluation: the caller supplies model, P6 image and output
// prefix. This executable may write diagnostic tensors; the backend never does.
#include "ncnn_runtime.hpp"
#include "yolo_backend.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/cache_policy.hpp>
#include <mirador/crop.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/letterbox.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ratio>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
namespace m = mirador;
namespace mi = mirador::integrations;
using Clock = std::chrono::steady_clock;
constexpr int64_t kBudget = int64_t{64} * 1024 * 1024;

template <class T>
T take(m::Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.status().message());
    }
    return result.take_value();
}
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
struct Picture {
    int width = 0;
    int height = 0;
    std::vector<std::byte> bytes;
};
m::ImageView view(const Picture& p) {
    m::ImageView result;
    result.width = p.width;
    result.height = p.height;
    result.row_stride_bytes = static_cast<int64_t>(p.width) * 3;
    result.format = m::PixelFormat::kRgb8;
    result.data = p.bytes.data();
    return result;
}
Picture load(const char* path) {
    std::ifstream input(path, std::ios::binary);
    Picture p;
    std::string magic;
    int maximum = 0;
    input >> magic >> p.width >> p.height >> maximum;
    require(magic == "P6" && p.width > 0 && p.height > 0 && p.width <= 8192 && p.height <= 8192 && maximum == 255,
            "expected bounded 8-bit P6, without comments");
    require(static_cast<int64_t>(p.width) * p.height * 3 <= kBudget, "P6 byte budget exceeded");
    input.get();
    p.bytes.resize(static_cast<size_t>(p.width) * p.height * 3);
    require(static_cast<bool>(
                input.read(reinterpret_cast<char*>(p.bytes.data()), static_cast<std::streamsize>(p.bytes.size()))),
            "truncated P6");
    return p;
}
void dump(const std::string& path, const std::vector<float>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
    require(static_cast<bool>(out), "tensor write failed");
}
struct Timing {
    double median = 0;
    double p95 = 0;
};
template <class F>
Timing measure(int iterations, F operation) {
    for (int i = 0; i < 2; ++i) {
        operation();
    }
    std::vector<double> samples;
    for (int i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        operation();
        samples.push_back(elapsed(start));
    }
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples[(samples.size() - 1) * 95 / 100]};
}
bool near(const std::vector<m::DetectionRegion>& a, const std::vector<m::DetectionRegion>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.class_id != y.class_id || std::abs(x.confidence - y.confidence) > 1e-6F ||
            std::abs(x.bounds.x - y.bounds.x) > .001F || std::abs(x.bounds.y - y.bounds.y) > .001F ||
            std::abs(x.bounds.width - y.bounds.width) > .001F || std::abs(x.bounds.height - y.bounds.height) > .001F) {
            return false;
        }
    }
    return true;
}
class Counted final : public m::DetectorBackend {
public:
    explicit Counted(mi::YoloDetectorBackend& backend) : backend_(backend) {}
    [[nodiscard]] m::BackendInfo info() const override { return backend_.info(); }
    m::Result<std::vector<m::DetectionRegion>> detect(const m::ImageView& image, const m::DetectionRequest& request,
                                                      const m::ExecutionContext& context) override {
        ++calls_;
        return backend_.detect(image, request, context);
    }
    [[nodiscard]] int calls() const { return calls_; }

private:
    mi::YoloDetectorBackend& backend_;
    int calls_ = 0;
};
void check_session(mi::YoloDetectorBackend& backend, const m::ImageView& image, const m::DetectionRequest& request,
                   const std::vector<m::DetectionRegion>& expected) {
    Counted counted(backend);
    m::PerceptionSessionOptions options;
    options.source_id = "offline-yolo-eval";
    auto session = take(m::PerceptionSession::create(options));
    m::Frame frame;
    frame.image = image;
    frame.source_id = options.source_id;
    frame.sequence = 1;
    const auto first = take(session.run_detector(frame, &counted, request));
    require(near(first, expected), "session coordinate recovery differs from direct backend");
    require(take(session.run_detector(frame, &counted, request)) == first && counted.calls() == 1,
            "same-frame capability cache did not reuse exact results");
    auto refresh = request;
    refresh.cache_policy = m::CachePolicy::kRefresh;
    require(near(take(session.run_detector(frame, &counted, refresh)), expected) && counted.calls() == 2,
            "explicit refresh did not execute real model again");
    const m::RectI roi{image.width / 4, image.height / 4, image.width / 2, image.height / 2};
    const auto cropped = take(m::crop(image, roi, kBudget));
    auto roi_expected = take(backend.detect(cropped.view(), request, {}));
    for (auto& region : roi_expected) {
        region.bounds.x += static_cast<float>(roi.x);
        region.bounds.y += static_cast<float>(roi.y);
    }
    auto roi_request = refresh;
    roi_request.roi = m::RectF{static_cast<float>(roi.x), static_cast<float>(roi.y), static_cast<float>(roi.width),
                               static_cast<float>(roi.height)};
    require(near(take(session.run_detector(frame, &counted, roi_request)), roi_expected),
            "session ROI boxes did not recover into the full image");
    for (const auto rotation : {m::Rotation::k90, m::Rotation::k180, m::Rotation::k270}) {
        frame.image.rotation = rotation;  // presented pixels stay identical (ImageView contract)
        require(near(take(session.run_detector(frame, &counted, refresh)), expected),
                "rotation metadata changed oriented detections");
    }
}
void check_contract(mi::YoloDetectorBackend& backend, const mi::YoloDetectorOptions& options, const m::ImageView& image,
                    const m::DetectionRequest& request, const std::vector<m::DetectionRegion>& expected) {
    auto padded = image;
    padded.row_stride_bytes += 13;
    std::vector<std::byte> storage(static_cast<size_t>(padded.row_stride_bytes) * image.height, std::byte{117});
    for (int y = 0; y < image.height; ++y) {
        std::copy_n(image.data + static_cast<int64_t>(y) * image.row_stride_bytes, image.row_stride_bytes,
                    storage.data() + static_cast<int64_t>(y) * padded.row_stride_bytes);
    }
    padded.data = storage.data();
    require(take(backend.detect(padded, request, {})) == expected, "padded stride changed detections");
    m::ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    require(backend.detect(image, request, cancelled).status().code() == m::ErrorCode::kCancelled, "cancel ignored");
    m::ExecutionContext expired;
    expired.deadline = Clock::now() - std::chrono::seconds(1);
    require(backend.detect(image, request, expired).status().code() == m::ErrorCode::kTimeout, "deadline ignored");
    auto bounded = options;
    bounded.work_budget_bytes = 1;
    auto small = take(mi::YoloDetectorBackend::create(bounded));
    require(small.detect(image, request, {}).status().code() == m::ErrorCode::kBudgetExceeded, "budget ignored");
    auto wrong_shape = options;
    ++wrong_shape.num_classes;
    auto incompatible = take(mi::YoloDetectorBackend::create(wrong_shape));
    require(incompatible.detect(image, request, {}).status().code() == m::ErrorCode::kBackendFailure,
            "incompatible real-model class layout was not rejected");
    auto high = request;
    high.min_confidence = 1;
    require(take(backend.detect(image, high, {})).empty(), "confidence=1 did not remove proposals");
    check_session(backend, image, request, expected);
}
void report(const std::string& prefix, const Picture& picture, const mi::NcnnTensor& raw,
            const mi::YoloPreparedInput& box, const std::vector<m::DetectionRegion>& regions, double load_us,
            Timing full, Timing forward, bool contract, int64_t backend_rss) {
    std::ofstream out(prefix + ".json");
    out << std::setprecision(17) << "{\"width\":" << picture.width << ",\"height\":" << picture.height
        << ",\"raw_shape\":[" << raw.channels << ',' << raw.height << ',' << raw.width << "],\"resized\":["
        << box.resized_width << ',' << box.resized_height << "],\"load_us\":" << load_us
        << ",\"full_p50_us\":" << full.median << ",\"full_p95_us\":" << full.p95
        << ",\"forward_p50_us\":" << forward.median << ",\"forward_p95_us\":" << forward.p95
        << ",\"contract_checks\":" << (contract ? "true" : "false");
#if defined(__linux__)
    out << ",\"backend_peak_rss_kib\":" << backend_rss;
#else
    (void)backend_rss;
#endif
    out << ",\"detections\":[";
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& r = regions[i];
        out << (i != 0 ? "," : "") << "{\"class\":" << r.class_id << ",\"score\":" << r.confidence << ",\"bounds\":["
            << r.bounds.x << ',' << r.bounds.y << ',' << r.bounds.width << ',' << r.bounds.height << "]}";
    }
    out << "]}\n";
    require(static_cast<bool>(out), "report write failed");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(
            argc == 9 || argc == 10,
            "usage: eval param bin classes input.ppm output-prefix iterations confidence contract(0/1) [bilinear114]");
        mi::YoloDetectorOptions options;
        options.param_path = argv[1];
        options.bin_path = argv[2];
        options.num_classes = std::stoi(argv[3]);
        if (argc == 10) {
            require(std::string(argv[9]) == "bilinear114", "unknown preprocessing profile");
            options.interpolation = mi::YoloInterpolation::kBilinear;
            options.pad_value = 114;
        }
        options.identity = {"offline-evaluation", "explicit-local-model"};
        const auto picture = load(argv[4]);
        const std::string prefix = argv[5];
        const int iterations = std::stoi(argv[6]);
        require(iterations >= 1 && iterations <= 1000, "iteration bound");
        m::DetectionRequest request;
        request.min_confidence = std::stof(argv[7]);
        const bool contract = std::string(argv[8]) == "1";
        const auto start = Clock::now();
        auto backend = take(mi::YoloDetectorBackend::create(options));
        const auto load_us = elapsed(start);
        const auto image = view(picture);
        const auto detections = take(backend.detect(image, request, {}));
        const auto full = measure(iterations, [&] {
            require(take(backend.detect(image, request, {})) == detections, "repeated inference changed results");
        });
        int64_t backend_rss = 0;
#if defined(__linux__)
        // Public sys/resource.h supplies rusage; avoid private glibc headers.
        // NOLINTNEXTLINE(misc-include-cleaner)
        rusage backend_usage{};
        getrusage(RUSAGE_SELF, &backend_usage);
        backend_rss = backend_usage.ru_maxrss;
#endif
        const auto boxed = take(
            mi::prepare_yolo_input(image, options.input_side, options.interpolation, options.pad_value, kBudget, {}));
        const auto& input = boxed.tensor;
        auto runtime = take(mi::NcnnRuntime::create({options.param_path, options.bin_path, 1}));
        const auto raw = take(runtime.run("x", input, "out", {}));
        const auto forward = measure(iterations, [&] { (void)take(runtime.run("x", input, "out", {})); });
        if (contract) {
            check_contract(backend, options, image, request, detections);
        }
        dump(prefix + ".input.f32", input.data);
        dump(prefix + ".raw.f32", raw.data);
        report(prefix, picture, raw, boxed, detections, load_us, full, forward, contract, backend_rss);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
