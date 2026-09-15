// M5-02 (DEC-015): self-contained smoke test for the ncnn runtime wrapper.
// Generates a tiny synthetic model (weight constants are test fixture data,
// never real model weights) in a unique temp directory, then checks forward
// numerics against an independent naive convolution, determinism, the packed
// image contract and the documented error paths. Exit code 0 = all checks
// passed; any failed check prints a FAIL line and the process returns 1.

#include "ncnn_runtime.hpp"

#include <mirador/execution_context.hpp>
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

// Synthetic 4x4 1-input 1-output valid convolution model, exactly as frozen in
// the M5-02 test plan: Input(data 4x4x1) -> Convolution(1 out channel, 3x3,
// stride 1, pad 0, bias). ncnn params: 0=num_output 1=kernel_w 2=dilation_w
// 3=stride_w 4=pad_left 5=bias_term 6=weight_data_size.
constexpr std::string_view kParamText = R"(7767517
2 2
Input data 0 1 data 0=4 1=4 2=1
Convolution conv0 1 1 data out 0=1 1=3 2=1 3=1 4=0 5=1 6=9
)";

// Fixture kernel weights [out_ch=1][kh=3][kw=3] in row-major order, values
// 0.25..2.25 so every tap has a distinct non-trivial contribution.
constexpr std::array<float, 9> kKernel = {0.25F, 0.5F, 0.75F, 1.0F, 1.25F, 1.5F, 1.75F, 2.0F, 2.25F};
constexpr float kBias = 0.5F;

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

bool write_model_files(const std::filesystem::path& dir) {
    const bool param_ok = write_file(dir / "model.param", kParamText.data(), kParamText.size());
    std::vector<std::byte> bin;
    bin.reserve((kKernel.size() + 1U) * 4U + 4U);
    // ncnn .bin container format for Convolution (layer/convolution.cpp in the
    // pinned runtime): the weight blob is read with type 0, i.e. prefixed by a
    // 4-byte little-endian flag word (0 selects raw float32); the bias blob is
    // read with type 1, i.e. raw float32 without a flag word.
    append_uint32_le(bin, 0U);  // flag: kernel weights are plain float32
    for (const float weight : kKernel) {
        append_float32_le(bin, weight);
    }
    append_float32_le(bin, kBias);  // bias: raw float32, no flag word
    const bool bin_ok = write_file(dir / "model.bin", bin.data(), bin.size());
    if (!param_ok || !bin_ok) {
        std::printf("FAIL: writing synthetic model files to %s\n", dir.string().c_str());
        ++g_failed_checks;
        return false;
    }
    std::printf("ok: synthetic model written to %s\n", dir.string().c_str());
    return true;
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
        std::filesystem::path candidate = base / ("mirador_ncnn_smoke_" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            return {};
        }
    }
    return {};
}

// Distinct small luminance values (1..16) keep the float convolution sums far
// below any ordering sensitivity while still varying per pixel.
uint8_t gray_pixel(int32_t x, int32_t y) {
    return static_cast<uint8_t>(x + 4 * y + 1);
}

// Gray8 staging buffer: 4 rows x 8-byte stride (4 pixel bytes + 4 padding).
constexpr size_t k_gray_buffer_size = 32U;

uint8_t rgb_pixel(int32_t x, int32_t y, int channel) {
    return static_cast<uint8_t>(x + 4 * y + 3 * channel + 1);
}

// Independent naive valid-convolution reference using the same fixture
// constants; expected output is 2x2x1 in planar CHW order.
std::array<float, 4> naive_convolution(const std::array<std::array<uint8_t, 4>, 4>& image) {
    std::array<float, 4> expected = {kBias, kBias, kBias, kBias};
    for (size_t out_y = 0; out_y < 2; ++out_y) {
        for (size_t out_x = 0; out_x < 2; ++out_x) {
            float sum = kBias;
            for (size_t ky = 0; ky < 3; ++ky) {
                for (size_t kx = 0; kx < 3; ++kx) {
                    sum += kKernel[ky * 3U + kx] * static_cast<float>(image[out_y + ky][out_x + kx]);
                }
            }
            expected[out_y * 2U + out_x] = sum;
        }
    }
    return expected;
}

mirador::integrations::NcnnTensor packed_gray_view(const mirador::ImageView& view) {
    const mirador::ExecutionContext never_cancelled;
    const auto packed = mirador::integrations::pack_image(view, never_cancelled);
    expect_true(packed.ok(), "pack_image succeeds for a Gray8 view");
    return packed.ok() ? packed.value() : mirador::integrations::NcnnTensor{};
}

