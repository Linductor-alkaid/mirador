#ifndef MIRADOR_INTEGRATIONS_DETECTOR_YOLO_BACKEND_HPP
#define MIRADOR_INTEGRATIONS_DETECTOR_YOLO_BACKEND_HPP

#include "ncnn_runtime.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/detection_postprocess.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mirador::integrations {

/// Model identity fields (see PpOcrModelIdentity): `model_id`/`model_revision`
/// flow into BackendInfo and therefore into capability cache keys (RULE-07).
struct YoloModelIdentity {
    std::string model_id;
    std::string model_revision;
};

/// Options for the YOLO-family reference detection backend (M5-04, DEC-015).
/// Paths come from the caller; weights are never embedded or downloaded.
struct YoloDetectorOptions {
    std::string param_path;
    std::string bin_path;
    YoloModelIdentity identity;
    /// Number of model classes C; the frozen output contract requires
    /// width == 5 + C per proposal row.
    int32_t num_classes = 80;
    /// Square model input side (YOLOv5/v8 nano convention: 320/416/640).
    int32_t input_side = 640;
    int num_threads = 1;
    /// true (YOLOv5 anchor-based exports): score = objectness * class score.
    /// false (merged-confidence exports): score = class score alone.
    bool use_objectness = true;
    /// Optional class label table; `label` stays empty for ids outside it.
    std::vector<std::string> class_names;
    /// M3 NMS parameters applied after decoding (RULE: reuse, not reinvent).
    mirador::NmsParams nms;
    /// Byte budget for the letterbox buffer.
    int64_t work_budget_bytes = int64_t{16} * 1024 * 1024;
    /// Explicit candidate budget (RULE-06): more survivors than this after
    /// confidence filtering fail with kBudgetExceeded.
    int32_t max_candidates = 1024;
};

/// YOLO-family reference detection backend (M5-03 sibling, DEC-015): one ncnn
/// model behind the mirador::DetectorBackend SPI (DEC-012). The frozen model
/// output contract is the YOLOv5 single-tensor layout — planar CHW with
/// channels == 1, height == proposal count N, width == 5 + C; each row is
/// [cx, cy, w, h, objectness, class_0 .. class_{C-1}] in model-input pixel
/// space. Conversions whose raw output differs (v8 split heads, anchor
/// decode, sigmoid placement) must normalize to this layout in their param
/// files; this reference deliberately does not reinvent head decoding.
///
/// `detect` letterboxes the prepared image (pad 0, x/255), decodes surviving
/// rows, maps boxes back to prepared-image pixel space (DEC-012 contract),
/// and applies the M3 `nms` reference. `request.min_confidence` is
/// backend-owned; `roi`/`max_side`/`output_space`/`cache_policy` stay
/// pipeline-owned. Accepts kRgb8 only (the pipeline converts).
class YoloDetectorBackend final : public mirador::DetectorBackend {
public:
    static Result<YoloDetectorBackend> create(const YoloDetectorOptions& options);

    [[nodiscard]] mirador::BackendInfo info() const override;
    mirador::Result<std::vector<mirador::DetectionRegion>> detect(const mirador::ImageView& prepared_image,
                                                                  const mirador::DetectionRequest& request,
                                                                  const mirador::ExecutionContext& context) override;

private:
    YoloDetectorOptions options_;
    NcnnRuntime runtime_;
    mirador::BackendInfo info_;
};

}  // namespace mirador::integrations

#endif  // MIRADOR_INTEGRATIONS_DETECTOR_YOLO_BACKEND_HPP
