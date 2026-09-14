#include <cstddef>
#include <cstdint>
#include <mirador/detection_postprocess.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <span>
#include <vector>
#include "mirador/detector_backend.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"

namespace mirador {
namespace {

bool is_finite(const DetectionRegion& region) noexcept {
    return std::isfinite(region.bounds.x) && std::isfinite(region.bounds.y) && std::isfinite(region.bounds.width) &&
           std::isfinite(region.bounds.height) && std::isfinite(region.confidence);
}

[[nodiscard]] Status validate_regions(std::span<const DetectionRegion> regions) noexcept {
    for (const DetectionRegion& region : regions) {
        if (!is_finite(region)) {
            return {ErrorCode::kInvalidArgument, "detections must carry finite coordinates"};
        }
    }
    return Status::success();
}

/// Marks every unconsumed candidate suppressed by the just-kept proposal.
void suppress_overlaps(std::span<const DetectionRegion> regions, std::span<const size_t> order, size_t kept_index,
                       std::vector<bool>& suppressed, const NmsParams& params) noexcept {
    for (const size_t j : order) {
        if (j == kept_index || suppressed[j]) {
            continue;
        }
        if (params.class_aware && regions[j].class_id != regions[kept_index].class_id) {
            continue;
        }
        if (intersection_over_union(regions[kept_index].bounds, regions[j].bounds) >
            static_cast<double>(params.iou_threshold)) {
            suppressed[j] = true;
        }
    }
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
    if (const Status invalid = validate_regions(regions); !invalid.ok()) {
        return invalid;
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
        suppress_overlaps(regions, order, i, suppressed, params);
    }
    return kept;
}

Result<std::vector<DetectionRegion>> filter_detections(std::span<const DetectionRegion> regions,
                                                       const DetectionFilterParams& params) {
    if (!std::isfinite(params.min_confidence) || params.min_confidence < 0.0F) {
        return Status(ErrorCode::kInvalidArgument, "min_confidence must be a finite non-negative value");
    }
    if (const Status invalid = validate_regions(regions); !invalid.ok()) {
        return invalid;
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