bool expect_packed_gray_shape(const mirador::integrations::NcnnTensor& tensor,
                              const std::array<std::array<uint8_t, 4>, 4>& pixels) {
    const bool shape_ok = tensor.width == 4 && tensor.height == 4 && tensor.channels == 1 && tensor.data.size() == 16U;
    expect_true(shape_ok, "packed Gray8 tensor shape is 4x4x1");
    if (!shape_ok) {
        return false;
    }
    bool values_ok = true;
    for (int32_t y = 0; y < 4; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            const size_t index = static_cast<size_t>(y) * 4U + static_cast<size_t>(x);
            values_ok = values_ok && tensor.data[index] ==
                                         static_cast<float>(pixels[static_cast<size_t>(y)][static_cast<size_t>(x)]);
        }
    }
    expect_true(values_ok, "packed Gray8 values honor row_stride_bytes 8");
    return values_ok;
}

// Check 1: forward numerics. The Gray8 view deliberately uses row stride 8 for
// a width of 4 so ignoring the stride would read padding bytes and fail.
void check_forward_numerics(const std::filesystem::path& dir) {
    constexpr int32_t k_width = 4;
    constexpr int32_t k_height = 4;
    constexpr int64_t k_stride = 8;  // 4 usable bytes + 4 padding bytes per row

    std::vector<std::byte> buffer(static_cast<size_t>(k_stride) * static_cast<size_t>(k_height));
    std::array<std::array<uint8_t, 4>, 4> pixels = {};
    for (int32_t y = 0; y < k_height; ++y) {
        for (int32_t x = 0; x < k_width; ++x) {
            const auto value = gray_pixel(x, y);
            pixels[static_cast<size_t>(y)][static_cast<size_t>(x)] = value;
            buffer[static_cast<size_t>(y) * static_cast<size_t>(k_stride) + static_cast<size_t>(x)] =
                static_cast<std::byte>(value);
        }
        // Poison the stride padding so an off-by-stride read corrupts results.
        for (int32_t x = k_width; x < k_stride; ++x) {
            buffer[static_cast<size_t>(y) * static_cast<size_t>(k_stride) + static_cast<size_t>(x)] =
                static_cast<std::byte>(0xAB);
        }
    }

    mirador::ImageView view;
    view.data = buffer.data();
    view.width = k_width;
    view.height = k_height;
    view.row_stride_bytes = k_stride;
    view.format = mirador::PixelFormat::kGray8;

    const mirador::integrations::NcnnTensor input = packed_gray_view(view);
    if (!expect_packed_gray_shape(input, pixels)) {
        return;
    }

    mirador::integrations::NcnnRuntimeOptions options;
    options.param_path = (dir / "model.param").string();
    options.bin_path = (dir / "model.bin").string();
    auto runtime = mirador::integrations::NcnnRuntime::create(options);
    expect_true(runtime.ok(), "NcnnRuntime::create loads the synthetic model");
    if (!runtime.ok()) {
        std::printf("  create status: %s\n", runtime.status().message().c_str());
        return;
    }
    auto runtime_value = std::move(runtime).take_value();

    const mirador::ExecutionContext never_cancelled;
    const auto output = runtime_value.run("data", input, "out", never_cancelled);
    expect_true(output.ok(), "run(data -> out) succeeds");
    if (!output.ok()) {
        std::printf("  run status: %s\n", output.status().message().c_str());
        return;
    }
    const mirador::integrations::NcnnTensor& tensor = output.value();
    expect_true(tensor.width == 2 && tensor.height == 2 && tensor.channels == 1, "run output shape is 2x2x1");
    expect_true(tensor.data.size() == 4U, "run output data holds exactly 4 floats");

    const std::array<float, 4> expected = naive_convolution(pixels);
    for (size_t out_y = 0; out_y < 2; ++out_y) {
        for (size_t out_x = 0; out_x < 2; ++out_x) {
            const size_t index = out_y * 2U + out_x;
            const auto got = static_cast<double>(tensor.data[index]);
            const auto want = static_cast<double>(expected[index]);
            expect_true(std::fabs(got - want) <= 1e-4, std::string("forward value at (") + std::to_string(out_x) + "," +
                                                           std::to_string(out_y) + ") matches naive convolution: got " +
                                                           std::to_string(got) + " want " + std::to_string(want));
        }
    }
}

