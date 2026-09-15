// M5-04 (DEC-015): self-contained synthetic-model smoke test for the YOLO
// reference detection backend. Generates a constant-logit model (all-zero
// kernel, per-proposal biases) in a unique temp directory whose 3x8 output
// rows are [cx,cy,w,h,obj,c0,c1,c2] in the frozen YOLOv5 single-tensor
// layout, then verifies decoding, the inverse-letterbox mapping, NMS
// duplicate suppression, both scoring modes, the explicit candidate budget,
// cancellation and the documented factory error paths. Weight constants are
// test fixture data, never real model weights; no network access, nothing
// printed or persisted beyond pass/fail lines.
//
// param adaptation for the pinned ncnn: the Convolution line omits keys 7+
// (key 8 is int8_scale_term, key 11 is kernel_h); kernel_h/stride_h default
// to kernel_w/stride_w, so the 64x64 stride-64 kernel collapses the 64x64x3
// input to 24 channels x 1 x 1, and the Reshape (keys 0=w 1=h 2=c) yields the
// frozen contract shape: channels=1, height=3 proposals, width=8 = 5 + 3.

#include "yolo_backend.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

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

constexpr int32_t k_input_side = 64;
constexpr int32_t k_num_classes = 3;
constexpr int32_t k_row_width = 8;          // 5 + classes
constexpr int32_t k_conv_outputs = 24;      // 3 proposals x 8 values
constexpr int32_t k_weight_count = 294912;  // 24 outputs x 3 channels x 64 x 64
constexpr int32_t k_prepared_side = 128;    // letterbox scale 0.5 -> inverse x2

constexpr std::string_view kParamText = R"(7767517
3 3
Input data 0 1 x 0=64 1=64 2=3
Convolution conv0 1 1 x conv_out 0=24 1=64 2=1 3=64 4=0 5=1 6=294912
Reshape reshape0 1 1 conv_out out 0=8 1=3 2=1
)";

// Fixture proposal rows [cx,cy,w,h,obj,c0,c1,c2]; row 2 duplicates row 0 so
// NMS must suppress it (IoU 1) while rows 0 and 1 overlap only marginally.
constexpr std::array<std::array<float, 8>, 3> kProposals = {{
    {32.F, 32.F, 32.F, 32.F, 0.9F, 0.8F, 0.1F, 0.05F},
    {48.F, 16.F, 16.F, 16.F, 0.6F, 0.1F, 0.5F, 0.2F},
    {32.F, 32.F, 32.F, 32.F, 0.9F, 0.8F, 0.1F, 0.05F},
}};

bool write_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    stream.close();
    return stream.good();
}

bool write_model(const std::filesystem::path& dir) {
    const bool param_ok = write_file(dir / "model.param", kParamText.data(), kParamText.size());
    // ncnn .bin container: flag word (0 = raw float32) + 294912 all-zero
    // kernel weights + 24 raw bias floats (no flag on the bias blob).
    constexpr size_t k_bias_offset = 4U + static_cast<size_t>(k_weight_count) * 4U;
    std::vector<std::byte> bin(k_bias_offset + static_cast<size_t>(k_conv_outputs) * 4U, std::byte{0});
    for (int32_t r = 0; r < 3; ++r) {
        for (int32_t j = 0; j < k_row_width; ++j) {
            const size_t offset = k_bias_offset + (static_cast<size_t>(r) * 8U + static_cast<size_t>(j)) * 4U;
            uint32_t bits = 0;
            const float value = kProposals[static_cast<size_t>(r)][static_cast<size_t>(j)];
            static_cast<void>(std::memcpy(&bits, &value, sizeof(bits)));
            bin[offset + 0U] = static_cast<std::byte>(bits & 0xFFU);
            bin[offset + 1U] = static_cast<std::byte>((bits >> 8U) & 0xFFU);
            bin[offset + 2U] = static_cast<std::byte>((bits >> 16U) & 0xFFU);
            bin[offset + 3U] = static_cast<std::byte>((bits >> 24U) & 0xFFU);
        }
    }
    const bool bin_ok = write_file(dir / "model.bin", bin.data(), bin.size());
    return param_ok && bin_ok;
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
        std::filesystem::path candidate = base / ("mirador_yolo_smoke_" + std::to_string(attempt));
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

// Deterministic non-trivial scene; the fixture model is bias-driven, so the
// pixels only exercise the letterbox/normalize path.
std::vector<std::byte> make_rgb_bytes(int32_t width, int32_t height) {
    std::vector<std::byte> bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const auto value = static_cast<uint8_t>((x * 3 + y * 11) % 253);
            const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3U;
            bytes[offset + 0U] = static_cast<std::byte>(value);
            bytes[offset + 1U] = static_cast<std::byte>(value);
            bytes[offset + 2U] = static_cast<std::byte>(value);
        }
    }
    return bytes;
}

