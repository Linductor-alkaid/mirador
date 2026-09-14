#ifndef MIRADOR_OCR_BACKEND_HPP
#define MIRADOR_OCR_BACKEND_HPP

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

/// Raw OCR capability result, one text line or block (design section 8). All
/// geometry lives in a coordinate space declared by the producer; SPI
/// implementations report prepared-image pixel space, and the perception
/// pipeline recovers it to the requested output space (DEC-012).
struct TextRegion {
    RectF bounds;
    std::string utf8_text;
    float confidence = 0.0F;
    std::vector<PointF> polygon;  ///< optional tight quad/coutline; empty when not produced

    /// Component equality (M3-07, test convenience).
    [[nodiscard]] friend bool operator==(const TextRegion& lhs, const TextRegion& rhs) noexcept {
        return lhs.bounds == rhs.bounds && lhs.utf8_text == rhs.utf8_text && lhs.confidence == rhs.confidence &&
               lhs.polygon == rhs.polygon;
    }
};

/// OCR execution request (design section 13, DEC-012 field contract). One type
/// serves both the session entry point and the SPI; each field has exactly one
/// consumer:
/// - consumed by the Mirador pipeline: `roi`/`roi_space` (crop), `max_side`
///   (long-edge downscale cap, 0 = uncapped), `output_space` (coordinate
///   recovery target), `cache_policy`;
/// - consumed by the Backend: `min_confidence` (result filtering),
///   `language_hint`, `backend_params` (opaque, implementation-defined, part
///   of the cache-key parameter digest).
struct OcrRequest {
    /// Region of interest; nullopt selects the whole frame. RectF coordinates
    /// are continuous pixel-area coordinates in `roi_space`.
    std::optional<RectF> roi;
    CoordinateSpaceId roi_space = CoordinateSpaceId::kOriented;
    std::string language_hint;  ///< e.g. "zh-Hans"; empty = implementation default
    float min_confidence = 0.0F;
    int32_t max_side = 0;
    std::string backend_params;  ///< opaque backend-private parameters
    CoordinateSpaceId output_space = CoordinateSpaceId::kOriented;
    CachePolicy cache_policy = CachePolicy::kReadWrite;
};

/// Synchronous OCR Backend SPI (design section 9, DEC-001/DEC-012). The
/// implementation owns model loading, runtime sessions and inference; Mirador
/// owns request normalization, common pre/post-processing, coordinate
/// recovery, caching and result organization. No runtime type ever crosses
/// this boundary (DEC-002): the stable contract is images and domain results.
class OcrBackend {
public:
    virtual ~OcrBackend() = default;

    /// Capability and identity query. The result feeds cache keys (RULE-07),
    /// format gating and diagnostics; implementations should return the
    /// currently loaded model state.
    [[nodiscard]] virtual BackendInfo info() const = 0;

    /// Recognizes text on an already-prepared image and returns regions in
    /// prepared-image pixel space ([0, w) x [0, h)); the caller recovers them
    /// to `request.output_space` (DEC-012 coordinate contract). Implementations
    /// must honor `request.min_confidence` and poll `context` (return
    /// kCancelled/kTimeout when observed, never silently swallow them), and
    /// must be deterministic for equal inputs (cache correctness). Fields
    /// `roi`/`roi_space`/`max_side`/`output_space`/`cache_policy` belong to the
    /// pipeline and must not be interpreted here.
    virtual Result<std::vector<TextRegion>> recognize(const ImageView& prepared_image, const OcrRequest& request,
                                                      const ExecutionContext& context) = 0;
};

}  // namespace mirador

#endif  // MIRADOR_OCR_BACKEND_HPP
