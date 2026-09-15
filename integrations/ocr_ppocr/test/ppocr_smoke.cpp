// M5-03 (DEC-015): self-contained synthetic-model smoke test for the PP-OCR
// reference backends. Generates a constant-probability det model (1x1 kernel,
// bias 0.9) and a constant-logit rec model (full-frame kernel, bias per time
// step) in a unique temp directory, then checks the greedy CTC decoder, the
// det/rec/combined recognition paths, the documented factory error codes and
// temp cleanup. Weight constants are test fixture data, never real model
// weights; no network access, nothing persisted outside the temp dir.
//
// Model param adaptation for the pinned ncnn (commit e54f7b1): the
// Convolution param keys differ from the classic PP-OCR convention there —
// key 8 is int8_scale_term (not kernel dilation), key 11 is kernel_h (not
// pad), 12/13 are dilation_h/stride_h — so the conv lines below omit keys
// 7..18 and rely on the defaults (kernel_h = kernel_w, stride/dilation 1,
// pads 0); the rec conv passes kernel_h explicitly via key 11. The ncnn .bin
// container gives each weight blob a 4-byte flag word (0 = raw float32) while
// conv bias blobs are raw float32 without a flag word.

#include "ctc_decode.hpp"
#include "ppocr_backend.hpp"

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <array>
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
        std::filesystem::path candidate = base / ("mirador_ppocr_smoke_" + std::to_string(attempt));
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
// pixel values only exercise the letterbox/normalize path.
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

