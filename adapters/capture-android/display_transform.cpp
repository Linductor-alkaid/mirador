#include "display_transform.hpp"

#include <mirador/status.hpp>

namespace mirador::adapters {

Result<Transform2D> projection_display_transform(int32_t buffer_width, int32_t buffer_height, int32_t display_width,
                                                 int32_t display_height) noexcept {
    if (buffer_width <= 0 || buffer_height <= 0 || display_width <= 0 || display_height <= 0) {
        return Status{ErrorCode::kInvalidArgument, "buffer and display sizes must be positive"};
    }
    return make_scale(static_cast<double>(display_width) / static_cast<double>(buffer_width),
                      static_cast<double>(display_height) / static_cast<double>(buffer_height),
                      CoordinateSpaceId::kOriented, CoordinateSpaceId::kDisplay);
}

}  // namespace mirador::adapters
