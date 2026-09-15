#ifndef MIRADOR_ADAPTERS_CAPTURE_ANDROID_DISPLAY_TRANSFORM_HPP
#define MIRADOR_ADAPTERS_CAPTURE_ANDROID_DISPLAY_TRANSFORM_HPP

#include <mirador/result.hpp>
#include <mirador/transform.hpp>

#include <cstdint>

namespace mirador::adapters {

/// DEC-016 kOriented -> kDisplay mapping for a MediaProjection VirtualDisplay
/// that resized the physical display (`display_width` x `display_height`
/// pixels) into a `buffer_width` x `buffer_height` capture buffer: a pure
/// scale in both axes — no rotation, no padding. The Java side reads the
/// real display size from DisplayMetrics and passes it in; core never
/// queries platform metrics (RULE-01).
///
/// Errors: kInvalidArgument for non-positive sizes. Never throws.
[[nodiscard]] Result<Transform2D> projection_display_transform(int32_t buffer_width, int32_t buffer_height,
                                                               int32_t display_width, int32_t display_height) noexcept;

}  // namespace mirador::adapters

#endif  // MIRADOR_ADAPTERS_CAPTURE_ANDROID_DISPLAY_TRANSFORM_HPP
