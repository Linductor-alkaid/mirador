#ifndef MIRADOR_DETECTOR_BACKEND_HPP
#define MIRADOR_DETECTOR_BACKEND_HPP

#include <mirador/backend_info.hpp>
#include <mirador/cache_policy.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/transform.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mirador {

/// Raw detection capability result: one box proposal (design section 8). The
/// class vocabulary is open (UI element proposer, icon detector, generic
/// object model); no anchor, feature-map or training-framework concept ever
/// appears here (design section 14, DEC-002).
struct DetectionRegion {
    RectF bounds;
    int32_t class_id = -1;
    std::string label;
    float confidence = 0.0F;
};

/// Detection execution request (design section 14, DEC-012 field contract).
/// Field consumers mirror OcrRequest: the Mirador pipeline consumes
/// `roi`/`roi_space`, `max_side`, `output_space` and `cache_policy`; the
/// Backend consumes `min_confidence` and `backend_params`.
struct DetectionRequest {
    std::optional<RectF> roi;
    CoordinateSpaceId roi_space = CoordinateSpaceId::kOriented;
    float min_confidence = 0.0F;
    int32_t max_side = 0;
    std::string backend_params;
    CoordinateSpaceId output_space = CoordinateSpaceId::kOriented;
    CachePolicy cache_policy = CachePolicy::kReadWrite;
};

/// Synchronous detection Backend SPI. Same contract as OcrBackend: regions are
/// returned in prepared-image pixel space, implementations honor
/// `min_confidence`, poll `context`, stay deterministic, and leave pipeline
/// fields untouched (DEC-012).
class DetectorBackend {
public:
    virtual ~DetectorBackend() = default;

    [[nodiscard]] virtual BackendInfo info() const = 0;

    virtual Result<std::vector<DetectionRegion>> detect(const ImageView& prepared_image,
                                                        const DetectionRequest& request,
                                                        const ExecutionContext& context) = 0;
};

}  // namespace mirador

#endif  // MIRADOR_DETECTOR_BACKEND_HPP
