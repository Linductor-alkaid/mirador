#ifndef MIRADOR_ADAPTERS_CAPTURE_ANDROID_ACCESSIBILITY_REGIONS_HPP
#define MIRADOR_ADAPTERS_CAPTURE_ANDROID_ACCESSIBILITY_REGIONS_HPP

#include <mirador/evidence.hpp>
#include <mirador/result.hpp>

#include <string>

namespace mirador::adapters {

/// One accessibility node collected on the Java side (AccessibilityNodeInfo
/// bounds in display pixels, Android rect convention [left, top, right,
/// bottom]). Android/JNI types never cross this header (RULE-02); this
/// struct is the boundary value type the JNI bridge fills.
struct AndroidNodeRegion {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    std::string text;         ///< node text or content description
    std::string role;         ///< platform role (e.g. "button"); never vision-inferred (RULE-11)
    std::string description;  ///< caller-provided context, passed through
    float confidence = 1.0F;  ///< platform facts are certain by default
    bool interactive = false;
    bool enabled = true;
};

/// Converts one collected node into an `ExternalRegion` in kDisplay space
/// (Android view bounds are display coordinates; callers fuse with
/// `FusionOptions::display_transform`, DEC-016, when the capture buffer lives
/// in a different space). Errors: kInvalidArgument for right < left or
/// bottom < top — degenerate zero-size nodes are kept, gating belongs to the
/// fusion thresholds. Never throws.
[[nodiscard]] Result<ExternalRegion> to_external_region(const AndroidNodeRegion& node) noexcept;

}  // namespace mirador::adapters

#endif  // MIRADOR_ADAPTERS_CAPTURE_ANDROID_ACCESSIBILITY_REGIONS_HPP
