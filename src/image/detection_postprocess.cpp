#include <mirador/detection_postprocess.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mirador {
namespace {

bool is_finite(const DetectionRegion& region) noexcept {
    return std::isfinite(region.bounds.x) && std::isfinite(region.bounds.y) && std::isfinite(region.bounds.width) &&
           std::isfinite(region.bounds.height) && std::isfinite(region.confidence);
}

}  // namespace

double intersection_over_union(const RectF& first, const RectF& second) noexcept {
    const double first_area = static_cast<double>(first.width) * first.height;
    const double second_area = static_cast<double>(second.width) * second.height;
    if (first_area <= 0.0 || second_area <= 0.0) {
        return 0.0;
    }
    const double left = std::max<double>(first.x, second.x);
    const double top = std::max<double>(first.y, second.y);
    const double right = std::min<double>(first.x + first.width, second.x + second.width);
    const double bottom = std::min<double>(first.y + first.height, second.y + second.height);
    const double width = std::max(0.0, right - left);
    const double height = std::max(0.0, bottom - top);
    const double intersection = width * height;
    return intersection / (first_area + second_area - intersection);
}

Result<std::vector<DetectionRegion>> nms(std::span<const DetectionRegion> regions, const NmsParams& params) {
    if (!std::isfinite(params.iou_threshold) || params.iou_threshold < 0.0F || params.iou_threshold > 1.0F) {
        return Status(ErrorCode::kInvalidArgument, "iou_threshold must lie in [0, 1]");
    }
    if (params.max_output < 0) {
        return Status(ErrorCode::kInvalidArgument, "max_output must be non-negative");
    }
    for (const DetectionRegion& region : regions) {
        if (!is_finite(region)) {
            return Status(ErrorCode::kInvalidArgument, "detections must carry finite coordinates");
        }
    }

    std::vector<size_t> order(regions.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [regions](size_t a, size_t b) {
        const float ca = regions[a].confidence;
        const float cb = regions[b].confidence;
        if (ca != cb) {
            return ca > cb;
        }
        return a < b;
    });

    std::vector<bool> suppressed(regions.size(), false);
    std::vector<DetectionRegion> kept;
    for (const size_t i : order) {
        if (suppressed[i]) {
            continue;
        }
        if (params.max_output > 0 && kept.size() >= static_cast<size_t>(params.max_output)) {
            break;
        }
        kept.push_back(regions[i]);
        for (const size_t j : order) {
            if (j == i || suppressed[j]) {
                continue;
            }
            if (params.class_aware && regions[j].class_id != regions[i].class_id) {
                continue;
            }
            if (intersection_over_union(regions[i].bounds, regions[j].bounds) >
                static_cast<double>(params.iou_threshold)) {
                suppressed[j] = true;
            }
        }
    }
    return kept;
}

Result<std::vector<DetectionRegion>> filter_detections(std::span<const DetectionRegion> regions,
                                                       const DetectionFilterParams& params) {
    if (!std::isfinite(params.min_confidence) || params.min_confidence < 0.0F) {
        return Status(ErrorCode::kInvalidArgument, "min_confidence must be a finite non-negative value");
    }
    for (const DetectionRegion& region : regions) {
        if (!is_finite(region)) {
            return Status(ErrorCode::kInvalidArgument, "detections must carry finite coordinates");
        }
    }

    const auto keeps_class = [&params](int32_t class_id) {
        return params.class_ids.empty() ||
               std::find(params.class_ids.begin(), params.class_ids.end(), class_id) != params.class_ids.end();
    };
    const auto keeps_label = [&params](const std::string& label) {
        return params.labels.empty() ||
               std::find(params.labels.begin(), params.labels.end(), label) != params.labels.end();
    };

    std::vector<DetectionRegion> kept;
    for (const DetectionRegion& region : regions) {
        if (region.confidence < params.min_confidence) {
            continue;
        }
        if (!keeps_class(region.class_id) || !keeps_label(region.label)) {
            continue;
        }
        kept.push_back(region);
    }
    return kept;
}

}  // namespace mirador
