#ifndef MIRADOR_INTEGRATIONS_PPOCR_INTERNAL_HPP
#define MIRADOR_INTEGRATIONS_PPOCR_INTERNAL_HPP

#include "ncnn_runtime.hpp"
#include "ppocr_backend.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mirador::integrations::ppocr_detail {

/// CTC blank class index of the reference PP-OCR rec models.
inline constexpr int32_t kRecBlankClass = 0;
/// Maximum charset entries a dictionary file may carry (factory budget).
inline constexpr size_t kMaxCharsetEntries = 65536;
/// Border value of the reference detection canvas (ppocrv5.cpp pads the /32
/// border with the YOLO-style constant 114 before ImageNet normalization).
inline constexpr std::uint8_t kDetPadValue = 114;

/// Format/cancel/deadline precheck shared by every backend entry point.
[[nodiscard]] Status precheck(const ImageView& image, const ExecutionContext& context);

/// CHW float tensor in, planar-normalized in place: v / 127.5 - 1 (the
/// PP-OCR convention for pad-0 letterboxed inputs). Polls cancellation.
[[nodiscard]] Result<void> normalize_ppocr(NcnnTensor& tensor, const ExecutionContext& context);

/// Swaps the packed RGB planes to BGR: the reference deployment (and the
/// PaddleOCR export behind it) consumes cv BGR planes.
[[nodiscard]] Result<void> swap_planes_to_bgr(NcnnTensor& tensor);

/// Reference detection canvas: the prepared image is never upscaled, resized
/// so its longer side fits `limit` (exact area weights), and copied into a
/// /32-aligned canvas whose border is filled with 114 (ppocrv5.cpp `detect`:
/// pad to the stride multiple, near-centered split).
struct DetCanvas {
    ImageBuffer buffer;
    float scale = 1.0F;  // image -> canvas
    int32_t pad_left = 0;
    int32_t pad_top = 0;
};

[[nodiscard]] Result<DetCanvas> make_det_canvas(const ImageView& image, int32_t limit, int64_t budget);

/// Maps a canvas-space box back to prepared-image space through the inverse
/// pad/scale transform of the detection canvas and clips it to the image.
[[nodiscard]] Result<RectF> unpad_bounds(const RectF& bounds, float scale, int32_t pad_left, int32_t pad_top,
                                         const ImageView& prepared);

/// Shared identity fields for the reference backend infos.
[[nodiscard]] BackendInfo make_info(const char* name, const PpOcrModelIdentity& identity);

/// Loads the rec dictionary: one UTF-8 character per line; file line i
/// (0-based) is class i+1 and class 0 is the CTC blank. Factory budget applies.
[[nodiscard]] Result<std::vector<std::string>> load_charset(const std::string& charset_path);

/// One decoded recognition line: raw model output, before any confidence
/// gating (the budgeted path reports low scores instead of dropping them).
struct RecLineOutput {
    std::string utf8_text;
    float confidence = 0.0F;
};

/// Optional nanosecond split of one rec atomic unit (M8-01 stage accounting;
/// benchmark diagnostics, never part of results or logs).
struct RecUnitTiming {
    int64_t preprocess_ns = 0;  ///< resize + pack + plane swap + normalize
    int64_t forward_ns = 0;     ///< one ncnn forward
    int64_t decode_ns = 0;      ///< CTC decode + charset text assembly
};

/// Single-line recognition unit (M8 atomic unit, DEC-025 section 3): resize to
/// `options.rec_height` with the aspect ratio preserved (width capped at
/// `options.rec_width`), pack/swap/normalize, one forward, greedy CTC decode
/// through `charset`. Errors from every stage propagate; cancellation and
/// deadline surface as kCancelled/kTimeout. `timing` (nullable) accumulates
/// the stage split of this unit.
[[nodiscard]] Result<RecLineOutput> run_rec_line(const PpOcrRecOptions& options,
                                                 const std::vector<std::string>& charset, const NcnnRuntime& runtime,
                                                 const ImageView& line_image, const ExecutionContext& context,
                                                 RecUnitTiming* timing = nullptr);

}  // namespace mirador::integrations::ppocr_detail

#endif  // MIRADOR_INTEGRATIONS_PPOCR_INTERNAL_HPP
