// DP-01: identical Release harness before/after. Optional external P6 pixels
// stay local. Synthetic defaults need no model, capture or third-party runtime.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mirador/change_detection.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/object_tracker.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/segment_growing_line_detector.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <string>
#include <vector>

namespace m = mirador;
template <class T>
T take(m::Result<T> r) {
    if (!r) {
        throw std::runtime_error(r.status().message());
    }
    return r.take_value();
}
// Plain benchmark-owned input with an explicit view, not an encapsulated model.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct Pixels {
    int w = 400, h = 240;
    std::vector<std::byte> bytes;
    [[nodiscard]] m::ImageView view() const {
        m::ImageView v;
        v.data = bytes.data();
        v.width = w;
        v.height = h;
        v.row_stride_bytes = static_cast<int64_t>(w) * 3;
        v.format = m::PixelFormat::kRgb8;
        return v;
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)
template <class F>
void measure(const std::string& name, int count, F operation) {
    for (int i = 0; i < 2; ++i) {
        operation();
    }
    std::vector<double> times;
    for (int i = 0; i < count; ++i) {
        auto start = std::chrono::steady_clock::now();
        operation();
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    std::cout << name << "," << times[times.size() / 2] << "," << times[(times.size() - 1) * 95 / 100] << "\n";
}
Pixels read_pixels(const char* path) {
    Pixels pixels;
    if (path != nullptr) {
        std::ifstream file(path, std::ios::binary);
        std::string magic;
        int max = 0;
        file >> magic >> pixels.w >> pixels.h >> max;
        file.get();
        if (magic != "P6" || pixels.w < 1 || pixels.h < 1 || pixels.w > 32768 || pixels.h > 32768 ||
            static_cast<std::int64_t>(pixels.w) * pixels.h * 3 > (32U << 20) || max != 255) {
            throw std::runtime_error("invalid bounded P6");
        }
        pixels.bytes.resize(static_cast<std::size_t>(pixels.w) * pixels.h * 3);
        if (!file.read(reinterpret_cast<char*>(pixels.bytes.data()),
                       static_cast<std::streamsize>(pixels.bytes.size()))) {
            throw std::runtime_error("short P6");
        }
    } else {
        pixels.bytes.resize(static_cast<std::size_t>(pixels.w) * pixels.h * 3);
        for (int y = 0; y < pixels.h; ++y) {
            for (int x = 0; x < pixels.w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    pixels.bytes[(static_cast<std::size_t>(y) * pixels.w + x) * 3 + c] =
                        std::byte((x * 13 + y * 29 + (x * y) % 31 + c * 37) & 255);
                }
            }
        }
    }
    return pixels;
}

int main(int argc, char** argv) {
    try {
        auto pixels = read_pixels(argc > 1 ? argv[1] : nullptr);
        const int iterations = argc > 2 ? std::stoi(argv[2]) : 11;
        if (iterations < 1 || iterations > 1000) {
            throw std::runtime_error("iterations out of range");
        }
        auto changed = pixels;
        for (auto& p : changed.bytes) {
            p ^= std::byte{255};
        }
        m::VisualRegion region;
        region.stable_id = 1;
        region.confidence = 1;
        region.bounds = argc > 1 ? m::RectF{55, 530, 98, 27} : m::RectF{160, 90, 40, 28};
        region.anchor = {region.bounds.x + region.bounds.width / 2, region.bounds.y + region.bounds.height / 2};
        auto tracker = take(m::ObjectTracker::create());
        (void)take(tracker.adopt_track(region, pixels.view(), 1));
        auto verification = take(tracker.verify_track(1, pixels.view(), std::nullopt));
        std::cout << std::setprecision(17) << "# ncc=" << verification.appearance.peak_ncc
                  << " psr=" << verification.appearance.peak_sidelobe_ratio
                  << " dx=" << verification.appearance.best_offset_dx
                  << " dy=" << verification.appearance.best_offset_dy << "\n";
        std::cout << "case,median_us,p95_us\n";
        measure("tracker", iterations, [&] { (void)take(tracker.verify_track(1, pixels.view(), std::nullopt)); });
        measure("change_same", iterations, [&] { (void)take(m::detect_change(pixels.view(), pixels.view(), {})); });
        measure("change_changed", iterations, [&] { (void)take(m::detect_change(pixels.view(), changed.view(), {})); });
        std::vector<std::byte> gray(std::size_t{300} * 180, std::byte{255});
        for (int y = 50; y < 130; ++y) {
            for (int x = 70; x < 220; ++x) {
                if (x < 72 || x >= 218 || y < 52 || y >= 128) {
                    gray[static_cast<std::size_t>(y) * 300 + x] = std::byte{0};
                }
            }
        }
        m::ImageView v;
        v.data = gray.data();
        v.width = 300;
        v.height = 180;
        v.row_stride_bytes = 300;
        v.format = m::PixelFormat::kGray8;
        m::SegmentGrowingLineDetector detector;
        auto lines = take(detector.detect(v, {}, {}));
        std::cout << "# thin_outline_lines=" << lines.segments.size() << "\n";
        measure("thin_outline", iterations, [&] { (void)take(detector.detect(v, {}, {})); });
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
