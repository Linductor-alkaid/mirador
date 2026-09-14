#include <cstddef>
#include <mirador/text_postprocess.hpp>

#include "connected_components.h"
#include "mirador/geometry.hpp"
#include "mirador/image_view.hpp"
#include "mirador/ocr_backend.hpp"
#include "mirador/result.hpp"
#include "mirador/status.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace mirador {
namespace {

bool is_valid(const DbPostprocessParams& params) noexcept {
    if (params.binarize_threshold < 1) {
        return false;
    }
    if (!(params.unclip_ratio >= 0.0) || params.unclip_ratio > 10.0 || !std::isfinite(params.unclip_ratio)) {
        return false;
    }
    if (params.min_box_pixels < 0) {
        return false;
    }
    if (!(params.min_mean_score >= 0.0) || params.min_mean_score > 1.0 || !std::isfinite(params.min_mean_score)) {
        return false;
    }
    if (params.max_boxes < 1 || params.max_boxes > 65535 || params.work_budget_bytes < 1024) {
        return false;
    }
    return true;
}

bool is_valid(const ContourBoxParams& params) noexcept {
    if (params.foreground_threshold < 1 || params.min_box_pixels < 0) {
        return false;
    }
    if (params.max_boxes < 1 || params.max_boxes > 65535 || params.work_budget_bytes < 1024) {
        return false;
    }
    return true;
}

/// DB unclip for an axis-aligned box: expand each side by
/// `area * ratio / perimeter`, clamped to the map rectangle.
RectF unclip_aabb(const RectF& box, double ratio, const RectF& map) noexcept {
    const double perimeter = 2.0 * (static_cast<double>(box.width) + box.height);
    if (perimeter <= 0.0) {
        return box;
    }
    const double offset = box.width * box.height * ratio / perimeter;
    const double left = std::max<double>(map.x, box.x - offset);
    const double top = std::max<double>(map.y, box.y - offset);
    const double right = std::min<double>(map.x + map.width, box.x + box.width + offset);
    const double bottom = std::min<double>(map.y + map.height, box.y + box.height + offset);
    return RectF{static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left),
                 static_cast<float>(bottom - top)};
}

RectF component_bounds(const image_internal::Component& component) noexcept {
    return RectF{static_cast<float>(component.min_x), static_cast<float>(component.min_y),
                 static_cast<float>(component.width()), static_cast<float>(component.height())};
}

}  // namespace

Result<std::vector<TextRegion>> db_postprocess_aabb(const ImageView& probability_map,
                                                    const DbPostprocessParams& params) noexcept {
    if (!is_valid(params)) {
        return Status(ErrorCode::kInvalidArgument, "DB postprocess parameters out of range");
    }
    const RectF map_rect{0.0F, 0.0F, static_cast<float>(probability_map.width),
                         static_cast<float>(probability_map.height)};
    auto components =
        image_internal::find_components(probability_map, params.binarize_threshold, params.work_budget_bytes);
    if (!components.ok()) {
        return Status(components.status().code(), components.status().message());
    }

    std::vector<TextRegion> boxes;
    for (const image_internal::Component& component : components.value()) {
        if (boxes.size() >= static_cast<size_t>(params.max_boxes)) {
            return Status(ErrorCode::kBudgetExceeded, "box cap exceeded");
        }
        if (component.pixel_count < params.min_box_pixels) {
            continue;
        }
        const double mean_score =
            static_cast<double>(component.value_sum) / 255.0 / static_cast<double>(component.pixel_count);
        if (mean_score < params.min_mean_score) {
            continue;
        }
        TextRegion region;
        region.bounds = unclip_aabb(component_bounds(component), params.unclip_ratio, map_rect);
        region.confidence = static_cast<float>(mean_score);
        boxes.push_back(region);
    }
    return boxes;
}

Result<std::vector<RectF>> recover_contour_boxes(const ImageView& binary, const ContourBoxParams& params) noexcept {
    if (!is_valid(params)) {
        return Status(ErrorCode::kInvalidArgument, "contour box parameters out of range");
    }
    auto components = image_internal::find_components(binary, params.foreground_threshold, params.work_budget_bytes);
    if (!components.ok()) {
        return Status(components.status().code(), components.status().message());
    }

    std::vector<RectF> boxes;
    for (const image_internal::Component& component : components.value()) {
        if (boxes.size() >= static_cast<size_t>(params.max_boxes)) {
            return Status(ErrorCode::kBudgetExceeded, "box cap exceeded");
        }
        if (component.pixel_count < params.min_box_pixels) {
            continue;
        }
        boxes.push_back(component_bounds(component));
    }
    return boxes;
}

}  // namespace mirador
