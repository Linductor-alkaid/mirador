// M8-03 (DEC-025): independent verification smoke test for the budgeted
// PP-OCR backend. Reuses the synthetic-model fixtures of ppocr_smoke.cpp — a
// constant-probability det model (1x1 kernel, bias 0.9, exactly one full-frame
// box) and a constant-logit rec model (whole-input 320x48 kernel decoding
// "ABCD") generated at runtime in a unique temp directory, no real weights, no
// network access. Covers: create() validation and identity reporting, the
// det->select->budgeted-rec happy path with count conservation, K boundaries
// and det-side min_confidence gating, the soft-budget stop with deferred
// geometry, deadline/cancel propagation (no work on entry gates), the
// finalize-reserve guard, per-call stage timing reset and request validation.
//
// Frame shape note: the frozen rec fixture kernel covers the whole 320x48
// input, so the det line crop (the full frame for the constant det map) must
// resize to exactly 320x48 — a 20:3 frame aspect (400x60, same as the
// ppocr_smoke combined check). A taller frame (e.g. 200x150) would resize to
// 64x48 and fail inside the fixture model itself, not in the budgeted code
// under test, so the happy-path frames here are 400x60.
//
// Round 3 addition — multi-candidate scenarios with an input-sensitive det
// fixture: 1x1 convolution, per-channel weight -0.1744 and bias 0.4410
// applied to the normalized input (v' = v/127.5 - 1), so gray 235 (scene
// background) -> p ~ 0.00, gray 40 (block) -> p ~ 0.80 (passes binarize 77
// and min_mean_score 0.6), and the canvas pad value 114 -> p ~ 0.496 (byte
// 127): the pad ring is foreground but is dropped by the score filter before
// any cap counting (components scan top-left first). Scene: 400x60, six 40x6
// blocks at rows y=15/38, columns x=40/180/320. Unclip offset = 40*6*1.5 /
// (2*(40+6)) = 3.913 px, so the recovered det box measures 47.826 x 13.826;
// the crop ROI rounds to 48x14 and run_rec_line resizes that to
// round(48*48/14) = 165 px width — the paired rec fixture therefore uses a
// 165x48 whole-input kernel, measured from the live det output at runtime
// (self-calibrating; the coordinator estimate of 166 skipped the crop-ROI
// integer rounding step).
//
// det_limit_hit==true is closed with a separate 4-block scene at
// max_boxes=3 (kept boxes == cap + 1 -> probe returns 4). A 6-block scene at
// max_boxes=3 cannot truncate: the frozen db_postprocess cap check fires for
// any component once `boxes.size()` reaches the cap (before the score drop),
// so the max_boxes+1 probe saturates and the whole call fails with
// kBudgetExceeded — that explicit-failure boundary is asserted too.
//
// ncnn param/bin layout notes are shared with ppocr_smoke.cpp (pinned ncnn
// commit e54f7b1): Convolution param keys 7..18 are omitted except kernel_h
// (key 11); .bin blobs carry a leading flag word (0 = raw float32) while conv
// bias blobs are raw float32 without a flag word.

#include "ppocr_backend.hpp"
#include "ppocr_budgeted.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

int g_failed_checks = 0;

void expect_true(bool condition, const std::string& what) {
    if (condition) {
        std::printf("ok: %s\n", what.c_str());
    } else {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failed_checks;
    }
}

void expect_code(const mirador::Status& status, mirador::ErrorCode expected, const std::string& what) {
    const bool matched = !status.ok() && status.code() == expected;
    if (matched) {
        std::printf("ok: %s\n", what.c_str());
    } else {
        std::printf("FAIL: %s (expected %s, got %s)\n", what.c_str(), mirador::error_code_name(expected),
                    status.ok() ? "Ok" : mirador::error_code_name(status.code()));
        ++g_failed_checks;
    }
}

// Det model: Input(640x640x3, blob "x") -> Convolution(1 out channel, 1x1
// kernel, bias) producing blob "out". All-zero weights make the probability
// map equal the bias (0.9) everywhere, independent of the input pixels.
constexpr std::string_view kDetParamText = R"(7767517
2 2
Input data 0 1 x 0=640 1=640 2=3
Convolution conv0 1 1 x out 0=1 1=1 2=1 3=1 4=0 5=1 6=3
)";

// Rec model: Input(320x48x3, blob "x") -> Convolution(20 out channels, kernel
// 320x48 covering the whole input, bias) -> Reshape(5x1x4) producing "out".
// Channels = 4 time steps, width = 5 classes (0 = blank). The per-channel
// biases put argmax of time step t at class (t % 4) + 1, so the decoded line
// is the class sequence 1,2,3,4.
constexpr std::string_view kRecParamText = R"(7767517
3 3
Input data 0 1 x 0=320 1=48 2=3
Convolution conv0 1 1 x conv_out 0=20 1=320 2=1 3=1 4=0 5=1 6=921600 11=48
Reshape reshape0 1 1 conv_out out 0=5 1=1 2=4
)";

constexpr int32_t kDetSide = 640;
constexpr int32_t kRecWidth = 320;
constexpr int32_t kRecHeight = 48;
constexpr int32_t kRecClasses = 5;
constexpr int32_t kRecConvOutputs = 20;      // time steps (4) x classes (5)
constexpr int32_t kRecWeightCount = 921600;  // 20 outputs x 3 channels x 48 x 320
constexpr int32_t kDetMaxBoxes = 512;        // configured det cap reported as det_limit

// Happy-path frame: 20:3 aspect so the full-frame det crop resizes to exactly
// 320x48 (see the file header note).
constexpr int32_t kFrameWidth = 400;
constexpr int32_t kFrameHeight = 60;

void append_uint32_le(std::vector<std::byte>& out, uint32_t bits) {
    out.push_back(static_cast<std::byte>(bits & 0xFFU));
    out.push_back(static_cast<std::byte>((bits >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>((bits >> 16U) & 0xFFU));
    out.push_back(static_cast<std::byte>((bits >> 24U) & 0xFFU));
}

void append_float32_le(std::vector<std::byte>& out, float value) {
    static_assert(sizeof(float) == 4, "test assumes 32-bit IEEE floats");
    uint32_t bits = 0;
    static_cast<void>(std::memcpy(&bits, &value, sizeof(bits)));
    append_uint32_le(out, bits);
}

bool write_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    stream.close();
    return stream.good();
}

bool write_det_model(const std::filesystem::path& dir) {
    const bool param_ok = write_file(dir / "det.param", kDetParamText.data(), kDetParamText.size());
    // ncnn .bin container: flag word (0 = raw float32) + 3 kernel weights
    // (1x1 kernel x 3 input channels, all zero) + raw bias float (no flag).
    std::vector<std::byte> bin;
    bin.reserve(20U);
    append_uint32_le(bin, 0U);
    for (int i = 0; i < 3; ++i) {
        append_float32_le(bin, 0.0F);
    }
    append_float32_le(bin, 0.9F);
    const bool bin_ok = write_file(dir / "det.bin", bin.data(), bin.size());
    return param_ok && bin_ok;
}

bool write_rec_model(const std::filesystem::path& dir) {
    const bool param_ok = write_file(dir / "rec.param", kRecParamText.data(), kRecParamText.size());
    // flag word + 921600 all-zero float32 weights + 20 raw bias floats.
    // bias[t * classes + j] = 1.0 if j == (t % 4) + 1 else 0.0.
    constexpr size_t k_bias_offset = 4U + static_cast<size_t>(kRecWeightCount) * 4U;
    std::vector<std::byte> bin(k_bias_offset + static_cast<size_t>(kRecConvOutputs) * 4U, std::byte{0});
    for (int32_t flat = 0; flat < kRecConvOutputs; ++flat) {
        const int32_t step = flat / kRecClasses;
        const int32_t cls = flat % kRecClasses;
        if (cls != (step % 4) + 1) {
            continue;
        }
        const size_t offset = k_bias_offset + static_cast<size_t>(flat) * 4U;
        uint32_t bits = 0;
        const float value = 1.0F;
        static_cast<void>(std::memcpy(&bits, &value, sizeof(bits)));
        bin[offset + 0U] = static_cast<std::byte>(bits & 0xFFU);
        bin[offset + 1U] = static_cast<std::byte>((bits >> 8U) & 0xFFU);
        bin[offset + 2U] = static_cast<std::byte>((bits >> 16U) & 0xFFU);
        bin[offset + 3U] = static_cast<std::byte>((bits >> 24U) & 0xFFU);
    }
    const bool bin_ok = write_file(dir / "rec.bin", bin.data(), bin.size());
    return param_ok && bin_ok;
}

bool write_text_file(const std::filesystem::path& path, std::string_view content) {
    return write_file(path, content.data(), content.size());
}

// Unique deterministic temp directory; no randomness and no network access.
std::filesystem::path make_temp_dir() {
    std::error_code base_ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(base_ec);
    if (base_ec) {
        return {};
    }
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::error_code ec;
        std::filesystem::path candidate = base / ("mirador_ppocr_budgeted_smoke_" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            return {};
        }
    }
    return {};
}