// Check 2: same input, two runs, bit-identical output (num_threads=1 contract).
void check_determinism(const std::filesystem::path& dir) {
    std::vector<std::byte> buffer(k_gray_buffer_size);
    for (int32_t y = 0; y < 4; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            buffer[static_cast<size_t>(y) * 8U + static_cast<size_t>(x)] = static_cast<std::byte>(gray_pixel(x, y));
        }
    }
    mirador::ImageView view;
    view.data = buffer.data();
    view.width = 4;
    view.height = 4;
    view.row_stride_bytes = 8;
    view.format = mirador::PixelFormat::kGray8;

    mirador::integrations::NcnnRuntimeOptions options;
    options.param_path = (dir / "model.param").string();
    options.bin_path = (dir / "model.bin").string();
    auto runtime = mirador::integrations::NcnnRuntime::create(options);
    if (!runtime.ok()) {
        std::printf("FAIL: determinism check could not load the model\n");
        ++g_failed_checks;
        return;
    }
    auto runtime_value = std::move(runtime).take_value();
    const mirador::integrations::NcnnTensor input = packed_gray_view(view);
    const mirador::ExecutionContext never_cancelled;
    const auto first = runtime_value.run("data", input, "out", never_cancelled);
    const auto second = runtime_value.run("data", input, "out", never_cancelled);
    expect_true(first.ok() && second.ok(), "determinism: both runs succeed");
    if (!first.ok() || !second.ok()) {
        return;
    }
    const bool identical =
        first.value().width == second.value().width && first.value().height == second.value().height &&
        first.value().channels == second.value().channels && first.value().data.size() == second.value().data.size() &&
        std::memcmp(first.value().data.data(), second.value().data.data(), first.value().data.size() * sizeof(float)) ==
            0;
    expect_true(identical, "determinism: two runs produce byte-identical output");
}

// Check 3: NV12 is a documented unsupported input for pack_image and must be
// rejected with kUnsupportedFormat (the view itself must be structurally valid).
void check_nv12_rejected() {
    constexpr size_t k_luma_size = 16U;   // 4x4 Y plane
    constexpr size_t k_chroma_size = 8U;  // 2 rows x 4 bytes of interleaved UV
    std::array<std::byte, k_luma_size> luma = {};
    std::array<std::byte, k_chroma_size> chroma = {};

    mirador::ImageView view;
    view.data = luma.data();
    view.width = 4;
    view.height = 4;
    view.row_stride_bytes = 4;
    view.format = mirador::PixelFormat::kNv12;
    view.secondary_plane.data = chroma.data();
    view.secondary_plane.row_stride_bytes = 4;

    const mirador::ExecutionContext never_cancelled;
    const auto packed = mirador::integrations::pack_image(view, never_cancelled);
    expect_code(packed.status(), mirador::ErrorCode::kUnsupportedFormat,
                "pack_image rejects a valid NV12 view with kUnsupportedFormat");
}

// Check 4: create() argument validation.
void check_create_error_paths(const std::filesystem::path& dir) {
    const mirador::integrations::NcnnRuntimeOptions empty_options;
    const auto empty = mirador::integrations::NcnnRuntime::create(empty_options);
    expect_code(empty.status(), mirador::ErrorCode::kInvalidArgument,
                "create with empty paths returns kInvalidArgument");

    mirador::integrations::NcnnRuntimeOptions missing_options;
    missing_options.param_path = (dir / "does_not_exist.param").string();
    missing_options.bin_path = (dir / "does_not_exist.bin").string();
    const auto missing = mirador::integrations::NcnnRuntime::create(missing_options);
    expect_code(missing.status(), mirador::ErrorCode::kBackendUnavailable,
                "create with missing model files returns kBackendUnavailable");
}

// Check 5: unknown blob names are caller errors, not backend failures.
void check_unknown_blob(mirador::integrations::NcnnRuntime& runtime, const mirador::integrations::NcnnTensor& input) {
    const mirador::ExecutionContext never_cancelled;
    const auto bad_input = runtime.run("nope", input, "out", never_cancelled);
    expect_code(bad_input.status(), mirador::ErrorCode::kInvalidArgument,
                "run with unknown input blob returns kInvalidArgument");

    const auto bad_output = runtime.run("data", input, "nope", never_cancelled);
    expect_code(bad_output.status(), mirador::ErrorCode::kInvalidArgument,
                "run with unknown output blob returns kInvalidArgument");
}

// Check 6: a cancelled context short-circuits before the forward pass.
void check_cancelled_run(mirador::integrations::NcnnRuntime& runtime, const mirador::integrations::NcnnTensor& input) {
    mirador::ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const auto result = runtime.run("data", input, "out", cancelled);
    expect_code(result.status(), mirador::ErrorCode::kCancelled,
                "run with a cancelled context returns kCancelled before forward");
}