// Check 1: greedy CTC decode pure logic.
void check_ctc_decode() {
    // Collapse repeats, skip blank, keep time order; distinct per-step
    // distributions verify the confidence is the mean over emitted steps.
    constexpr int32_t k_steps = 5;
    constexpr int32_t k_classes = 3;
    constexpr int32_t k_blank = 0;
    const std::array<std::array<float, 3>, 5> scores = {{
        {0.0F, 3.0F, 0.0F},  // t0: best 1 (emitted)
        {0.0F, 2.0F, 0.0F},  // t1: best 1 (collapsed repeat)
        {0.0F, 0.0F, 1.0F},  // t2: best 2 (emitted)
        {2.0F, 0.0F, 0.0F},  // t3: best 0 = blank (skipped)
        {0.0F, 0.0F, 2.0F},  // t4: best 2 (emitted)
    }};
    const auto step_probability = [](const std::array<float, 3>& row) {
        const auto max_value = static_cast<double>(*std::max_element(row.begin(), row.end()));
        double sum = 0.0;
        for (const float value : row) {
            sum += std::exp(static_cast<double>(value) - max_value);
        }
        return 1.0 / sum;
    };

    const auto decoded = mirador::integrations::ctc_decode_greedy(scores.front().data(), k_steps, k_classes, k_blank);
    expect_true(decoded.ok(), "ctc_decode_greedy decodes a valid score matrix");
    if (decoded.ok()) {
        // CTC semantics: consecutive repeats collapse ([1,1] -> 1) and blank
        // acts as a separator, so [1,1,2,0,2] is THREE characters [1,2,2] —
        // without the separator the two 2s would be undecodable.
        const std::vector<int32_t> expected_classes = {1, 2, 2};
        expect_true(decoded.value().class_indices == expected_classes,
                    "ctc: [1,1,2,0,2] decodes to [1,2,2] (blank separates)");
        const double expected_confidence =
            (step_probability(scores[0]) + step_probability(scores[2]) + step_probability(scores[4])) / 3.0;
        expect_true(std::fabs(static_cast<double>(decoded.value().confidence) - expected_confidence) <= 1e-6,
                    "ctc: confidence is the mean over the 3 emitted steps");
    }

    // Repeats collapse only when adjacent: no blank between them here.
    const std::array<std::array<float, 3>, 5> adjacent = {{
        {0.0F, 1.0F, 0.0F},  // 1
        {0.0F, 2.0F, 0.0F},  // 1 (collapsed)
        {0.0F, 0.0F, 3.0F},  // 2
        {0.0F, 0.0F, 4.0F},  // 2 (collapsed)
        {0.0F, 1.5F, 0.0F},  // 1
    }};
    const auto collapsed = mirador::integrations::ctc_decode_greedy(adjacent.front().data(), 5, 3, 0);
    expect_true(collapsed.ok() && collapsed.value().class_indices == std::vector<int32_t>{1, 2, 1},
                "ctc: adjacent repeats collapse ([1,1,2,2,1] -> [1,2,1])");

    // A blank between two identical classes yields two distinct characters.
    const std::array<std::array<float, 3>, 3> separated = {{
        {0.0F, 0.0F, 2.0F},  // 2
        {2.0F, 0.0F, 0.0F},  // blank
        {0.0F, 0.0F, 2.0F},  // 2 (blank separated, emitted again)
    }};
    const auto separated_decode = mirador::integrations::ctc_decode_greedy(separated.front().data(), 3, 3, 0);
    expect_true(separated_decode.ok() && separated_decode.value().class_indices == std::vector<int32_t>{2, 2},
                "ctc: [2,0,2] decodes to [2,2] (blank separates identical classes)");

    // All-blank input decodes to nothing with zero confidence.
    const std::array<std::array<float, 3>, 4> blank_rows = {{
        {2.0F, 0.0F, 0.0F},
        {2.0F, 0.0F, 0.0F},
        {2.0F, 0.0F, 0.0F},
        {2.0F, 0.0F, 0.0F},
    }};
    const auto all_blank = mirador::integrations::ctc_decode_greedy(blank_rows.front().data(), 4, 3, 0);
    expect_true(all_blank.ok() && all_blank.value().class_indices.empty() && all_blank.value().confidence == 0.0F,
                "ctc: all-blank input decodes to empty with confidence 0");

    // Ties take the first maximum: uniform row -> class 0 (blank here is 2),
    // symmetric row {0,1,1} -> class 1, not class 2.
    const std::array<float, 3> uniform_row = {0.5F, 0.5F, 0.5F};
    const auto tie_first = mirador::integrations::ctc_decode_greedy(uniform_row.data(), 1, 3, 2);
    expect_true(tie_first.ok() && tie_first.value().class_indices == std::vector<int32_t>{0},
                "ctc: uniform tie resolves to the first class");
    const std::array<float, 3> tie_row = {0.0F, 1.0F, 1.0F};
    const auto tie_second = mirador::integrations::ctc_decode_greedy(tie_row.data(), 1, 3, 2);
    expect_true(tie_second.ok() && tie_second.value().class_indices == std::vector<int32_t>{1},
                "ctc: two-way tie resolves to the lower class index");

    // Documented error paths.
    const auto zero_steps = mirador::integrations::ctc_decode_greedy(scores.front().data(), 0, 3, 0);
    expect_code(zero_steps.status(), mirador::ErrorCode::kInvalidArgument, "ctc: steps=0 -> kInvalidArgument");
    const auto one_class = mirador::integrations::ctc_decode_greedy(scores.front().data(), 1, 1, 0);
    expect_code(one_class.status(), mirador::ErrorCode::kInvalidArgument, "ctc: classes=1 -> kInvalidArgument");
    const auto blank_negative = mirador::integrations::ctc_decode_greedy(scores.front().data(), 1, 3, -1);
    expect_code(blank_negative.status(), mirador::ErrorCode::kInvalidArgument, "ctc: blank=-1 -> kInvalidArgument");
    const auto blank_out_of_range = mirador::integrations::ctc_decode_greedy(scores.front().data(), 1, 3, 3);
    expect_code(blank_out_of_range.status(), mirador::ErrorCode::kInvalidArgument,
                "ctc: blank==classes -> kInvalidArgument");
    const auto null_data = mirador::integrations::ctc_decode_greedy(nullptr, 1, 3, 0);
    expect_code(null_data.status(), mirador::ErrorCode::kInvalidArgument, "ctc: null data -> kInvalidArgument");
}

mirador::integrations::PpOcrDetOptions det_options(const std::filesystem::path& dir) {
    mirador::integrations::PpOcrDetOptions options;
    options.param_path = (dir / "det.param").string();
    options.bin_path = (dir / "det.bin").string();
    options.identity = mirador::integrations::PpOcrModelIdentity{"bench-det", "rev-1"};
    options.det_side = kDetSide;
    return options;
}

mirador::integrations::PpOcrRecOptions rec_options(const std::filesystem::path& dir) {
    mirador::integrations::PpOcrRecOptions options;
    options.param_path = (dir / "rec.param").string();
    options.bin_path = (dir / "rec.bin").string();
    options.charset_path = (dir / "charset.txt").string();
    options.identity = mirador::integrations::PpOcrModelIdentity{"bench-rec", "rev-1"};
    options.rec_height = kRecHeight;
    options.rec_width = kRecWidth;
    return options;
}