mirador::ImageView make_rgb_view(const std::vector<std::byte>& bytes, int32_t width, int32_t height) {
    mirador::ImageView view;
    view.data = bytes.data();
    view.width = width;
    view.height = height;
    view.row_stride_bytes = static_cast<int64_t>(width) * 3;
    view.format = mirador::PixelFormat::kRgb8;
    return view;
}

// Deterministic non-trivial scene; the fixture models are bias-driven, so the
// pixel values only exercise the canvas/resize/normalize/crop path.
std::vector<std::byte> make_rgb_bytes(int32_t width, int32_t height) {
    std::vector<std::byte> bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const auto value = static_cast<uint8_t>((x * 2 + y * 7) % 251);
            const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3U;
            bytes[offset + 0U] = static_cast<std::byte>(value);
            bytes[offset + 1U] = static_cast<std::byte>(value);
            bytes[offset + 2U] = static_cast<std::byte>(value);
        }
    }
    return bytes;
}

bool bounds_close(const mirador::RectF& bounds, float x, float y, float width, float height, float tolerance) {
    return std::fabs(bounds.x - x) <= tolerance && std::fabs(bounds.y - y) <= tolerance &&
           std::fabs(bounds.width - width) <= tolerance && std::fabs(bounds.height - height) <= tolerance;
}

bool timings_equal(const mirador::integrations::PpOcrStageTimings& lhs,
                   const mirador::integrations::PpOcrStageTimings& rhs) {
    return lhs.det_canvas_ns == rhs.det_canvas_ns && lhs.det_pack_normalize_ns == rhs.det_pack_normalize_ns &&
           lhs.det_forward_ns == rhs.det_forward_ns && lhs.det_postprocess_ns == rhs.det_postprocess_ns &&
           lhs.select_ns == rhs.select_ns && lhs.crop_ns == rhs.crop_ns &&
           lhs.rec_preprocess_ns == rhs.rec_preprocess_ns && lhs.rec_forward_ns == rhs.rec_forward_ns &&
           lhs.rec_decode_ns == rhs.rec_decode_ns && lhs.finalize_ns == rhs.finalize_ns;
}

mirador::integrations::PpOcrBudgetedOptions budgeted_options(const std::filesystem::path& dir) {
    mirador::integrations::PpOcrBudgetedOptions options;
    options.det.param_path = (dir / "det.param").string();
    options.det.bin_path = (dir / "det.bin").string();
    options.det.identity = mirador::integrations::PpOcrModelIdentity{"m8-det", "r1"};
    options.det.det_side = kDetSide;
    options.det.postprocess.max_boxes = kDetMaxBoxes;
    options.rec.param_path = (dir / "rec.param").string();
    options.rec.bin_path = (dir / "rec.bin").string();
    options.rec.charset_path = (dir / "charset.txt").string();
    options.rec.identity = mirador::integrations::PpOcrModelIdentity{"m8-rec", "r1"};
    options.rec.rec_height = kRecHeight;
    options.rec.rec_width = kRecWidth;
    return options;
}

mirador::OcrBatchRequest batch_request(int32_t max_rec_lines) {
    mirador::OcrBatchRequest request;
    request.max_rec_lines = max_rec_lines;
    return request;
}

const char* stop_reason_name(mirador::OcrBatchStopReason reason) {
    switch (reason) {
        case mirador::OcrBatchStopReason::kCompleted:
            return "kCompleted";
        case mirador::OcrBatchStopReason::kRecLineLimit:
            return "kRecLineLimit";
        case mirador::OcrBatchStopReason::kSoftBudget:
            return "kSoftBudget";
    }
    return "unknown";
}

// Check 1: documented create() validation and identity reporting.
void check_create_validation(const std::filesystem::path& dir) {
    auto valid = mirador::integrations::PpOcrBudgetedBackend::create(budgeted_options(dir));
    expect_true(valid.ok(), "budgeted create loads the synthetic det/rec fixtures and charset");
    if (valid.ok()) {
        auto backend = std::move(valid).take_value();
        const mirador::BackendInfo info = backend.info();
        expect_true(info.name == "ppocr-budgeted-ncnn-reference",
                    "create: info().name is ppocr-budgeted-ncnn-reference");
        expect_true(info.model_id == "m8-det+m8-rec", "create: model_id joins the det and rec identities");
        expect_true(info.model_revision == "r1+r1", "create: model_revision joins the det and rec revisions");
    }

    auto bad_side = budgeted_options(dir);
    bad_side.det.det_side = 100;  // not a multiple of 32
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(bad_side).status(),
                mirador::ErrorCode::kInvalidArgument, "create: det_side=100 (not /32) -> kInvalidArgument");

    auto big_side = budgeted_options(dir);
    big_side.det.det_side = 4128;  // multiple of 32 but above 4096
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(big_side).status(),
                mirador::ErrorCode::kInvalidArgument, "create: det_side=4128 (>4096) -> kInvalidArgument");

    auto zero_boxes = budgeted_options(dir);
    zero_boxes.det.postprocess.max_boxes = 0;
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(zero_boxes).status(),
                mirador::ErrorCode::kInvalidArgument, "create: max_boxes=0 -> kInvalidArgument");

    auto many_boxes = budgeted_options(dir);
    many_boxes.det.postprocess.max_boxes = 65536;
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(many_boxes).status(),
                mirador::ErrorCode::kInvalidArgument, "create: max_boxes=65536 -> kInvalidArgument");

    auto short_rec = budgeted_options(dir);
    short_rec.rec.rec_height = 8;
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(short_rec).status(),
                mirador::ErrorCode::kInvalidArgument, "create: rec_height=8 (<16) -> kInvalidArgument");

    auto wide_rec = budgeted_options(dir);
    wide_rec.rec.rec_width = 2048;
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(wide_rec).status(),
                mirador::ErrorCode::kInvalidArgument, "create: rec_width=2048 (>1024) -> kInvalidArgument");

    auto negative_unit = budgeted_options(dir);
    negative_unit.rec_unit_upper_bound = std::chrono::milliseconds{-1};
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(negative_unit).status(),
                mirador::ErrorCode::kInvalidArgument, "create: negative rec_unit_upper_bound -> kInvalidArgument");

    auto missing_det = budgeted_options(dir);
    missing_det.det.param_path = (dir / "missing.param").string();
    missing_det.det.bin_path = (dir / "missing.bin").string();
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(missing_det).status(),
                mirador::ErrorCode::kBackendUnavailable, "create: missing det model files -> kBackendUnavailable");

    auto missing_rec = budgeted_options(dir);
    missing_rec.rec.bin_path = (dir / "missing.bin").string();
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(missing_rec).status(),
                mirador::ErrorCode::kBackendUnavailable, "create: missing rec model file -> kBackendUnavailable");

    auto missing_charset = budgeted_options(dir);
    missing_charset.rec.charset_path = (dir / "missing_charset.txt").string();
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(missing_charset).status(),
                mirador::ErrorCode::kBackendUnavailable, "create: missing charset file -> kBackendUnavailable");

    auto empty_charset = budgeted_options(dir);
    empty_charset.rec.charset_path = (dir / "charset_empty.txt").string();
    static_cast<void>(write_text_file(empty_charset.rec.charset_path, ""));
    expect_code(mirador::integrations::PpOcrBudgetedBackend::create(empty_charset).status(),
                mirador::ErrorCode::kBackendUnavailable, "create: empty charset file -> kBackendUnavailable");
}

