#ifndef MIRADOR_INTEGRATIONS_PPOCR_BACKEND_HPP
#define MIRADOR_INTEGRATIONS_PPOCR_BACKEND_HPP

#include "ncnn_runtime.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>
#include <mirador/text_postprocess.hpp>

#include <string>
#include <vector>

namespace mirador::integrations {

/// Shared identity fields for the PP-OCR reference backends. `model_id` and
/// `model_revision` flow into BackendInfo and therefore into capability cache
/// keys (RULE-07): callers must change them when the model files change.
struct PpOcrModelIdentity {
    std::string model_id;
    std::string model_revision;
};

/// Detection-stage options (DBNet reference, M5-03). Paths come from the
/// caller; weights are never embedded or downloaded (DEC-015).
struct PpOcrDetOptions {
    std::string param_path;
    std::string bin_path;
    PpOcrModelIdentity identity;
    /// Square model input side; the prepared image is letterboxed to
    /// side x side (pad 0, normalized to x/127.5 - 1).
    int32_t det_side = 960;
    int num_threads = 1;
    /// M3 DB postprocess parameters (binarize threshold, unclip, budgets).
    DbPostprocessParams postprocess;
    /// Byte budget for the letterbox and probability-map buffers.
    int64_t work_budget_bytes = int64_t{16} * 1024 * 1024;
};

/// PP-OCR text detection reference backend (M5-03, DEC-015): one ncnn model
/// producing a 1 x side x side probability map, postprocessed by the M3
/// `db_postprocess_aabb` reference chain. `recognize` returns boxes in
/// prepared-image pixel space (DEC-012 contract); `utf8_text` stays empty
/// (detection only). Accepts kRgb8 only — the perception pipeline converts
/// to the first accepted format (DEC-012).
class PpOcrDetBackend final : public OcrBackend {
public:
    static Result<PpOcrDetBackend> create(const PpOcrDetOptions& options);

    [[nodiscard]] BackendInfo info() const override;
    Result<std::vector<TextRegion>> recognize(const ImageView& prepared_image, const OcrRequest& request,
                                              const ExecutionContext& context) override;

private:
    PpOcrDetOptions options_;
    NcnnRuntime runtime_;
    BackendInfo info_;
};

/// Recognition-stage options (CRNN/SVTR-style line recognizer reference).
struct PpOcrRecOptions {
    std::string param_path;
    std::string bin_path;
    /// Dictionary file: one UTF-8 character per line; file line i (0-based)
    /// is class i+1 and class 0 is the CTC blank. Provided by the caller.
    std::string charset_path;
    PpOcrModelIdentity identity;
    /// Model input size (PP-OCR rec: 3 x rec_height x rec_width).
    int32_t rec_height = 48;
    int32_t rec_width = 320;
    int num_threads = 1;
    int64_t work_budget_bytes = int64_t{16} * 1024 * 1024;
};

/// PP-OCR text recognition reference backend (M5-03): treats the prepared
/// image as ONE text line (letterbox to rec_height x rec_width, normalize to
/// x/127.5 - 1), runs the model and greedy-CTC-decodes the output. The model
/// output contract is planar CHW with channels = time steps and width =
/// class count (height 1); conversions whose raw output differs must reshape
/// in their param files. For full-image OCR use `PpOcrBackend`, which feeds
/// det boxes to this stage line by line. Accepts kRgb8 only.
class PpOcrRecBackend final : public OcrBackend {
public:
    static Result<PpOcrRecBackend> create(const PpOcrRecOptions& options);

    [[nodiscard]] BackendInfo info() const override;
    Result<std::vector<TextRegion>> recognize(const ImageView& prepared_image, const OcrRequest& request,
                                              const ExecutionContext& context) override;

    [[nodiscard]] int32_t rec_height() const noexcept { return options_.rec_height; }
    [[nodiscard]] int32_t rec_width() const noexcept { return options_.rec_width; }
    [[nodiscard]] int64_t work_budget_bytes() const noexcept { return options_.work_budget_bytes; }

private:
    PpOcrRecOptions options_;
    NcnnRuntime runtime_;
    std::vector<std::string> charset_;  ///< class 1 .. N (class 0 is the blank)
    BackendInfo info_;
};

/// Combined detection + recognition pipeline reference backend (M5-03):
/// `recognize` runs the det stage, orders the boxes (top-to-bottom,
/// left-to-right, deterministic tie-break), crops each box from the prepared
/// image and runs the rec stage to fill `utf8_text` and `confidence`. Boxes
/// below `min_confidence` are dropped before recognition (backend-owned
/// field, DEC-012).
class PpOcrBackend final : public OcrBackend {
public:
    static Result<PpOcrBackend> create(const PpOcrDetOptions& det_options, const PpOcrRecOptions& rec_options);

    [[nodiscard]] BackendInfo info() const override;
    Result<std::vector<TextRegion>> recognize(const ImageView& prepared_image, const OcrRequest& request,
                                              const ExecutionContext& context) override;

private:
    PpOcrBackend(PpOcrDetBackend det, PpOcrRecBackend rec) noexcept;
    PpOcrDetBackend det_;
    PpOcrRecBackend rec_;
    BackendInfo info_;
};

}  // namespace mirador::integrations

#endif  // MIRADOR_INTEGRATIONS_PPOCR_BACKEND_HPP
