// M3-11: runtime-free example walking the traditional-vision line from design
// sections 15 and 24 M3 - a synthetic road scene (gray bytes only) goes
// through the LineDetector SPI, and the raw segment set is filtered by angle
// and length and then merged collinearly, exactly the composable utilities a
// road-boundary consumer (robotics, driver assistance) would reuse. The same
// pipeline serves screen-divider detection on desktop terminals; nothing here
// is Android-specific, no model runs and no network or disk is touched.

#include <mirador/image_view.hpp>
#include <mirador/line_detector.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/segment_growing_line_detector.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "mirador/geometry.hpp"

namespace {

using mirador::ImageView;
using mirador::LineDetectRequest;
using mirador::LineFilterParams;
using mirador::LineSegment;
using mirador::PixelFormat;
using mirador::SegmentGrowingLineDetector;

constexpr int32_t kWidth = 200;
constexpr int32_t kHeight = 150;

/// Synthetic road scene: dark asphalt, a bright horizontal horizon line and
/// two bright lane markings converging toward the vanishing point. Pure
/// deterministic painting, no resources.
void paint_road(std::vector<std::byte>& bytes) {
    const int64_t stride = kWidth;
    bytes.assign(static_cast<size_t>(kWidth) * kHeight, std::byte{30});  // asphalt
    const auto set = [&](int32_t x, int32_t y, uint8_t value) {
        if (x >= 0 && x < kWidth && y >= 0 && y < kHeight) {
            bytes[static_cast<size_t>(y) * stride + x] = static_cast<std::byte>(value);
        }
    };
    // Horizon: row 40, full width.
    for (int32_t x = 4; x < kWidth - 4; ++x) {
        set(x, 40, 210);
    }
    // Lane markings: converging diagonals (3:2 slope), dashed for realism.
    for (int32_t t = 0; t < 108; ++t) {
        const int32_t y = 46 + t;
        set(60 - t / 2, y, 230);
        set(140 + t / 2, y, 230);
    }
}

ImageView gray_view(const std::vector<std::byte>& bytes) {
    ImageView view;
    view.data = bytes.data();
    view.width = kWidth;
    view.height = kHeight;
    view.row_stride_bytes = kWidth;
    view.format = PixelFormat::kGray8;
    return view;
}

void print_segments(const char* title, const std::vector<LineSegment>& segments) {
    std::printf("%s: %zu segment(s)\n", title, segments.size());
    for (const LineSegment& segment : segments) {
        std::printf("  (%.1f, %.1f) -> (%.1f, %.1f)  angle %.1f deg  length %.1f  confidence %.2f\n", segment.begin.x,
                    segment.begin.y, segment.end.x, segment.end.y, mirador::segment_angle_deg(segment),
                    mirador::segment_length(segment), segment.confidence);
    }
}

}  // namespace

int main() {
    std::vector<std::byte> scene;
    paint_road(scene);

    SegmentGrowingLineDetector detector;
    const LineDetectRequest request;
    const auto detected = detector.detect(gray_view(scene), request, {});
    if (!detected.ok()) {
        std::printf("detection failed: %s\n", detected.status().message().c_str());
        return 1;
    }
    print_segments("raw ridges (both sides of every marking)", detected.value().segments);

    // The horizon is near-horizontal; the lane markings are steep diagonals.
    LineFilterParams horizon_filter;
    horizon_filter.min_angle_deg = 0.0;
    horizon_filter.max_angle_deg = 5.0;
    horizon_filter.min_length = 30.0;
    const auto horizon = mirador::filter_segments(detected.value().segments, horizon_filter);
    if (!horizon.ok()) {
        std::printf("filter failed: %s\n", horizon.status().message().c_str());
        return 1;
    }

    LineFilterParams lane_filter;
    // A 2:1 marking slope sits at ~63 deg; the segment orientation may be
    // reported from either endpoint (63 or 116 mod 180), so keep both.
    lane_filter.min_angle_deg = 55.0;
    lane_filter.max_angle_deg = 125.0;
    lane_filter.min_length = 40.0;
    const auto lanes = mirador::filter_segments(detected.value().segments, lane_filter);
    if (!lanes.ok()) {
        std::printf("filter failed: %s\n", lanes.status().message().c_str());
        return 1;
    }

    mirador::CollinearMergeParams merge_params;
    merge_params.angle_tolerance_deg = 3.0;
    merge_params.distance_tolerance = 3.0;
    merge_params.gap_tolerance = 8.0;
    const auto merged_lanes = mirador::merge_collinear(lanes.value(), merge_params);
    if (!merged_lanes.ok()) {
        std::printf("merge failed: %s\n", merged_lanes.status().message().c_str());
        return 1;
    }

    print_segments("horizon candidates", horizon.value());
    print_segments("merged lane markings", merged_lanes.value());
    std::printf("road tour done: %zu raw ridge(s) -> %zu horizon + %zu lane segment(s)\n",
                detected.value().segments.size(), horizon.value().size(), merged_lanes.value().size());
    return 0;
}