// Check 2: happy path — one full-frame det candidate, K=1, everything counts
// out; the required stage timings are all accounted after the call.
void check_happy_path(mirador::integrations::PpOcrBudgetedBackend& backend, const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    const mirador::ExecutionContext never_cancelled;

    const auto outcome = backend.recognize_budgeted(view, batch_request(1), never_cancelled);
    expect_true(outcome.ok(), "happy: recognize_budgeted succeeds");
    if (!outcome.ok()) {
        std::printf("  happy status: %s\n", outcome.status().message().c_str());
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();

    expect_true(result.detected_count == 1, "happy: exactly one candidate detected");
    expect_true(result.recognized_count == 1, "happy: recognized_count == 1");
    expect_true(result.deferred_count == 0, "happy: deferred_count == 0");
    expect_true(result.reused_count == 0, "happy: reused_count == 0");
    expect_true(result.rec_forward_count == 1, "happy: exactly one rec forward");
    expect_true(result.stop_reason == mirador::OcrBatchStopReason::kCompleted, "happy: stop_reason == kCompleted");
    expect_true(result.completion == mirador::OcrBatchCompletion::kComplete, "happy: completion == kComplete");
    expect_true(result.det_limit == kDetMaxBoxes, "happy: det_limit reports the configured max_boxes");
    expect_true(!result.det_limit_hit, "happy: det_limit_hit is false");
    expect_true(ocr_batch_result_consistent(result), "happy: ocr_batch_result_consistent holds");

    expect_true(result.lines.size() == 1, "happy: exactly one line record");
    if (result.lines.size() == 1) {
        const mirador::OcrLineRecord& line = result.lines.front();
        expect_true(line.utf8_text == "ABCD", "happy: line decodes to \"ABCD\"");
        expect_true(line.state == mirador::OcrCompletionState::kRecognized, "happy: line state is kRecognized");
        expect_true(
            line.rec_confidence.has_value() && line.rec_confidence.value() > 0.0F && line.rec_confidence.value() < 1.0F,
            "happy: rec_confidence is set and in (0, 1)");
        expect_true(line.execution_order == 0 && line.candidate_id == 0,
                    "happy: execution_order == 0 and candidate_id == 0");
        expect_true(line.det_confidence > 0.8F && line.det_confidence <= 1.0F,
                    "happy: det_confidence is the ~0.9 fixture map score");
        expect_true(bounds_close(line.bounds, 0.0F, 0.0F, static_cast<float>(kFrameWidth),
                                 static_cast<float>(kFrameHeight), 2.0F),
                    "happy: bounds recover the full-frame det box (tolerance 2 px)");
        expect_true(line.bounds.width > 0.0F && line.bounds.height > 0.0F && line.bounds.x >= 0.0F &&
                        line.bounds.y >= 0.0F &&
                        line.bounds.x + line.bounds.width <= static_cast<float>(kFrameWidth) + 1e-3F &&
                        line.bounds.y + line.bounds.height <= static_cast<float>(kFrameHeight) + 1e-3F,
                    "happy: bounds are non-degenerate and inside the frame");
    }

    const mirador::integrations::PpOcrStageTimings timings = backend.last_stage_timings();
    expect_true(timings.det_forward_ns > 0, "timings: det_forward_ns > 0 after the happy call");
    expect_true(timings.rec_forward_ns > 0, "timings: rec_forward_ns > 0 after the happy call");
    expect_true(timings.select_ns > 0, "timings: select_ns > 0 after the happy call");
    expect_true(timings.finalize_ns > 0, "timings: finalize_ns > 0 after the happy call");
}

// Check: rec scores are reported, never text-gated — min_confidence filters at
// the det stage only (the 0.9 det score passes 0.5; the ~0.405 rec score stays).
void check_rec_scores_not_gated(mirador::integrations::PpOcrBudgetedBackend& backend,
                                const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    mirador::OcrBatchRequest request = batch_request(1);
    request.min_confidence = 0.5F;
    const mirador::ExecutionContext never_cancelled;

    const auto outcome = backend.recognize_budgeted(view, request, never_cancelled);
    expect_true(outcome.ok(), "rec-gating: recognize_budgeted succeeds with min_confidence=0.5");
    if (!outcome.ok()) {
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();
    expect_true(result.detected_count == 1 && result.recognized_count == 1,
                "rec-gating: the ~0.9 det box passes min_confidence=0.5 and is recognized");
    if (result.lines.size() == 1) {
        const mirador::OcrLineRecord& line = result.lines.front();
        expect_true(line.state == mirador::OcrCompletionState::kRecognized && line.rec_confidence.has_value() &&
                        line.rec_confidence.value() < 0.5F,
                    "rec-gating: the ~0.4 rec score is reported, not text-gated");
    }
}

// Check 3: K > N completes; a fully filtered candidate set completes empty.
void check_k_boundaries(mirador::integrations::PpOcrBudgetedBackend& backend,
                        const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    const mirador::ExecutionContext never_cancelled;

    const auto large_k = backend.recognize_budgeted(view, batch_request(64), never_cancelled);
    expect_true(large_k.ok(), "K=64 (N=1): recognize_budgeted succeeds");
    if (large_k.ok()) {
        const mirador::OcrBatchResult& result = large_k.value();
        expect_true(result.recognized_count == 1 && result.deferred_count == 0 && result.rec_forward_count == 1,
                    "K=64 (N=1): min(K, N)=1 line recognized, nothing deferred");
        expect_true(result.stop_reason == mirador::OcrBatchStopReason::kCompleted &&
                        result.completion == mirador::OcrBatchCompletion::kComplete,
                    "K=64 (N=1): stop kCompleted and completion kComplete");
        expect_true(ocr_batch_result_consistent(result), "K=64 (N=1): report is consistent");
    }

    mirador::OcrBatchRequest filtered = batch_request(1);
    filtered.min_confidence = 0.95F;  // the ~0.9 det score drops out at det time
    const auto empty_set = backend.recognize_budgeted(view, filtered, never_cancelled);
    expect_true(empty_set.ok(), "min_confidence=0.95: recognize_budgeted succeeds");
    if (empty_set.ok()) {
        const mirador::OcrBatchResult& result = empty_set.value();
        expect_true(result.detected_count == 0 && result.lines.empty(),
                    "min_confidence=0.95: the ~0.9 det box is filtered before selection");
        expect_true(result.recognized_count == 0 && result.rec_forward_count == 0,
                    "min_confidence=0.95: no recognition ran");
        expect_true(result.stop_reason == mirador::OcrBatchStopReason::kCompleted &&
                        result.completion == mirador::OcrBatchCompletion::kComplete,
                    "min_confidence=0.95: an empty candidate set still completes");
        expect_true(result.det_limit == kDetMaxBoxes && !result.det_limit_hit,
                    "min_confidence=0.95: det diagnostics still reported");
        expect_true(ocr_batch_result_consistent(result), "min_confidence=0.95: report is consistent");
    }
}

// Check 4: soft_budget=0 stops before the first rec unit and keeps queryable
// det geometry on the deferred line.
void check_soft_budget(mirador::integrations::PpOcrBudgetedBackend& backend,
                       const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    mirador::OcrBatchRequest request = batch_request(4);
    request.soft_budget = std::chrono::milliseconds{0};
    const mirador::ExecutionContext never_cancelled;

    const auto outcome = backend.recognize_budgeted(view, request, never_cancelled);
    expect_true(outcome.ok(), "soft budget 0 ms: recognize_budgeted returns a partial report, not an error");
    if (!outcome.ok()) {
        std::printf("  soft budget status: %s\n", outcome.status().message().c_str());
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();
    expect_true(result.rec_forward_count == 0 && result.recognized_count == 0, "soft budget 0 ms: no rec forward ran");
    expect_true(result.deferred_count == 1, "soft budget 0 ms: the candidate is deferred");
    expect_true(result.stop_reason == mirador::OcrBatchStopReason::kSoftBudget,
                "soft budget 0 ms: stop_reason == kSoftBudget");
    expect_true(result.completion == mirador::OcrBatchCompletion::kPartial, "soft budget 0 ms: completion == kPartial");
    expect_true(ocr_batch_result_consistent(result), "soft budget 0 ms: report is consistent");

    expect_true(result.lines.size() == 1, "soft budget 0 ms: the deferred line stays queryable");
    if (result.lines.size() == 1) {
        const mirador::OcrLineRecord& line = result.lines.front();
        expect_true(line.state == mirador::OcrCompletionState::kDeferred && line.utf8_text.empty() &&
                        !line.rec_confidence.has_value() && line.execution_order == -1,
                    "soft budget 0 ms: deferred line carries no text, no rec score, no execution order");
        expect_true(line.candidate_id == 0 && line.det_confidence > 0.8F,
                    "soft budget 0 ms: deferred line keeps its candidate id and det score");
        expect_true(bounds_close(line.bounds, 0.0F, 0.0F, static_cast<float>(kFrameWidth),
                                 static_cast<float>(kFrameHeight), 2.0F),
                    "soft budget 0 ms: deferred bounds equal the recovered det box");
    }
}

// Check 5: hard deadline at entry fails the whole call before det; a tight
// deadline plus a large finalize reserve either times out or soft-stops.
void check_deadline_guards(mirador::integrations::PpOcrBudgetedBackend& backend,
                           const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);

    {
        // Deadline already past when the call enters: precheck must reject the
        // whole call with kTimeout and no stage (not even det) may run.
        const mirador::integrations::PpOcrStageTimings snapshot = backend.last_stage_timings();
        mirador::ExecutionContext expired;
        expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{1};
        const auto outcome = backend.recognize_budgeted(view, batch_request(1), expired);
        expect_code(outcome.status(), mirador::ErrorCode::kTimeout, "expired deadline: whole call fails with kTimeout");
        expect_true(timings_equal(backend.last_stage_timings(), snapshot),
                    "expired deadline: no stage work ran (timings untouched)");
    }

    {
        // finalize_reserve=500 ms against a 1 ms deadline: the first rec unit
        // never fits, so the run either times out during det (machine slower
        // than 1 ms) or soft-stops right after det. Both are contract-valid;
        // anything else is a defect.
        mirador::OcrBatchRequest request = batch_request(8);
        request.finalize_reserve = std::chrono::milliseconds{500};
        mirador::ExecutionContext tight;
        tight.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1};
        const auto outcome = backend.recognize_budgeted(view, request, tight);
        if (outcome.ok()) {
            const mirador::OcrBatchResult& result = outcome.value();
            std::printf("info: finalize-guard run produced a report (stop=%s recognized=%ld deferred=%ld)\n",
                        stop_reason_name(result.stop_reason), static_cast<long>(result.recognized_count),
                        static_cast<long>(result.deferred_count));
            const bool known_stop = result.stop_reason == mirador::OcrBatchStopReason::kSoftBudget ||
                                    result.stop_reason == mirador::OcrBatchStopReason::kCompleted;
            expect_true(known_stop, "finalize guard: stop_reason is kSoftBudget or kCompleted");
            expect_true(result.rec_forward_count == result.recognized_count,
                        "finalize guard: rec forwards equal recognized lines");
            expect_true(ocr_batch_result_consistent(result), "finalize guard: report is consistent");
        } else {
            std::printf("info: finalize-guard run failed with kTimeout (det consumed the 1 ms deadline)\n");
            expect_code(outcome.status(), mirador::ErrorCode::kTimeout,
                        "finalize guard: with det over deadline the whole call is kTimeout");
        }
    }
}

// Check (round 2): deterministic finalize-guard soft stop. A rec unit upper
// bound of 1 hour means the remaining time of a 5 s deadline can never fit
// one unit plus the 500 ms finalize reserve, so the run must soft-stop right
// after det — deterministically, on any machine. The guard lives only in the
// rec loop (never before det), so det itself always runs and reports.
void check_finalize_guard_soft_stop(const std::filesystem::path& dir, const std::vector<std::byte>& frame_bytes) {
    auto options = budgeted_options(dir);
    options.rec_unit_upper_bound = std::chrono::hours{1};
    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(options);
    expect_true(backend_result.ok(), "finalize soft stop: backend with 1 h rec_unit_upper_bound loads");
    if (!backend_result.ok()) {
        return;
    }
    auto backend = std::move(backend_result).take_value();

    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    mirador::OcrBatchRequest request = batch_request(4);
    request.finalize_reserve = std::chrono::milliseconds{500};
    mirador::ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};

    const auto outcome = backend.recognize_budgeted(view, request, context);
    expect_true(outcome.ok(), "finalize soft stop: recognize_budgeted returns a report, not kTimeout");
    if (!outcome.ok()) {
        std::printf("  finalize soft stop status: %s\n", outcome.status().message().c_str());
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();
    expect_true(result.detected_count == 1,
                "finalize soft stop: det ran and detected the candidate (no guard before det)");
    expect_true(result.stop_reason == mirador::OcrBatchStopReason::kSoftBudget,
                "finalize soft stop: stop_reason == kSoftBudget");
    expect_true(result.rec_forward_count == 0 && result.recognized_count == 0,
                "finalize soft stop: no rec forward ran");
    expect_true(result.deferred_count == 1 && result.lines.size() == 1,
                "finalize soft stop: the candidate is deferred and stays queryable");
    expect_true(result.completion == mirador::OcrBatchCompletion::kPartial,
                "finalize soft stop: completion == kPartial");
    expect_true(ocr_batch_result_consistent(result), "finalize soft stop: report is consistent");
}

// Check (round 2): det_limit comes from the configured max_boxes cap. A cap
// of exactly 1 with exactly one detected box must not report det_limit_hit
// (a hit requires strictly more boxes than the cap) and must not change the
// run's completeness.
void check_det_limit_report(const std::filesystem::path& dir, const std::vector<std::byte>& frame_bytes) {
    auto options = budgeted_options(dir);
    options.det.postprocess.max_boxes = 1;
    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(options);
    expect_true(backend_result.ok(), "det_limit=1: backend with max_boxes=1 loads");
    if (!backend_result.ok()) {
        return;
    }
    auto backend = std::move(backend_result).take_value();

    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    const mirador::ExecutionContext never_cancelled;

    const auto outcome = backend.recognize_budgeted(view, batch_request(1), never_cancelled);
    expect_true(outcome.ok(), "det_limit=1: recognize_budgeted succeeds on the happy path");
    if (!outcome.ok()) {
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();
    expect_true(result.det_limit == 1, "det_limit=1: det_limit reports the configured cap");
    expect_true(!result.det_limit_hit, "det_limit=1: exactly one box does not hit a cap of 1");
    expect_true(result.detected_count == 1 && result.recognized_count == 1 && result.rec_forward_count == 1,
                "det_limit=1: the single candidate is still recognized");
    expect_true(result.stop_reason == mirador::OcrBatchStopReason::kCompleted &&
                    result.completion == mirador::OcrBatchCompletion::kComplete,
                "det_limit=1: run still completes");
    expect_true(ocr_batch_result_consistent(result), "det_limit=1: report is consistent");
}

// ---------------------------------------------------------------------------
// Round 3: multi-candidate scenarios with the input-sensitive det fixture.

constexpr uint8_t kSceneBackground = 235;  // p ~ 0.00 with the sensitive weights
constexpr uint8_t kSceneBlock = 40;        // p ~ 0.80 with the sensitive weights
constexpr int32_t kBlockWidth = 40;
constexpr int32_t kBlockHeight = 6;

struct BlockPos {
    int32_t x;
    int32_t y;
};

// Analytic unclipped det box for one 40x6 block (DB unclip 1.5):
// offset = area * ratio / perimeter = 40*6*1.5 / (2*(40+6)) = 3.91304 px.
constexpr double kUnclipOffset =
    static_cast<double>(kBlockWidth) * kBlockHeight * 1.5 / (2.0 * (static_cast<double>(kBlockWidth) + kBlockHeight));

std::vector<std::byte> make_block_scene(int32_t width, int32_t height, const std::vector<BlockPos>& blocks) {
    std::vector<std::byte> bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U);
    const auto set_pixel = [&bytes, width](int32_t x, int32_t y, uint8_t value) {
        const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3U;
        bytes[offset + 0U] = static_cast<std::byte>(value);
        bytes[offset + 1U] = static_cast<std::byte>(value);
        bytes[offset + 2U] = static_cast<std::byte>(value);
    };
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            set_pixel(x, y, kSceneBackground);
        }
    }
    for (const BlockPos& block : blocks) {
        for (int32_t dy = 0; dy < kBlockHeight; ++dy) {
            for (int32_t dx = 0; dx < kBlockWidth; ++dx) {
                set_pixel(block.x + dx, block.y + dy, kSceneBlock);
            }
        }
    }
    return bytes;
}