// Check 7: RGB8 packing produces a 4x4x3 planar CHW tensor with values copied
// as stored, honoring a padded row stride (16 bytes for 12 bytes of pixels).
void check_rgb_pack_layout() {
    constexpr int32_t k_width = 4;
    constexpr int32_t k_height = 4;
    constexpr int64_t k_stride = 16;  // 12 usable bytes + 4 padding bytes per row

    std::vector<std::byte> buffer(static_cast<size_t>(k_stride) * static_cast<size_t>(k_height));
    for (int32_t y = 0; y < k_height; ++y) {
        for (int32_t x = 0; x < k_width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                buffer[static_cast<size_t>(y) * static_cast<size_t>(k_stride) + static_cast<size_t>(x) * 3U +
                       static_cast<size_t>(channel)] = static_cast<std::byte>(rgb_pixel(x, y, channel));
            }
        }
        for (int32_t x = k_width * 3; x < k_stride; ++x) {
            buffer[static_cast<size_t>(y) * static_cast<size_t>(k_stride) + static_cast<size_t>(x)] =
                static_cast<std::byte>(0xCD);
        }
    }

    mirador::ImageView view;
    view.data = buffer.data();
    view.width = k_width;
    view.height = k_height;
    view.row_stride_bytes = k_stride;
    view.format = mirador::PixelFormat::kRgb8;

    const mirador::ExecutionContext never_cancelled;
    const auto packed = mirador::integrations::pack_image(view, never_cancelled);
    expect_true(packed.ok(), "pack_image succeeds for an RGB8 view");
    if (!packed.ok()) {
        std::printf("  pack status: %s\n", packed.status().message().c_str());
        return;
    }
    const mirador::integrations::NcnnTensor& tensor = packed.value();
    expect_true(tensor.width == 4 && tensor.height == 4 && tensor.channels == 3, "packed RGB8 tensor shape is 4x4x3");
    expect_true(tensor.data.size() == 48U, "packed RGB8 tensor holds 48 floats");

    // Sample points per the planar CHW contract: plane c at offset c*w*h.
    const auto sample = [&tensor](int32_t x, int32_t y, int channel) {
        return tensor.data[static_cast<size_t>(channel) * 16U + static_cast<size_t>(y) * 4U + static_cast<size_t>(x)];
    };
    expect_true(sample(1, 0, 0) == static_cast<float>(rgb_pixel(1, 0, 0)),
                "RGB8 sample (x=1, y=0, c=0) matches planar CHW layout");
    expect_true(sample(3, 1, 1) == static_cast<float>(rgb_pixel(3, 1, 1)),
                "RGB8 sample (x=3, y=1, c=1) honors the padded row stride");
    expect_true(sample(2, 3, 2) == static_cast<float>(rgb_pixel(2, 3, 2)),
                "RGB8 sample (x=2, y=3, c=2) matches planar CHW layout");
}

bool run_all_checks(const std::filesystem::path& dir) {
    if (!write_model_files(dir)) {
        return false;
    }
    check_forward_numerics(dir);
    check_determinism(dir);
    check_nv12_rejected();
    check_create_error_paths(dir);

    // Remaining checks reuse one healthy runtime and one packed input.
    mirador::integrations::NcnnRuntimeOptions options;
    options.param_path = (dir / "model.param").string();
    options.bin_path = (dir / "model.bin").string();
    auto runtime = mirador::integrations::NcnnRuntime::create(options);
    if (!runtime.ok()) {
        std::printf("FAIL: shared runtime could not load the model: %s\n", runtime.status().message().c_str());
        ++g_failed_checks;
    } else {
        auto runtime_value = std::move(runtime).take_value();
        std::vector<std::byte> buffer(k_gray_buffer_size);
        for (int32_t y = 0; y < 4; ++y) {
            for (int32_t x = 0; x < 4; ++x) {
                buffer[static_cast<size_t>(y) * 8U + static_cast<size_t>(x)] = static_cast<std::byte>(gray_pixel(x, y));
            }
        }
        mirador::ImageView view;
        view.data = buffer.data();
        view.width = 4;
        view.height = 4;
        view.row_stride_bytes = 8;
        view.format = mirador::PixelFormat::kGray8;
        const mirador::integrations::NcnnTensor input = packed_gray_view(view);
        check_unknown_blob(runtime_value, input);
        check_cancelled_run(runtime_value, input);
    }
    check_rgb_pack_layout();
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
        std::printf("ncnn smoke: FAIL (%d failed check(s))\n", g_failed_checks);
        return 1;
    }
    std::printf("ncnn smoke: PASS (synthetic model, numerics, determinism, error paths)\n");
    return 0;
}
