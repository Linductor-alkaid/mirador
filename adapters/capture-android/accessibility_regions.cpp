#include "accessibility_regions.hpp"

#include <mirador/geometry.hpp>
#include <mirador/status.hpp>

#include <cmath>

namespace mirador::adapters {

Result<ExternalRegion> to_external_region(const AndroidNodeRegion& node) noexcept {
    if (node.right < node.left || node.bottom < node.top) {
        return Status{ErrorCode::kInvalidArgument, "node bounds must satisfy right >= left and bottom >= top"};
    }
    if (!std::isfinite(node.confidence)) {
        return Status{ErrorCode::kInvalidArgument, "node confidence must be finite"};
    }
    ExternalRegion region;
    region.bounds = RectF{static_cast<float>(node.left), static_cast<float>(node.top),
                          static_cast<float>(node.right - node.left), static_cast<float>(node.bottom - node.top)};
    region.text = node.text;
    region.role = node.role;
    region.description = node.description;
    region.confidence = node.confidence;
    region.interactive = node.interactive;
    region.enabled = node.enabled;
    return region;
}

}  // namespace mirador::adapters