// Input-sensitive det fixture: same param text as the constant fixture (the
// graph is identical), only the bin differs — flag word + 3 channel weights
// (-0.1744) + raw bias 0.4410.
bool write_sensitive_det_model(const std::filesystem::path& dir) {
    const bool param_ok = write_file(dir / "det_sensitive.param", kDetParamText.data(), kDetParamText.size());
    std::vector<std::byte> bin;
    bin.reserve(20U);
    append_uint32_le(bin, 0U);
    for (int i = 0; i < 3; ++i) {
        append_float32_le(bin, -0.1744F);
    }
    append_float32_le(bin, 0.4410F);
    const bool bin_ok = write_file(dir / "det_sensitive.bin", bin.data(), bin.size());
    return param_ok && bin_ok;
}

// Rec fixture with a whole-input kernel of `kernel_w` x 48 so that a line crop
// resizing to exactly kernel_w x 48 yields a single conv output position.
// Same blob layout as write_rec_model (flag word + zero weights + 20 biases
// with bias[t*5 + (t%4+1)] = 1.0), decodes to "ABCD".
bool write_rec_model_with_kernel(const std::filesystem::path& dir, int32_t kernel_w) {
    const int64_t weight_count = static_cast<int64_t>(kRecConvOutputs) * 3 * kRecHeight * kernel_w;
    const std::string param_text =
        "7767517\n3 3\n"
        "Input data 0 1 x 0=" +
        std::to_string(kernel_w) +
        " 1=48 2=3\n"
        "Convolution conv0 1 1 x conv_out 0=20 1=" +
        std::to_string(kernel_w) + " 2=1 3=1 4=0 5=1 6=" + std::to_string(weight_count) +
        " 11=48\n"
        "Reshape reshape0 1 1 conv_out out 0=5 1=1 2=4\n";
    const bool param_ok = write_file(dir / "rec_s.param", param_text.data(), param_text.size());

    constexpr size_t k_flag_bytes = 4U;
    std::vector<std::byte> bin(
        k_flag_bytes + static_cast<size_t>(weight_count) * 4U + static_cast<size_t>(kRecConvOutputs) * 4U,
        std::byte{0});
    for (int32_t flat = 0; flat < kRecConvOutputs; ++flat) {
        const int32_t step = flat / kRecClasses;
        const int32_t cls = flat % kRecClasses;
        if (cls != (step % 4) + 1) {
            continue;
        }
        const size_t offset = k_flag_bytes + static_cast<size_t>(weight_count) * 4U + static_cast<size_t>(flat) * 4U;
        uint32_t bits = 0;
        const float value = 1.0F;
        static_cast<void>(std::memcpy(&bits, &value, sizeof(bits)));
        bin[offset + 0U] = static_cast<std::byte>(bits & 0xFFU);
        bin[offset + 1U] = static_cast<std::byte>((bits >> 8U) & 0xFFU);
        bin[offset + 2U] = static_cast<std::byte>((bits >> 16U) & 0xFFU);
        bin[offset + 3U] = static_cast<std::byte>((bits >> 24U) & 0xFFU);
    }
    const bool bin_ok = write_file(dir / "rec_s.bin", bin.data(), bin.size());
    return param_ok && bin_ok;
}