mirador::integrations::YoloDetectorOptions base_options(const std::filesystem::path& dir) {
    mirador::integrations::YoloDetectorOptions options;
    options.param_path = (dir / "model.param").string();
    options.bin_path = (dir / "model.bin").string();
    options.identity = mirador::integrations::YoloModelIdentity{"bench-yolo", "rev-1"};
    options.num_classes = k_num_classes;
    options.input_side = k_input_side;
    options.use_objectness = true;
    options.class_names = {"person", "car", "dog"};
    return options;
}

bool bounds_close(const mirador::RectF& bounds, float x, float y, float width, float height, float tolerance) {
    return std::fabs(bounds.x - x) <= tolerance && std::fabs(bounds.y - y) <= tolerance &&
           std::fabs(bounds.width - width) <= tolerance && std::fabs(bounds.height - height) <= tolerance;
}

bool confidence_close(float confidence, float expected) {
    return std::fabs(confidence - expected) <= 0.01F;
}

// Check 1: default backend, min_confidence 0.25 — NMS suppresses the
// duplicated row and the two survivors come back score-descending.
void check_detect_default(mirador::integrations::YoloDetectorBackend& detector, const std::vector<std::byte>& bytes) {
    const mirador::ImageView view = make_rgb_view(bytes, k_prepared_side, k_prepared_side);
    mirador::DetectionRequest request;
    request.min_confidence = 0.25F;
    const mirador::ExecutionContext never_cancelled;

    const auto detections = detector.detect(view, request, never_cancelled);
    expect_true(detections.ok(), "detect succeeds with min_confidence 0.25");
    if (!detections.ok()) {
        std::printf("  detect status: %s\n", detections.status().message().c_str());
        return;
    }
    expect_true(detections.value().size() == 2, "exactly 2 regions survive (duplicate row suppressed)");
    if (detections.value().size() != 2) {
        return;
    }
    const mirador::DetectionRegion& first = detections.value()[0];
    const mirador::DetectionRegion& second = detections.value()[1];
    expect_true(bounds_close(first.bounds, 32.0F, 32.0F, 64.0F, 64.0F, 1.0F),
                "region0 bounds are (32,32,64,64) after the x2 unletterbox");
    expect_true(first.class_id == 0 && first.label == "person", "region0 is class 0 \"person\"");
    expect_true(confidence_close(first.confidence, 0.72F), "region0 confidence is 0.9*0.8 = 0.72");
    expect_true(bounds_close(second.bounds, 80.0F, 16.0F, 32.0F, 32.0F, 1.0F),
                "region1 bounds are (80,16,32,32) after the x2 unletterbox");
    expect_true(second.class_id == 1 && second.label == "car", "region1 is class 1 \"car\"");
    expect_true(confidence_close(second.confidence, 0.30F), "region1 confidence is 0.6*0.5 = 0.30");
    expect_true(first.confidence >= second.confidence, "regions are ordered score-descending");
}

// Check 2: min_confidence 0.5 keeps only the strong duplicate pair.
void check_min_confidence_filter(mirador::integrations::YoloDetectorBackend& detector,
                                 const std::vector<std::byte>& bytes) {
    const mirador::ImageView view = make_rgb_view(bytes, k_prepared_side, k_prepared_side);
    mirador::DetectionRequest request;
    request.min_confidence = 0.5F;
    const mirador::ExecutionContext never_cancelled;

    const auto detections = detector.detect(view, request, never_cancelled);
    expect_true(detections.ok() && detections.value().size() == 1, "min_confidence 0.5 keeps exactly 1 region");
    if (detections.ok() && detections.value().size() == 1) {
        const mirador::DetectionRegion& only = detections.value().front();
        expect_true(only.class_id == 0 && only.label == "person" && confidence_close(only.confidence, 0.72F),
                    "the surviving region is the 0.72 \"person\"");
    }
}

// Check 3: merged-confidence scoring (use_objectness=false).
void check_merged_scoring(const std::filesystem::path& dir, const std::vector<std::byte>& bytes) {
    auto options = base_options(dir);
    options.use_objectness = false;
    auto result = mirador::integrations::YoloDetectorBackend::create(options);
    expect_true(result.ok(), "second backend with use_objectness=false loads");
    if (!result.ok()) {
        return;
    }
    auto detector = std::move(result).take_value();
    const mirador::ImageView view = make_rgb_view(bytes, k_prepared_side, k_prepared_side);
    mirador::DetectionRequest request;
    request.min_confidence = 0.25F;
    const mirador::ExecutionContext never_cancelled;

    const auto detections = detector.detect(view, request, never_cancelled);
    expect_true(detections.ok() && detections.value().size() == 2, "merged scoring keeps 2 regions");
    if (detections.ok() && detections.value().size() == 2) {
        expect_true(confidence_close(detections.value()[0].confidence, 0.8F) &&
                        confidence_close(detections.value()[1].confidence, 0.5F),
                    "merged scores are the raw class maxima 0.8 / 0.5");
    }
}