// Check 2: det backend on a non-square RGB8 frame (letterbox + inference +
// constant 0.9 probability map + DB postprocess + inverse letterbox).
void check_det_backend(const std::filesystem::path& dir) {
    auto det_result = mirador::integrations::PpOcrDetBackend::create(det_options(dir));
    expect_true(det_result.ok(), "det backend loads the synthetic model");
    if (!det_result.ok()) {
        std::printf("  det create status: %s\n", det_result.status().message().c_str());
        return;
    }
    auto det = std::move(det_result).take_value();
    expect_true(det.info().model_id == "bench-det", "det backend reports the configured model id");

    const std::vector<std::byte> bytes = make_rgb_bytes(200, 150);
    const mirador::ImageView view = make_rgb_view(bytes, 200, 150);
    const mirador::OcrRequest default_request;
    const mirador::ExecutionContext never_cancelled;

    const auto regions = det.recognize(view, default_request, never_cancelled);
    expect_true(regions.ok(), "det recognize succeeds on a 200x150 RGB8 frame");
    if (!regions.ok()) {
        std::printf("  det recognize status: %s\n", regions.status().message().c_str());
        return;
    }
    expect_true(regions.value().size() == 1, "det finds exactly one region for a constant 0.9 map");
    if (regions.value().size() == 1) {
        const mirador::TextRegion& region = regions.value().front();
        expect_true(bounds_close(region.bounds, 0.0F, 0.0F, 200.0F, 150.0F, 2.0F),
                    "det bounds recover the full 200x150 frame (tolerance 2 px)");
        expect_true(region.confidence >= 0.8F && region.confidence <= 1.0F, "det confidence is the 0.9 map mean score");
        expect_true(region.utf8_text.empty(), "det region carries no text (detection only)");
    }

    mirador::OcrRequest strict_request;
    strict_request.min_confidence = 0.95F;
    const auto filtered = det.recognize(view, strict_request, never_cancelled);
    expect_true(filtered.ok() && filtered.value().empty(), "det drops the ~0.9 box when min_confidence=0.95");
}

// Check 3: rec backend on a 100x25 line image; the model emits classes 1..4
// which the 4-line charset maps to "ABCD".
void check_rec_backend(const std::filesystem::path& dir) {
    auto rec_result = mirador::integrations::PpOcrRecBackend::create(rec_options(dir));
    expect_true(rec_result.ok(), "rec backend loads the synthetic model and charset");
    if (!rec_result.ok()) {
        std::printf("  rec create status: %s\n", rec_result.status().message().c_str());
        return;
    }
    auto rec = std::move(rec_result).take_value();
    expect_true(rec.rec_height() == kRecHeight && rec.rec_width() == kRecWidth,
                "rec backend reports the configured input size");

    const std::vector<std::byte> bytes = make_rgb_bytes(100, 25);
    const mirador::ImageView view = make_rgb_view(bytes, 100, 25);
    const mirador::OcrRequest default_request;
    const mirador::ExecutionContext never_cancelled;

    const auto regions = rec.recognize(view, default_request, never_cancelled);
    expect_true(regions.ok(), "rec recognize succeeds on a 100x25 RGB8 frame");
    if (!regions.ok()) {
        std::printf("  rec recognize status: %s\n", regions.status().message().c_str());
        return;
    }
    expect_true(regions.value().size() == 1, "rec returns exactly one line region");
    if (regions.value().size() == 1) {
        const mirador::TextRegion& region = regions.value().front();
        expect_true(region.utf8_text == "ABCD", "rec decodes classes 1,2,3,4 to \"ABCD\"");
        expect_true(bounds_close(region.bounds, 0.0F, 0.0F, 100.0F, 25.0F, 0.5F),
                    "rec bounds cover the whole line image");
        expect_true(region.confidence >= 0.3F && region.confidence <= 0.7F,
                    "rec confidence is the per-step softmax mean (~e/(e+4))");
    }

    mirador::OcrRequest strict_request;
    strict_request.min_confidence = 0.9F;
    const auto filtered = rec.recognize(view, strict_request, never_cancelled);
    expect_true(filtered.ok() && filtered.value().empty(), "rec returns nothing when min_confidence=0.9");
}