mirador::integrations::PpOcrDetOptions sensitive_det_options(const std::filesystem::path& dir, int32_t max_boxes) {
    mirador::integrations::PpOcrDetOptions det;
    det.param_path = (dir / "det_sensitive.param").string();
    det.bin_path = (dir / "det_sensitive.bin").string();
    det.identity = mirador::integrations::PpOcrModelIdentity{"m8-det-s", "r1"};
    det.det_side = kDetSide;
    det.postprocess.max_boxes = max_boxes;
    return det;
}

mirador::integrations::PpOcrBudgetedOptions sensitive_budgeted_options(const std::filesystem::path& dir,
                                                                       int32_t max_boxes) {
    mirador::integrations::PpOcrBudgetedOptions options = budgeted_options(dir);
    options.det = sensitive_det_options(dir, max_boxes);
    options.rec.param_path = (dir / "rec_s.param").string();
    options.rec.bin_path = (dir / "rec_s.bin").string();
    options.rec.identity = mirador::integrations::PpOcrModelIdentity{"m8-rec-s", "r1"};
    return options;
}

// Multi-candidate scenario calibration: writes the input-sensitive fixtures,
// measures the live det boxes and derives the rec kernel width the budgeted
// rec stage will actually see (integer crop ROI, then the same resize
// rounding as run_rec_line). Returns the measured kernel width, or 0 after
// reporting a failure.
int32_t check_sensitive_calibration(const std::filesystem::path& dir, const mirador::ImageView& view) {
    const mirador::ExecutionContext never_cancelled;
    expect_true(write_sensitive_det_model(dir), "sensitive det fixture written");
    auto det_result = mirador::integrations::PpOcrDetBackend::create(sensitive_det_options(dir, 256));
    expect_true(det_result.ok(), "sensitive det backend loads");
    if (!det_result.ok()) {
        std::printf("  sensitive det create status: %s\n", det_result.status().message().c_str());
        return 0;
    }
    auto det = std::move(det_result).take_value();
    const auto regions = det.recognize(view, mirador::OcrRequest{}, never_cancelled);
    expect_true(regions.ok() && regions.value().size() == 6, "sensitive det finds exactly 6 blocks");
    if (!regions.ok() || regions.value().size() != 6) {
        return 0;
    }
    const mirador::RectF measured = regions.value().front().bounds;
    expect_true(bounds_close(measured, static_cast<float>(40 - kUnclipOffset), static_cast<float>(15 - kUnclipOffset),
                             static_cast<float>(kBlockWidth + 2 * kUnclipOffset),
                             static_cast<float>(kBlockHeight + 2 * kUnclipOffset), 0.25F),
                "sensitive det box matches the analytic unclip geometry (47.826 x 13.826)");
    const auto crop_w = static_cast<int32_t>(std::lround(measured.width));
    const auto crop_h = static_cast<int32_t>(std::lround(measured.height));
    bool uniform_crop = true;
    for (const mirador::TextRegion& region : regions.value()) {
        uniform_crop = uniform_crop && static_cast<int32_t>(std::lround(region.bounds.width)) == crop_w &&
                       static_cast<int32_t>(std::lround(region.bounds.height)) == crop_h;
    }
    expect_true(uniform_crop, "all 6 det boxes round to the same crop size");
    const int64_t scaled_width = static_cast<int64_t>(crop_w) * kRecHeight;
    const int32_t kernel_w =
        std::clamp(static_cast<int32_t>(std::lround(static_cast<double>(scaled_width) / crop_h)), 1, kRecWidth);
    std::printf("info: det box %.3f x %.3f -> crop %d x %d -> rec kernel width %d (expected 165)\n",
                static_cast<double>(measured.width), static_cast<double>(measured.height), crop_w, crop_h, kernel_w);
    expect_true(crop_w == 48 && crop_h == 14 && kernel_w == 165,
                "crop rounds to 48x14 and the rec kernel width is 165 (round(48*48/14))");
    expect_true(write_rec_model_with_kernel(dir, kernel_w), "sensitive rec fixture written for the measured kernel");
    return kernel_w;
}