// Checks 4-5: explicit candidate budget, cancellation, factory error paths.
void check_budget_cancel_and_errors(const std::filesystem::path& dir, const std::vector<std::byte>& bytes) {
    const mirador::ImageView view = make_rgb_view(bytes, k_prepared_side, k_prepared_side);
    const mirador::ExecutionContext never_cancelled;

    auto budget_options = base_options(dir);
    budget_options.max_candidates = 1;
    auto budget_result = mirador::integrations::YoloDetectorBackend::create(budget_options);
    expect_true(budget_result.ok(), "budget backend loads");
    if (budget_result.ok()) {
        auto budget_detector = std::move(budget_result).take_value();
        mirador::DetectionRequest request;
        request.min_confidence = 0.25F;
        const auto budgeted = budget_detector.detect(view, request, never_cancelled);
        expect_code(budgeted.status(), mirador::ErrorCode::kBudgetExceeded,
                    "more survivors than max_candidates -> kBudgetExceeded");
    }

    auto default_result = mirador::integrations::YoloDetectorBackend::create(base_options(dir));
    expect_true(default_result.ok(), "default backend loads for the cancellation check");
    if (default_result.ok()) {
        auto detector = std::move(default_result).take_value();
        mirador::ExecutionContext cancelled;
        cancelled.is_cancelled = [] { return true; };
        mirador::DetectionRequest request;
        request.min_confidence = 0.25F;
        const auto cancelled_run = detector.detect(view, request, cancelled);
        expect_code(cancelled_run.status(), mirador::ErrorCode::kCancelled,
                    "detect with a cancelled context -> kCancelled");
    }

    auto bad_classes = base_options(dir);
    bad_classes.num_classes = 0;
    const auto bad_classes_backend = mirador::integrations::YoloDetectorBackend::create(bad_classes);
    expect_code(bad_classes_backend.status(), mirador::ErrorCode::kInvalidArgument,
                "num_classes=0 -> kInvalidArgument");

    auto bad_side = base_options(dir);
    bad_side.input_side = 100;  // not a multiple of 32
    const auto bad_side_backend = mirador::integrations::YoloDetectorBackend::create(bad_side);
    expect_code(bad_side_backend.status(), mirador::ErrorCode::kInvalidArgument, "input_side=100 -> kInvalidArgument");

    auto missing_model = base_options(dir);
    missing_model.param_path = (dir / "missing.param").string();
    missing_model.bin_path = (dir / "missing.bin").string();
    const auto missing_backend = mirador::integrations::YoloDetectorBackend::create(missing_model);
    expect_code(missing_backend.status(), mirador::ErrorCode::kBackendUnavailable,
                "missing model files -> kBackendUnavailable");
}

// Check 6: backend identity contract.
void check_info(const std::filesystem::path& dir) {
    auto result = mirador::integrations::YoloDetectorBackend::create(base_options(dir));
    expect_true(result.ok(), "backend loads for the info check");
    if (!result.ok()) {
        return;
    }
    auto detector = std::move(result).take_value();
    const mirador::BackendInfo info = detector.info();
    expect_true(info.name == "yolo-ncnn-reference", "info name is \"yolo-ncnn-reference\"");
    expect_true(info.model_id == "bench-yolo" && info.model_revision == "rev-1",
                "info round-trips model_id and model_revision");
    expect_true(info.accepted_formats.size() == 1 && info.accepted_formats[0] == mirador::PixelFormat::kRgb8,
                "accepted_formats is exactly {kRgb8}");
}

bool run_all_checks(const std::filesystem::path& dir) {
    if (!write_model(dir)) {
        std::printf("FAIL: writing the synthetic model to %s\n", dir.string().c_str());
        ++g_failed_checks;
        return false;
    }
    std::printf("ok: synthetic YOLO model written to %s\n", dir.string().c_str());

    auto result = mirador::integrations::YoloDetectorBackend::create(base_options(dir));
    expect_true(result.ok(), "default backend loads the synthetic model");
    if (!result.ok()) {
        std::printf("  create status: %s\n", result.status().message().c_str());
        return false;
    }
    auto detector = std::move(result).take_value();

    const std::vector<std::byte> bytes = make_rgb_bytes(k_prepared_side, k_prepared_side);
    check_detect_default(detector, bytes);
    check_min_confidence_filter(detector, bytes);
    check_merged_scoring(dir, bytes);
    check_budget_cancel_and_errors(dir, bytes);
    check_info(dir);
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
        std::printf("yolo smoke: FAIL (%d failed check(s))\n", g_failed_checks);
        return 1;
    }
    std::printf("yolo smoke: PASS (decode, unletterbox, NMS, scoring modes, budget, factory errors)\n");
    return 0;
}