// Check 4: combined det + rec pipeline on the 200x150 frame.
void check_combined_backend(const std::filesystem::path& dir) {
    auto combined_result = mirador::integrations::PpOcrBackend::create(det_options(dir), rec_options(dir));
    expect_true(combined_result.ok(), "combined backend loads both models");
    if (!combined_result.ok()) {
        std::printf("  combined create status: %s\n", combined_result.status().message().c_str());
        return;
    }
    auto combined = std::move(combined_result).take_value();

    const std::vector<std::byte> bytes = make_rgb_bytes(200, 150);
    const mirador::ImageView view = make_rgb_view(bytes, 200, 150);
    const mirador::OcrRequest default_request;
    const mirador::ExecutionContext never_cancelled;

    const auto regions = combined.recognize(view, default_request, never_cancelled);
    expect_true(regions.ok(), "combined recognize succeeds");
    if (!regions.ok()) {
        std::printf("  combined recognize status: %s\n", regions.status().message().c_str());
        return;
    }
    expect_true(regions.value().size() == 1, "combined pipeline returns exactly one region");
    if (regions.value().size() == 1) {
        const mirador::TextRegion& region = regions.value().front();
        expect_true(region.utf8_text == "ABCD", "combined region text is \"ABCD\"");
        // det ~0.902 x rec ~0.405 = ~0.365.
        expect_true(region.confidence >= 0.25F && region.confidence <= 0.6F,
                    "combined confidence is the det score times the rec score");
        expect_true(bounds_close(region.bounds, 0.0F, 0.0F, 200.0F, 150.0F, 2.0F),
                    "combined bounds stay in prepared-image space");
    }
}

// Check 5: documented factory error paths.
void check_factory_errors(const std::filesystem::path& dir) {
    mirador::integrations::PpOcrDetOptions bad_side = det_options(dir);
    bad_side.det_side = 100;  // not a multiple of 32
    const auto bad_side_backend = mirador::integrations::PpOcrDetBackend::create(bad_side);
    expect_code(bad_side_backend.status(), mirador::ErrorCode::kInvalidArgument,
                "det create with det_side=100 -> kInvalidArgument");

    mirador::integrations::PpOcrDetOptions missing_model = det_options(dir);
    missing_model.param_path = (dir / "missing.param").string();
    missing_model.bin_path = (dir / "missing.bin").string();
    const auto missing_backend = mirador::integrations::PpOcrDetBackend::create(missing_model);
    expect_code(missing_backend.status(), mirador::ErrorCode::kBackendUnavailable,
                "det create with missing model files -> kBackendUnavailable");

    mirador::integrations::PpOcrRecOptions missing_charset = rec_options(dir);
    missing_charset.charset_path = (dir / "missing_charset.txt").string();
    const auto missing_charset_backend = mirador::integrations::PpOcrRecBackend::create(missing_charset);
    expect_code(missing_charset_backend.status(), mirador::ErrorCode::kBackendUnavailable,
                "rec create with a missing charset file -> kBackendUnavailable");

    mirador::integrations::PpOcrRecOptions empty_charset = rec_options(dir);
    empty_charset.charset_path = (dir / "charset_empty.txt").string();
    static_cast<void>(write_text_file(empty_charset.charset_path, ""));
    const auto empty_charset_backend = mirador::integrations::PpOcrRecBackend::create(empty_charset);
    expect_code(empty_charset_backend.status(), mirador::ErrorCode::kBackendUnavailable,
                "rec create with an empty charset file -> kBackendUnavailable");
}

bool run_all_checks(const std::filesystem::path& dir) {
    if (!write_det_model(dir) || !write_rec_model(dir) || !write_text_file(dir / "charset.txt", "A\nB\nC\nD\n")) {
        std::printf("FAIL: writing synthetic fixtures to %s\n", dir.string().c_str());
        ++g_failed_checks;
        return false;
    }
    std::printf("ok: synthetic det/rec models and charset written to %s\n", dir.string().c_str());

    check_ctc_decode();
    check_det_backend(dir);
    check_rec_backend(dir);
    check_combined_backend(dir);
    check_factory_errors(dir);
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
        std::printf("ppocr smoke: FAIL (%d failed check(s))\n", g_failed_checks);
        return 1;
    }
    std::printf("ppocr smoke: PASS (ctc decode, det path, rec path, combined pipeline, factory errors)\n");
    return 0;
}