// Check A: multi-line happy path — every candidate recognized, report in
// reading order (rows top-to-bottom, then columns left-to-right).
void check_multi_line_happy(const std::filesystem::path& dir, const mirador::ImageView& view) {
    const mirador::ExecutionContext never_cancelled;
    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(sensitive_budgeted_options(dir, 256));
    expect_true(backend_result.ok(), "multi-line: budgeted backend (max_boxes=256) loads");
    if (!backend_result.ok()) {
        return;
    }
    auto backend = std::move(backend_result).take_value();
    const auto outcome = backend.recognize_budgeted(view, batch_request(64), never_cancelled);
    expect_true(outcome.ok(), "multi-line: recognize_budgeted succeeds with K=64");
    if (!outcome.ok()) {
        std::printf("  multi-line status: %s\n", outcome.status().message().c_str());
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();
    expect_true(result.detected_count == 6 && result.recognized_count == 6 && result.deferred_count == 0 &&
                    result.rec_forward_count == 6,
                "multi-line: all 6 candidates recognized with 6 forwards");
    expect_true(result.stop_reason == mirador::OcrBatchStopReason::kCompleted &&
                    result.completion == mirador::OcrBatchCompletion::kComplete && !result.det_limit_hit &&
                    result.det_limit == 256,
                "multi-line: kCompleted/kComplete, cap 256 not hit");
    static constexpr std::array<int32_t, 3> kColumnX = {40, 180, 320};
    bool lines_ok = result.lines.size() == 6;
    for (int32_t index = 0; lines_ok && index < 6; ++index) {
        const mirador::OcrLineRecord& line = result.lines[static_cast<size_t>(index)];
        const int32_t expect_x = kColumnX[index % 3];
        const int32_t expect_y = index < 3 ? 15 : 38;
        lines_ok = lines_ok && line.candidate_id == index && line.utf8_text == "ABCD" &&
                   line.state == mirador::OcrCompletionState::kRecognized && line.execution_order == index &&
                   line.rec_confidence.has_value() && line.det_confidence > 0.75F && line.det_confidence <= 0.85F &&
                   bounds_close(line.bounds, static_cast<float>(expect_x - kUnclipOffset),
                                static_cast<float>(expect_y - kUnclipOffset),
                                static_cast<float>(kBlockWidth + 2 * kUnclipOffset),
                                static_cast<float>(kBlockHeight + 2 * kUnclipOffset), 0.5F);
    }
    expect_true(lines_ok,
                "multi-line: candidate ids 0..5 in reading order (y asc, x asc), each decodes to \"ABCD\"");
    expect_true(ocr_batch_result_consistent(result), "multi-line: report is consistent");
}

// Check B: K < N — the reading-order prefix runs, the tail is deferred, and
// repeated runs are bitwise identical.
void check_k_less_than_n(const std::filesystem::path& dir, const mirador::ImageView& view) {
    const mirador::ExecutionContext never_cancelled;
    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(sensitive_budgeted_options(dir, 6));
    expect_true(backend_result.ok(), "K<N: budgeted backend (max_boxes=6) loads");
    if (!backend_result.ok()) {
        return;
    }
    auto backend = std::move(backend_result).take_value();
    const auto first_outcome = backend.recognize_budgeted(view, batch_request(4), never_cancelled);
    expect_true(first_outcome.ok(), "K<N: recognize_budgeted succeeds with K=4");
    if (!first_outcome.ok()) {
        return;
    }
    const mirador::OcrBatchResult& first = first_outcome.value();
    expect_true(first.detected_count == 6 && !first.det_limit_hit && first.det_limit == 6,
                "K<N: 6 candidates, cap 6 reported, not hit");
    expect_true(first.recognized_count == 4 && first.deferred_count == 2 && first.rec_forward_count == 4,
                "K<N: 4 recognized, 2 deferred, 4 forwards");
    expect_true(first.stop_reason == mirador::OcrBatchStopReason::kRecLineLimit &&
                    first.completion == mirador::OcrBatchCompletion::kPartial,
                "K<N: stop kRecLineLimit, completion kPartial");
    bool executed_ok = true;
    for (int32_t index = 0; executed_ok && index < 4; ++index) {
        const mirador::OcrLineRecord& line = first.lines[static_cast<size_t>(index)];
        executed_ok = executed_ok && line.candidate_id == index && line.execution_order == index &&
                      line.state == mirador::OcrCompletionState::kRecognized && line.utf8_text == "ABCD";
    }
    expect_true(executed_ok, "K<N: the reading-order prefix 0..3 is executed in order");
    bool deferred_ok = true;
    for (int32_t index = 4; deferred_ok && index < 6; ++index) {
        const mirador::OcrLineRecord& line = first.lines[static_cast<size_t>(index)];
        const int32_t expect_x = index == 4 ? 180 : 320;
        deferred_ok = deferred_ok && line.candidate_id == index &&
                      line.state == mirador::OcrCompletionState::kDeferred && line.utf8_text.empty() &&
                      !line.rec_confidence.has_value() && line.execution_order == -1 &&
                      bounds_close(line.bounds, static_cast<float>(expect_x - kUnclipOffset),
                                   static_cast<float>(38 - kUnclipOffset),
                                   static_cast<float>(kBlockWidth + 2 * kUnclipOffset),
                                   static_cast<float>(kBlockHeight + 2 * kUnclipOffset), 0.5F);
    }
    expect_true(deferred_ok, "K<N: deferred lines 4..5 keep queryable det geometry with no text");
    expect_true(ocr_batch_result_consistent(first), "K<N: report is consistent");
    bool deterministic = true;
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto outcome = backend.recognize_budgeted(view, batch_request(4), never_cancelled);
        deterministic = deterministic && outcome.ok() && outcome.value() == first;
    }
    expect_true(deterministic, "K<N: three repeated runs are bitwise identical (operator==)");
}

// Check C: caller priority overrides reading order for execution while the
// report stays in reading order.
void check_priority_execution_order(const std::filesystem::path& dir, const mirador::ImageView& view) {
    const mirador::ExecutionContext never_cancelled;
    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(sensitive_budgeted_options(dir, 6));
    expect_true(backend_result.ok(), "priority: budgeted backend loads");
    if (!backend_result.ok()) {
        return;
    }
    auto backend = std::move(backend_result).take_value();
    mirador::OcrBatchRequest request = batch_request(2);
    // ROI around the reading-order LAST line (candidate 5 at x=320, row
    // y=38), padded so only that line intersects it.
    request.priorities.push_back(mirador::OcrPriorityEntry{
        mirador::RectF{320.0F - kUnclipOffset - 5.0F, 38.0F - kUnclipOffset - 5.0F,
                       kBlockWidth + 2 * kUnclipOffset + 10.0F, kBlockHeight + 2 * kUnclipOffset + 10.0F},
        1.0F});
    const auto outcome = backend.recognize_budgeted(view, request, never_cancelled);
    expect_true(outcome.ok(), "priority: recognize_budgeted succeeds with K=2 and a priority ROI");
    if (!outcome.ok()) {
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();
    expect_true(result.recognized_count == 2 && result.deferred_count == 4 && result.rec_forward_count == 2,
                "priority: 2 recognized, 4 deferred, 2 forwards");
    expect_true(result.stop_reason == mirador::OcrBatchStopReason::kRecLineLimit &&
                    result.completion == mirador::OcrBatchCompletion::kPartial,
                "priority: stop kRecLineLimit, completion kPartial");
    bool order_ok = result.lines.size() == 6;
    for (int32_t index = 0; order_ok && index < 6; ++index) {
        order_ok = order_ok && result.lines[static_cast<size_t>(index)].candidate_id == index;
    }
    expect_true(order_ok, "priority: report lines stay in reading order (candidate ids 0..5)");
    expect_true(result.lines[5].execution_order == 0 &&
                    result.lines[5].state == mirador::OcrCompletionState::kRecognized &&
                    result.lines[5].utf8_text == "ABCD" && result.lines[5].rec_confidence.has_value(),
                "priority: the highest-bounds line (candidate 5) executes first and is recognized");
    expect_true(
        result.lines[0].execution_order == 1 && result.lines[0].state == mirador::OcrCompletionState::kRecognized,
        "priority: the reading-order first line executes second");
    bool middle_deferred = true;
    for (int32_t index = 1; middle_deferred && index < 5; ++index) {
        middle_deferred = middle_deferred && result.lines[static_cast<size_t>(index)].execution_order == -1 &&
                          result.lines[static_cast<size_t>(index)].state == mirador::OcrCompletionState::kDeferred;
    }
    expect_true(middle_deferred, "priority: lines 1..4 stay deferred without execution orders");
    expect_true(ocr_batch_result_consistent(result), "priority: report is consistent");
}

// Check D: det_limit_hit truncation. With exactly cap+1 kept boxes the
// max_boxes+1 probe returns cap+1, the run keeps the cap-sized prefix and
// reports the hit; full recognition of the prefix still completes.
void check_det_limit_hit_truncation(const std::filesystem::path& dir) {
    const mirador::ExecutionContext never_cancelled;
    const std::vector<BlockPos> four_blocks = {{40, 15}, {180, 15}, {40, 38}, {180, 38}};
    const std::vector<std::byte> scene4 = make_block_scene(kFrameWidth, kFrameHeight, four_blocks);
    const mirador::ImageView view4 = make_rgb_view(scene4, kFrameWidth, kFrameHeight);
    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(sensitive_budgeted_options(dir, 3));
    expect_true(backend_result.ok(), "det_limit_hit: budgeted backend (max_boxes=3) loads");
    if (!backend_result.ok()) {
        return;
    }
    auto backend = std::move(backend_result).take_value();
    const auto outcome = backend.recognize_budgeted(view4, batch_request(64), never_cancelled);
    expect_true(outcome.ok(), "det_limit_hit: recognize_budgeted succeeds (4 boxes vs cap 3)");
    if (!outcome.ok()) {
        std::printf("  det_limit_hit status: %s\n", outcome.status().message().c_str());
        return;
    }
    const mirador::OcrBatchResult& result = outcome.value();
    expect_true(result.det_limit == 3 && result.det_limit_hit,
                "det_limit_hit: cap 3 reported, hit flag set (probe returned 4)");
    expect_true(result.detected_count == 3, "det_limit_hit: only the cap-sized prefix became candidates");
    expect_true(result.recognized_count == 3 && result.rec_forward_count == 3 && result.deferred_count == 0,
                "det_limit_hit: the 3 kept candidates are all recognized");
    expect_true(result.stop_reason == mirador::OcrBatchStopReason::kCompleted &&
                    result.completion == mirador::OcrBatchCompletion::kComplete,
                "det_limit_hit: completion is kComplete although coverage is truncated (check the hit flag)");
    expect_true(ocr_batch_result_consistent(result), "det_limit_hit: report is consistent");
}

// Check E: probe saturation boundary. More kept boxes than the probe cap
// (cap+1) cannot truncate — the frozen postprocess cap check fires for any
// component once the cap is reached, so the whole call fails explicitly.
void check_probe_saturation_boundary(const std::filesystem::path& dir, const mirador::ImageView& view) {
    const mirador::ExecutionContext never_cancelled;
    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(sensitive_budgeted_options(dir, 3));
    expect_true(backend_result.ok(), "probe saturation: budgeted backend (max_boxes=3) loads");
    if (!backend_result.ok()) {
        return;
    }
    auto backend = std::move(backend_result).take_value();
    const auto outcome = backend.recognize_budgeted(view, batch_request(64), never_cancelled);
    expect_code(outcome.status(), mirador::ErrorCode::kBudgetExceeded,
                "probe saturation: 6 kept boxes vs cap 3 fails the whole call with kBudgetExceeded");
}

// Multi-candidate scenarios dispatcher: reading order over 6 lines, K<N
// deferral with bitwise-stable repeats, caller-priority execution order,
// det_limit_hit truncation (4-block scene: kept == cap+1) and the
// probe-saturation boundary.
void check_multi_candidate_scenarios(const std::filesystem::path& dir) {
    const std::vector<BlockPos> six_blocks = {{40, 15}, {180, 15}, {320, 15}, {40, 38}, {180, 38}, {320, 38}};
    const std::vector<std::byte> scene = make_block_scene(kFrameWidth, kFrameHeight, six_blocks);
    const mirador::ImageView view = make_rgb_view(scene, kFrameWidth, kFrameHeight);
    if (check_sensitive_calibration(dir, view) == 0) {
        return;
    }
    check_multi_line_happy(dir, view);
    check_k_less_than_n(dir, view);
    check_priority_execution_order(dir, view);
    check_det_limit_hit_truncation(dir);
    check_probe_saturation_boundary(dir, view);
}

// Check 6: cancellation before det fails the whole call with kCancelled and
// without any forward.
void check_cancellation(mirador::integrations::PpOcrBudgetedBackend& backend,
                        const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    const mirador::integrations::PpOcrStageTimings snapshot = backend.last_stage_timings();
    mirador::ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };

    const auto outcome = backend.recognize_budgeted(view, batch_request(4), cancelled);
    expect_code(outcome.status(), mirador::ErrorCode::kCancelled,
                "always-cancelled context: whole call fails with kCancelled");
    expect_true(timings_equal(backend.last_stage_timings(), snapshot),
                "cancelled before det: no det or rec forward ran (timings untouched)");
}

// Check 7: timings reset per call — a det-only call carries no rec phase from
// the previous call, and a second full call re-accumulates every phase.
void check_timings_reset(mirador::integrations::PpOcrBudgetedBackend& backend,
                         const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    const mirador::ExecutionContext never_cancelled;

    {
        mirador::OcrBatchRequest det_only = batch_request(1);
        det_only.min_confidence = 0.95F;  // zero candidates -> no crop, no rec unit
        const auto outcome = backend.recognize_budgeted(view, det_only, never_cancelled);
        expect_true(outcome.ok(), "timings reset: det-only call succeeds");
        if (outcome.ok()) {
            const mirador::integrations::PpOcrStageTimings timings = backend.last_stage_timings();
            expect_true(timings.det_forward_ns > 0 && timings.select_ns > 0 && timings.finalize_ns > 0,
                        "timings reset: det-side phases still accounted on the det-only call");
            expect_true(timings.rec_forward_ns == 0 && timings.rec_preprocess_ns == 0 && timings.rec_decode_ns == 0 &&
                            timings.crop_ns == 0,
                        "timings reset: no rec/crop accumulation leaks from the previous call");
        }
    }

    {
        const auto outcome = backend.recognize_budgeted(view, batch_request(1), never_cancelled);
        expect_true(outcome.ok(), "timings reset: second full call succeeds");
        if (outcome.ok()) {
            const mirador::integrations::PpOcrStageTimings timings = backend.last_stage_timings();
            expect_true(timings.det_forward_ns > 0 && timings.rec_forward_ns > 0 && timings.select_ns > 0 &&
                            timings.finalize_ns > 0,
                        "timings reset: second call re-accumulates all stage timings");
        }
    }
}

// Check 8: request validation failures and the format gate, each before any
// stage work.
void check_request_validation(mirador::integrations::PpOcrBudgetedBackend& backend,
                              const std::vector<std::byte>& frame_bytes) {
    const mirador::ImageView view = make_rgb_view(frame_bytes, kFrameWidth, kFrameHeight);
    const mirador::ExecutionContext never_cancelled;
    const mirador::integrations::PpOcrStageTimings snapshot = backend.last_stage_timings();

    {
        const auto outcome = backend.recognize_budgeted(view, batch_request(0), never_cancelled);
        expect_code(outcome.status(), mirador::ErrorCode::kInvalidArgument, "request K=0 -> kInvalidArgument");
    }
    {
        mirador::OcrBatchRequest request = batch_request(1);
        request.priorities.assign(65, mirador::OcrPriorityEntry{});  // cap is 64 entries
        const auto outcome = backend.recognize_budgeted(view, request, never_cancelled);
        expect_code(outcome.status(), mirador::ErrorCode::kBudgetExceeded,
                    "65 priority entries (cap 64) -> kBudgetExceeded");
    }
    expect_true(timings_equal(backend.last_stage_timings(), snapshot),
                "validation failures run no stage work (timings untouched)");

    std::vector<std::byte> gray_bytes(64, std::byte{0});
    mirador::ImageView gray_view;
    gray_view.data = gray_bytes.data();
    gray_view.width = 8;
    gray_view.height = 8;
    gray_view.row_stride_bytes = 8;
    gray_view.format = mirador::PixelFormat::kGray8;
    const auto wrong_format = backend.recognize_budgeted(gray_view, batch_request(1), never_cancelled);
    expect_code(wrong_format.status(), mirador::ErrorCode::kUnsupportedFormat, "kGray8 input -> kUnsupportedFormat");
}

bool run_all_checks(const std::filesystem::path& dir) {
    if (!write_det_model(dir) || !write_rec_model(dir) || !write_text_file(dir / "charset.txt", "A\nB\nC\nD\n")) {
        std::printf("FAIL: writing synthetic fixtures to %s\n", dir.string().c_str());
        ++g_failed_checks;
        return false;
    }
    std::printf("ok: synthetic det/rec models and charset written to %s\n", dir.string().c_str());

    check_create_validation(dir);

    auto backend_result = mirador::integrations::PpOcrBudgetedBackend::create(budgeted_options(dir));
    if (!backend_result.ok()) {
        std::printf("FAIL: budgeted backend creation failed: %s\n", backend_result.status().message().c_str());
        ++g_failed_checks;
        return false;
    }
    auto backend = std::move(backend_result).take_value();

    // One shared deterministic frame: the constant det map yields exactly one
    // full-frame candidate, so every scenario below sees N=1.
    const std::vector<std::byte> frame_bytes = make_rgb_bytes(kFrameWidth, kFrameHeight);

    check_happy_path(backend, frame_bytes);
    check_rec_scores_not_gated(backend, frame_bytes);
    check_k_boundaries(backend, frame_bytes);
    check_det_limit_report(dir, frame_bytes);
    check_soft_budget(backend, frame_bytes);
    check_deadline_guards(backend, frame_bytes);
    check_finalize_guard_soft_stop(dir, frame_bytes);
    check_cancellation(backend, frame_bytes);
    check_timings_reset(backend, frame_bytes);
    check_request_validation(backend, frame_bytes);
    check_multi_candidate_scenarios(dir);
    return g_failed_checks == 0;
}

}  // namespace

int main() {
    const std::filesystem::path dir = make_temp_dir();
    if (dir.empty()) {
        std::printf("FAIL: could not create a unique temp directory\n");
        return 1;
    }

    const bool all_passed = run_all_checks(dir);

    std::error_code cleanup_ec;
    static_cast<void>(std::filesystem::remove_all(dir, cleanup_ec));
    if (cleanup_ec) {
        std::printf("warning: temp cleanup of %s failed: %s\n", dir.string().c_str(), cleanup_ec.message().c_str());
    }

    if (!all_passed) {
        std::printf("ppocr budgeted smoke: FAIL (%d failed check(s))\n", g_failed_checks);
        return 1;
    }
    std::printf(
        "ppocr budgeted smoke: PASS (create validation, happy path, K boundaries, soft budget, "
        "deadline/cancel guards, finalize guard soft stop, timings reset, request validation, "
        "det_limit report, multi-candidate reading order/K<N/priority/det_limit_hit)\n");
    return 0;
}
