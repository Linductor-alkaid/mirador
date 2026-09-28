// M7-12 (DEC-020/DEC-015): self-contained synthetic-model smoke test for the
// NanoTrack reference tracking backend, written by the independent
// verification round. Generates deterministic ncnn param/bin pairs at runtime
// (zero weights in the source tree, project standards section 9.2.4) whose
// zero kernels turn every convolution into its bias, so the frozen decode
// contract of nanotrack_backend.hpp becomes hand-checkable exactly:
//
//   * Frozen decode (winner scan, ltrb mapping, clamp, confidence): with
//     window_influence = 0 all 16 cells respond identically, the frozen
//     first-row-major-maximum rule must pick cell (0,0), and the crafted box
//     biases ltrb = [2,1,4,3] map to exact bounds {2,3,6,4} for the prior
//     {16,16,8,8} (scale_z = 1, crop 32 px, half_instance 16 — all small
//     integers, exact in float32).
//   * The caller's prior drives the position (DEC-020): shifting the prior by
//     (+4,+4) shifts the decoded bounds by exactly (+4,+4).
//   * The committed reference-size advance is checked against an independent
//     double-precision replay of the documented decode steps.
//   * Validation matrix (contract blocks 4/6): malformed views/bounds ->
//     kInvalidArgument, undeclared formats -> kUnsupportedFormat, and the
//     FROZEN ordering — validation precedes cancellation.
//   * kCancelled/kTimeout explicit, session stays usable, per-call state is
//     all-or-nothing (contract block 3); kBackendFailure visible and never
//     stale (zero-bias box model: no usable cell; off-image box: degenerate
//     clamp; wrong blob names: the wrapper's kInvalidArgument remapped to
//     kBackendFailure; 3-channel cls: the frozen shape gate).
//   * Byte budget (RULE-06): work_budget_bytes is checked per allocation
//     request — a budget below one crop fails initialize, a budget below one
//     search crop fails update, both with kBudgetExceeded.
//   * Pixel sensitivity and crop geometry (pixel-sensitive model variant):
//     changing one in-crop byte changes the result; the same change outside
//     the search crop does not; stride padding and 0/90/180/270 rotation
//     metadata never do (block 6); input bytes are never modified.
//   * Per-sequence bit determinism (block 8) across sessions and backends
//     (num_threads pinned to 1; any other value is rejected at create()).
//   * Privacy (RULE-10/DOD-06): no filesystem writes around the run and no
//     pixel-borne marker in any produced Status message.
//
// The synthetic models only validate pipeline and decode correctness — they
// say nothing about tracking quality (DOD-05/RISK-2026-13: real weights run
// through caller-provided explicit paths only).

#include "nanotrack_backend.hpp"

#include "ncnn_runtime.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/tracker_backend.hpp>

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
#include <limits>
#include <map>
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

// Bit-exact float comparison — the decode pins below assert exact float32
// outcomes, so equality is the point, not an accident (bugprone-float-equal is
// acknowledged once here instead of at every call site).
bool floats_equal(float lhs, float rhs) {
    return lhs == rhs;  // NOLINT(bugprone-float-equal)
}

bool tensors_equal(const mirador::integrations::NcnnTensor& lhs, const mirador::integrations::NcnnTensor& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height && lhs.channels == rhs.channels && lhs.data == rhs.data;
}

// --- synthetic model generation -----------------------------------------------
//
// Geometry shared by every model variant: exemplar 16, instance 32, stride 8,
// score grid 4x4. The backbone is a 1x1 convolution (size preserving), so the
// template feature is 2x16x16 and the search feature 2x32x32. The head's two
// branches collapse their features with strided convolutions: cls kernel 4
// stride 4 on the 16x16 template feature -> 2x4x4, box kernel 8 stride 8 on
// the 32x32 search feature -> 4x4x4. ncnn weight layout (pinned ncnn
// convolution.cpp: weight_data.reshape(maxk, num_input, num_output)) is
// output-channel major, then input channel, then spatial; raw-float weight
// blobs carry a zero flag word, biases follow flagless (modelbin.cpp).

constexpr int32_t k_exemplar = 16;
constexpr int32_t k_instance = 32;
constexpr int32_t k_score_size = 4;
constexpr int32_t k_total_stride = 8;
constexpr float k_context_amount = 0.5F;
constexpr float k_penalty_k = 0.148F;
constexpr float k_size_learning_rate = 0.39F;
constexpr int64_t k_default_work_budget = int64_t{16} * 1024 * 1024;

// Frozen box biases of the constant model: ltrb = [2,1,4,3] search-crop px.
constexpr float k_box_left = 2.0F;
constexpr float k_box_top = 1.0F;
constexpr float k_box_right = 4.0F;
constexpr float k_box_bottom = 3.0F;
// Frozen cls biases: background logit 0, foreground logit 2.
constexpr float k_cls_background_logit = 0.0F;
constexpr float k_cls_foreground_logit = 2.0F;

constexpr std::string_view kBackboneParam = R"(7767517
2 2
Input data 0 1 input 0=0 1=0 2=3
Convolution conv 1 1 input output 0=2 1=1 2=1 3=1 4=0 5=0 6=6
)";

std::string head_param_text(std::string_view cls_blob, std::string_view box_blob, int32_t cls_channels) {
    // Dynamic input blobs (w=h=0): the convolutions fix the geometry.
    std::string text = "7767517\n4 4\n";
    text += "Input headin1 0 1 input1 0=0 1=0 2=2\n";
    text += "Input headin2 0 1 input2 0=0 1=0 2=2\n";
    text += "Convolution clsconv 1 1 input1 " + std::string(cls_blob) + " 0=" + std::to_string(cls_channels) +
            " 1=4 2=1 3=4 4=0 5=1 6=" + std::to_string(cls_channels * 2 * 16) + "\n";
    text += "Convolution boxconv 1 1 input2 " + std::string(box_blob) + " 0=4 1=8 2=1 3=8 4=0 5=1 6=512\n";
    return text;
}

void append_flag(std::vector<std::byte>& bin) {
    // ncnn raw-float32 weight blob flag word (modelbin.cpp type-0 path).
    constexpr std::array<std::byte, 4> flag = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    bin.insert(bin.end(), flag.begin(), flag.end());
}

void append_float(std::vector<std::byte>& bin, float value) {
    uint32_t bits = 0;
    static_cast<void>(std::memcpy(&bits, &value, sizeof(bits)));
    bin.push_back(static_cast<std::byte>(bits & 0xFFU));
    bin.push_back(static_cast<std::byte>((bits >> 8U) & 0xFFU));
    bin.push_back(static_cast<std::byte>((bits >> 16U) & 0xFFU));
    bin.push_back(static_cast<std::byte>((bits >> 24U) & 0xFFU));
}

void append_zeros(std::vector<std::byte>& bin, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        append_float(bin, 0.0F);
    }
}

bool write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    return stream.good();
}

bool write_file(const std::filesystem::path& path, const std::vector<std::byte>& bin) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bin.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                 static_cast<std::streamsize>(bin.size()));
    stream.close();
    return stream.good();
}

// Head model variants under test (all share the frozen blob contract unless
// the variant deliberately breaks it):
//   Constant    : zero kernels everywhere -> outputs are the crafted biases.
//   NoUsableCell: all-zero box biases -> every cell has pw = ph = 0 -> the
//                 response scan finds no usable cell.
//   OffImage    : box biases place the decoded box entirely left of the crop
//                 center -> the clamp leaves nothing -> kBackendFailure.
//   WrongBlobs  : outputs named clsout/boxout -> unknown output blob, remapped
//                 from kInvalidArgument to kBackendFailure.
//   WrongCls    : 3-channel cls output -> the frozen 2-channel shape gate.
//   Pixel       : box branch weights 1/64 on the search feature's channel 1
//                 (feat = 0.01*R), so decoded bounds follow the crop bytes.
enum class HeadVariant : std::uint8_t { kConstant, kNoUsableCell, kOffImage, kWrongBlobs, kWrongCls, kPixel };

bool write_backbone(const std::filesystem::path& dir, bool pixel_sensitive) {
    if (!write_file(dir / "backbone.param", kBackboneParam)) {
        return false;
    }
    std::vector<std::byte> bin;
    append_flag(bin);
    append_zeros(bin, 6U);  // 2 output channels x 3 input channels, 1x1 kernel
    if (pixel_sensitive) {
        // feat0 = 0.01*(R+G+B); feat1 = 0.01*R (the channel the pixel head reads).
        bin.resize(4U);
        for (const float weight : {0.01F, 0.01F, 0.01F, 0.01F, 0.0F, 0.0F}) {
            append_float(bin, weight);
        }
    }
    return write_file(dir / "backbone.bin", bin);
}

bool write_head(const std::filesystem::path& dir, HeadVariant variant) {
    const std::string_view cls_blob = variant == HeadVariant::kWrongBlobs ? "clsout" : "output1";
    const std::string_view box_blob = variant == HeadVariant::kWrongBlobs ? "boxout" : "output2";
    const int32_t cls_channels = variant == HeadVariant::kWrongCls ? 3 : 2;
    if (!write_file(dir / "head.param", head_param_text(cls_blob, box_blob, cls_channels))) {
        return false;
    }
    std::vector<std::byte> bin;
    // cls branch: kernel 4x4 stride 4 over the template feature; background is
    // a constant zero logit, and the foreground logit reads channel 1 of the
    // template feature with 1/16 weights (= the 4x4 block mean of feat1 =
    // 0.01*R, plus the bias) — so the winner cell is the maximum-R block of
    // the stored template. Weight order: [out][in][spatial].
    append_flag(bin);
    append_zeros(bin, static_cast<size_t>(cls_channels) * 2U * 16U);
    if (variant == HeadVariant::kPixel) {
        // fg (=out 1) reads feat1: skip the flag, out 0 (2 in-channels x 16
        // taps) and out 1's in 0 (16 taps); write 16 x 0.0625 (0x3D800000).
        constexpr uint32_t k_one_sixteenth = 0x3D800000U;
        size_t offset = 4U + (2U * 16U + 16U) * 4U;
        for (int32_t i = 0; i < 16; ++i) {
            bin[offset + 0U] = static_cast<std::byte>(k_one_sixteenth & 0xFFU);
            bin[offset + 1U] = static_cast<std::byte>((k_one_sixteenth >> 8U) & 0xFFU);
            bin[offset + 2U] = static_cast<std::byte>((k_one_sixteenth >> 16U) & 0xFFU);
            bin[offset + 3U] = static_cast<std::byte>((k_one_sixteenth >> 24U) & 0xFFU);
            offset += 4U;
        }
    }
    append_float(bin, k_cls_background_logit);
    append_float(bin, k_cls_foreground_logit);
    if (cls_channels == 3) {
        append_float(bin, 0.0F);
    }
    // box branch: kernel 8x8 stride 8 over the 2-channel search feature.
    append_flag(bin);
    append_zeros(bin, 512U);
    if (variant == HeadVariant::kPixel) {
        // Overwrite channel-1 weights with +-1/64: left/top get +mean(patch of
        // feat1 = 0.01*R), right/bottom get -mean. ltrb then = bias +- m, so
        // pw = 6 and ph = 4 stay constant in every cell (identical penalties,
        // the winner is pinned to cell (0,0)) while the decoded box center
        // tracks patch (0,0)'s mean R linearly. Weight order: [out][in][spatial].
        size_t offset = 4U;                                           // past the cls flag
        offset += static_cast<size_t>(cls_channels) * 2U * 16U * 4U;  // past the cls weights
        offset += cls_channels == 3 ? 12U : 8U;                       // past the cls biases
        offset += 4U;                                                 // past the box flag
        for (int32_t out_channel = 0; out_channel < 4; ++out_channel) {
            offset += static_cast<size_t>(64U) * 4U;  // input channel 0 stays zero
            // 0x3C800000 = +0.015625 (2^-6), 0xBC800000 = -0.015625.
            const uint32_t bits = out_channel < 2 ? 0x3C800000U : 0xBC800000U;
            for (int32_t i = 0; i < 64; ++i) {
                bin[offset + 0U] = static_cast<std::byte>(bits & 0xFFU);
                bin[offset + 1U] = static_cast<std::byte>((bits >> 8U) & 0xFFU);
                bin[offset + 2U] = static_cast<std::byte>((bits >> 16U) & 0xFFU);
                bin[offset + 3U] = static_cast<std::byte>((bits >> 24U) & 0xFFU);
                offset += 4U;
            }
        }
    }
    float left = k_box_left;
    float top = k_box_top;
    float right = k_box_right;
    float bottom = k_box_bottom;
    if (variant == HeadVariant::kNoUsableCell) {
        left = 0.0F;
        top = 0.0F;
        right = 0.0F;
        bottom = 0.0F;
    } else if (variant == HeadVariant::kOffImage) {
        // pw = 2, ph = 2000 > 0 (usable cell), but the box sits ~1000 px left
        // of the cell: the mapped box misses the image and nothing survives
        // the clamp.
        left = 1000.0F;
        top = 1000.0F;
        right = -998.0F;
        bottom = 1000.0F;
    }
    append_float(bin, left);
    append_float(bin, top);
    append_float(bin, right);
    append_float(bin, bottom);
    return write_file(dir / "head.bin", bin);
}

// --- fixtures: frames, views, options ------------------------------------------

constexpr int32_t k_base_width = 48;
constexpr int32_t k_base_height = 36;   // even base frame
constexpr int32_t k_matrix_width = 49;  // odd sizes for the coordinate matrix
constexpr int32_t k_matrix_height = 35;

/// Owning RGB buffer with an explicit row stride (padding bytes included).
struct RgbImage {
    std::vector<std::byte> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int64_t stride = 0;
};

mirador::ImageView view_of(const RgbImage& image, mirador::Rotation rotation = mirador::Rotation::k0) {
    mirador::ImageView view;
    view.data = image.pixels.data();
    view.width = image.width;
    view.height = image.height;
    view.row_stride_bytes = image.stride;
    view.format = mirador::PixelFormat::kRgb8;
    view.rotation = rotation;
    return view;
}

uint8_t base_channel(int32_t x, int32_t y, int channel) {
    static constexpr std::array<int, 3> k_channels = {7, 13, 29};
    const int value =
        (x * k_channels[static_cast<size_t>(channel)] + y * (k_channels[static_cast<size_t>(channel)] + 3)) % 200;
    return static_cast<uint8_t>(value + 30);
}

RgbImage make_frame(int32_t width, int32_t height, int64_t stride_padding) {
    RgbImage image;
    image.width = width;
    image.height = height;
    image.stride = static_cast<int64_t>(width) * 3 + stride_padding;
    image.pixels.assign(static_cast<size_t>(image.stride) * static_cast<size_t>(height), std::byte{0xAA});
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x) * 3U +
                             static_cast<size_t>(channel)] = std::byte{base_channel(x, y, channel)};
            }
        }
    }
    return image;
}

/// The same presented content over a wider stride (padding bytes stay 0xAA).
RgbImage repad(const RgbImage& frame, int64_t stride_padding) {
    if (stride_padding == 0) {
        return frame;
    }
    RgbImage image;
    image.width = frame.width;
    image.height = frame.height;
    image.stride = static_cast<int64_t>(frame.width) * 3 + stride_padding;
    image.pixels.assign(static_cast<size_t>(image.stride) * static_cast<size_t>(frame.height), std::byte{0xAA});
    for (int32_t y = 0; y < frame.height; ++y) {
        for (int32_t x = 0; x < frame.width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x) * 3U +
                             static_cast<size_t>(channel)] =
                    frame.pixels[static_cast<size_t>(y) * static_cast<size_t>(frame.stride) +
                                 static_cast<size_t>(x) * 3U + static_cast<size_t>(channel)];
            }
        }
    }
    return image;
}

void bump_red_pixel(RgbImage& image, int32_t x, int32_t y, int32_t delta) {
    const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x) * 3U;
    const auto value = static_cast<uint8_t>(std::to_integer<uint8_t>(image.pixels[offset]) + delta);
    image.pixels[offset] = std::byte{value};
}

/// FNV-1a over the whole buffer (content and stride padding).
uint64_t buffer_hash(const RgbImage& image) {
    uint64_t hash = 14695981039346656037ULL;
    for (const std::byte byte : image.pixels) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

mirador::ExecutionContext clean_context() {
    return {};
}

mirador::ExecutionContext cancelled_context() {
    mirador::ExecutionContext context;
    context.is_cancelled = [] { return true; };
    return context;
}

mirador::ExecutionContext expired_deadline_context() {
    mirador::ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    return context;
}

mirador::ExecutionContext hostile_context() {
    // Already cancelled AND past the deadline: the frozen ordering still
    // demands kInvalidArgument for malformed requests.
    mirador::ExecutionContext context;
    context.is_cancelled = [] { return true; };
    context.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    return context;
}

mirador::integrations::NanoTrackerOptions make_options(const std::filesystem::path& dir, float window_influence,
                                                       int64_t work_budget_bytes) {
    mirador::integrations::NanoTrackerOptions options;
    options.backbone_param_path = (dir / "backbone.param").string();
    options.backbone_bin_path = (dir / "backbone.bin").string();
    options.head_param_path = (dir / "head.param").string();
    options.head_bin_path = (dir / "head.bin").string();
    options.identity = mirador::integrations::NanoTrackModelIdentity{"bench-nanotrack", "rev-7"};
    options.exemplar_size = k_exemplar;
    options.instance_size = k_instance;
    options.score_size = k_score_size;
    options.total_stride = k_total_stride;
    options.context_amount = k_context_amount;
    options.penalty_k = k_penalty_k;
    options.window_influence = window_influence;
    options.size_learning_rate = k_size_learning_rate;
    options.num_threads = 1;
    options.work_budget_bytes = work_budget_bytes;
    return options;
}

bool bounds_eq(const mirador::RectF& bounds, float x, float y, float width, float height) {
    return floats_equal(bounds.x, x) && floats_equal(bounds.y, y) && floats_equal(bounds.width, width) &&
           floats_equal(bounds.height, height);
}

bool inside_image(const mirador::RectF& bounds, int32_t width, int32_t height) {
    return std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width) &&
           std::isfinite(bounds.height) && bounds.x >= 0.0F && bounds.y >= 0.0F && bounds.width > 0.0F &&
           bounds.height > 0.0F && bounds.x + bounds.width <= static_cast<float>(width) &&
           bounds.y + bounds.height <= static_cast<float>(height);
}

bool conf_close(float confidence, float expected) {
    return std::fabs(confidence - expected) <= 1.0e-5F;
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
        std::filesystem::path candidate = base / ("mirador_nanotrack_smoke_" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            return {};
        }
    }
    return {};
}

/// Snapshot of the /tmp entry names carrying this suite's prefix (the shared
/// /tmp on this machine sees unrelated external files; the prefix filter keeps
/// the privacy check immune to them).
std::map<std::string, int> snapshot_own_temp_entries() {
    std::map<std::string, int> entries;
    std::error_code base_ec;
    std::filesystem::directory_iterator it(std::filesystem::temp_directory_path(base_ec),
                                           std::filesystem::directory_options::skip_permission_denied, base_ec);
    if (base_ec) {
        return entries;
    }
    const std::filesystem::directory_iterator end;
    while (it != end) {
        if (it->path().filename().string().starts_with("mirador_nanotrack")) {
            entries[it->path().filename().string()] = 1;
        }
        std::error_code increment_ec;
        it.increment(increment_ec);
        if (increment_ec) {
            break;
        }
    }
    return entries;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- checks ---------------------------------------------------------------------

// 1. Factory, identity and the determinism-critical defaults.
void check_factory_and_info(const std::filesystem::path& dir) {
    const mirador::integrations::NanoTrackerOptions options = make_options(dir, 0.0F, k_default_work_budget);
    expect_true(options.num_threads == 1, "NanoTrackerOptions default num_threads is 1 (bit determinism)");
    auto created = mirador::integrations::NanoTrackerBackend::create(options);
    expect_true(created.ok(), "backend loads the synthetic constant model");
    if (!created.ok()) {
        std::printf("  create status: %s\n", created.status().message().c_str());
        return;
    }
    auto backend = std::move(created).take_value();
    const mirador::BackendInfo info = backend.info();
    expect_true(info.name == "nanotrack-ncnn-reference", "info name is \"nanotrack-ncnn-reference\"");
    expect_true(info.implementation_version == "0.1.0", "info implementation_version is 0.1.0");
    expect_true(info.model_id == "bench-nanotrack" && info.model_revision == "rev-7",
                "info round-trips model_id and model_revision");
    expect_true(info.accepted_formats.size() == 1U && info.accepted_formats[0] == mirador::PixelFormat::kRgb8,
                "accepted_formats is exactly {kRgb8}");
    expect_true(!info.thread_safe, "info.thread_safe is false");
    const mirador::Result<void> checked = mirador::validate(info);
    expect_true(checked.ok(), "info() passes BackendInfo::validate");
}

// 2. Factory rejection matrix (every create() guard, including the pinned
// num_threads=1 determinism gate).
void check_factory_errors(const std::filesystem::path& dir) {
    const auto create_with = [&](auto&& tweak, const std::string& what) {
        mirador::integrations::NanoTrackerOptions options = make_options(dir, 0.0F, k_default_work_budget);
        tweak(options);
        const auto created = mirador::integrations::NanoTrackerBackend::create(options);
        expect_code(created.status(), mirador::ErrorCode::kInvalidArgument, what);
    };
    create_with([](auto& o) { o.backbone_param_path.clear(); }, "empty backbone param path -> kInvalidArgument");
    create_with([](auto& o) { o.head_bin_path.clear(); }, "empty head bin path -> kInvalidArgument");
    create_with([](auto& o) { o.num_threads = 2; }, "num_threads=2 -> kInvalidArgument (determinism pinned)");
    create_with([](auto& o) { o.exemplar_size = 0; }, "exemplar_size=0 -> kInvalidArgument");
    create_with([](auto& o) { o.instance_size = 15; }, "instance_size < exemplar_size -> kInvalidArgument");
    create_with([](auto& o) { o.score_size = 1; }, "score_size=1 -> kInvalidArgument");
    create_with([](auto& o) { o.score_size = 129; }, "score_size=129 -> kInvalidArgument");
    create_with([](auto& o) { o.total_stride = 0; }, "total_stride=0 -> kInvalidArgument");
    create_with([](auto& o) { o.context_amount = 1.0F; }, "context_amount=1.0 -> kInvalidArgument");
    create_with([](auto& o) { o.penalty_k = -0.1F; }, "penalty_k<0 -> kInvalidArgument");
    create_with([](auto& o) { o.window_influence = 1.01F; }, "window_influence>1 -> kInvalidArgument");
    create_with([](auto& o) { o.size_learning_rate = -0.5F; }, "size_learning_rate<0 -> kInvalidArgument");
    create_with([](auto& o) { o.work_budget_bytes = 0; }, "work_budget_bytes=0 -> kInvalidArgument");

    mirador::integrations::NanoTrackerOptions missing = make_options(dir, 0.0F, k_default_work_budget);
    missing.head_param_path = (dir / "missing.param").string();
    const auto missing_backend = mirador::integrations::NanoTrackerBackend::create(missing);
    expect_code(missing_backend.status(), mirador::ErrorCode::kBackendUnavailable,
                "missing model files -> kBackendUnavailable");
}

// 3. The wrapper-level plumbing behind the backend: exact output values of the
// constant head (zero kernels -> biases), and single- vs multi-output
// consistency of run_multi's per-output extractors.
void check_wrapper_constant_head(const std::filesystem::path& dir) {
    auto backbone = mirador::integrations::NcnnRuntime::create(
        {(dir / "backbone.param").string(), (dir / "backbone.bin").string(), 1});
    auto head =
        mirador::integrations::NcnnRuntime::create({(dir / "head.param").string(), (dir / "head.bin").string(), 1});
    expect_true(backbone.ok() && head.ok(), "raw wrapper loads the synthetic model pair");
    if (!backbone.ok() || !head.ok()) {
        return;
    }
    expect_true(head.value().num_threads() == 1, "wrapper reports num_threads 1");

    mirador::integrations::NcnnTensor template_feature;
    template_feature.width = k_exemplar;
    template_feature.height = k_exemplar;
    template_feature.channels = 2;
    template_feature.data.assign(size_t{2U} * 16U * 16U, 0.0F);
    mirador::integrations::NcnnTensor search_feature;
    search_feature.width = k_instance;
    search_feature.height = k_instance;
    search_feature.channels = 2;
    search_feature.data.assign(size_t{2U} * 32U * 32U, 0.0F);

    const auto both = head.value().run_multi({{"input1", template_feature}, {"input2", search_feature}},
                                             {"output1", "output2"}, clean_context());
    expect_true(both.ok(), "run_multi serves both head outputs");
    if (!both.ok()) {
        std::printf("  run_multi status: %s\n", both.status().message().c_str());
        return;
    }
    expect_true(both.value().size() == 2U, "run_multi returns both requested outputs in order");
    const mirador::integrations::NcnnTensor& cls = both.value()[0];
    const mirador::integrations::NcnnTensor& box = both.value()[1];
    expect_true(cls.channels == 2 && cls.width == k_score_size && cls.height == k_score_size, "cls output is 2x4x4");
    expect_true(box.channels == 4 && box.width == k_score_size && box.height == k_score_size, "box output is 4x4x4");
    bool cls_exact = cls.data.size() == 32U;  // 2 channels x 4 x 4
    for (size_t i = 0; cls_exact && i < 16U; ++i) {
        cls_exact = floats_equal(cls.data[i], k_cls_background_logit);
    }
    for (size_t i = 16U; cls_exact && i < 32U; ++i) {
        cls_exact = floats_equal(cls.data[i], k_cls_foreground_logit);
    }
    expect_true(cls_exact, "cls planes are exactly the background/foreground logits (zero kernel -> bias)");
    bool box_exact = box.data.size() == 64U;  // 4 channels x 4 x 4
    for (size_t cell = 0; box_exact && cell < 16U; ++cell) {
        box_exact = floats_equal(box.data[cell], k_box_left) && floats_equal(box.data[16U + cell], k_box_top) &&
                    floats_equal(box.data[32U + cell], k_box_right) && floats_equal(box.data[48U + cell], k_box_bottom);
    }
    expect_true(box_exact, "box planes are exactly the crafted ltrb biases in every cell");
    const auto single = head.value().run_multi({{"input1", template_feature}, {"input2", search_feature}}, {"output1"},
                                               clean_context());
    expect_true(single.ok() && single.value().size() == 1U && tensors_equal(single.value()[0], cls),
                "single-output extraction matches the multi-output first plane bitwise");
}

// 4. Weight-layout pins for the pixel-sensitive variant: the crafted backbone
// and box-head weights must land on the channels the decode consumes.
void check_wrapper_pixel_layout(const std::filesystem::path& dir) {
    auto backbone = mirador::integrations::NcnnRuntime::create(
        {(dir / "backbone.param").string(), (dir / "backbone.bin").string(), 1});
    auto head =
        mirador::integrations::NcnnRuntime::create({(dir / "head.param").string(), (dir / "head.bin").string(), 1});
    expect_true(backbone.ok() && head.ok(), "pixel-sensitive model pair loads");
    if (!backbone.ok() || !head.ok()) {
        return;
    }
    mirador::integrations::NcnnTensor rgb;
    rgb.width = 2;
    rgb.height = 2;
    rgb.channels = 3;
    rgb.data = {100.0F, 100.0F, 100.0F, 100.0F, 10.0F, 10.0F, 10.0F, 10.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    const auto feature = backbone.value().run("input", rgb, "output", clean_context());
    expect_true(feature.ok() && feature.value().channels == 2, "backbone keeps the 2-channel layout");
    if (!feature.ok()) {
        return;
    }
    // R=100 G=10 B=1: feat0 = 0.01*111, feat1 = 0.01*100 (weights [out][in]).
    expect_true(conf_close(feature.value().data[0], 1.11F) && conf_close(feature.value().data.back(), 1.0F),
                "backbone weights hit output channels in [out][in] order");

    mirador::integrations::NcnnTensor search;
    search.width = k_instance;
    search.height = k_instance;
    search.channels = 2;
    search.data.assign(size_t{2U} * 32U * 32U, 0.0F);
    for (size_t i = size_t{32U} * 32U; i < search.data.size(); ++i) {
        search.data[i] = 1.28F;  // channel 1 everywhere: box = bias + 1.28
    }
    mirador::integrations::NcnnTensor template_feature;
    template_feature.width = k_exemplar;
    template_feature.height = k_exemplar;
    template_feature.channels = 2;
    template_feature.data.assign(size_t{2U} * 16U * 16U, 0.0F);
    const auto box =
        head.value().run_multi({{"input1", template_feature}, {"input2", search}}, {"output2"}, clean_context());
    expect_true(box.ok() && box.value().size() == 1U, "pixel head box branch runs");
    if (!box.ok()) {
        return;
    }
    bool layout_exact = true;
    for (size_t cell = 0; layout_exact && cell < 16U; ++cell) {
        layout_exact = conf_close(box.value()[0].data[cell], k_box_left + 1.28F) &&
                       conf_close(box.value()[0].data[16U + cell], k_box_top + 1.28F) &&
                       conf_close(box.value()[0].data[32U + cell], k_box_right - 1.28F) &&
                       conf_close(box.value()[0].data[48U + cell], k_box_bottom - 1.28F);
    }
    expect_true(layout_exact, "box channel weights +-1/64 on channel 1 add/subtract the patch mean per ltrb channel");
}

// 5. Wrapper argument validation (the surface the backend drives).
void check_wrapper_argument_errors(const std::filesystem::path& dir) {
    auto head =
        mirador::integrations::NcnnRuntime::create({(dir / "head.param").string(), (dir / "head.bin").string(), 1});
    expect_true(head.ok(), "wrapper loads for the argument-error checks");
    if (!head.ok()) {
        return;
    }
    mirador::integrations::NcnnTensor feature;
    feature.width = 16;
    feature.height = 16;
    feature.channels = 2;
    feature.data.assign(size_t{2U} * 16U * 16U, 0.0F);

    const auto empty_inputs = head.value().run_multi({}, {"output1"}, clean_context());
    expect_code(empty_inputs.status(), mirador::ErrorCode::kInvalidArgument, "empty inputs -> kInvalidArgument");
    const auto duplicate_outputs =
        head.value().run_multi({{"input1", feature}}, {"output1", "output1"}, clean_context());
    expect_code(duplicate_outputs.status(), mirador::ErrorCode::kInvalidArgument,
                "duplicate output names -> kInvalidArgument");
    const auto unknown_input = head.value().run_multi({{"nosuch", feature}}, {"output1"}, clean_context());
    expect_code(unknown_input.status(), mirador::ErrorCode::kInvalidArgument, "unknown input blob -> kInvalidArgument");
    const auto unknown_output = head.value().run_multi({{"input1", feature}}, {"nosuch"}, clean_context());
    expect_code(unknown_output.status(), mirador::ErrorCode::kInvalidArgument,
                "unknown output blob -> kInvalidArgument");

    mirador::integrations::NcnnTensor mismatched = feature;
    mismatched.data.pop_back();
    const auto shape_mismatch = head.value().run_multi({{"input1", mismatched}}, {"output1"}, clean_context());
    expect_code(shape_mismatch.status(), mirador::ErrorCode::kInvalidArgument,
                "tensor shape/data mismatch -> kInvalidArgument");

    const auto cancelled = head.value().run_multi({{"input1", feature}}, {"output1"}, cancelled_context());
    expect_code(cancelled.status(), mirador::ErrorCode::kCancelled, "cancelled context -> kCancelled");
    const auto timed_out = head.value().run_multi({{"input1", feature}}, {"output1"}, expired_deadline_context());
    expect_code(timed_out.status(), mirador::ErrorCode::kTimeout, "expired deadline -> kTimeout");

    auto head_runtime = std::move(head).take_value();
    // Deliberate moved-from probe: the implementation moves out of head_runtime
    // and the drained object must report kBackendUnavailable afterwards.
    const mirador::integrations::NcnnRuntime recipient = std::move(head_runtime);
    static_cast<void>(recipient);
    const auto unavailable = head_runtime.run_multi(  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        {{"input1", feature}}, {"output1"}, clean_context());
    expect_code(unavailable.status(), mirador::ErrorCode::kBackendUnavailable,
                "moved-from runtime -> kBackendUnavailable");
}

// 6. The frozen decode, the caller-owned position prior, and the committed
// reference-size advance — hand-derived exact values plus an independent
// double-precision replay of the documented decode steps.
void check_frozen_decode(const std::filesystem::path& dir) {
    auto created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    expect_true(created.ok(), "backend loads for the decode checks");
    if (!created.ok()) {
        return;
    }
    auto backend = std::move(created).take_value();
    const RgbImage frame = make_frame(k_base_width, k_base_height, 0);
    const mirador::ImageView view = view_of(frame);

    const mirador::RectF initial{16.0F, 16.0F, 8.0F, 8.0F};
    const mirador::RectF shifted_prior{20.0F, 20.0F, 8.0F, 8.0F};
    auto initialized = backend.initialize(view, mirador::TrackerInitRequest{initial, {}}, clean_context());
    expect_true(initialized.ok(), "initialize succeeds on the synthetic frame");
    if (!initialized.ok()) {
        std::printf("  initialize status: %s\n", initialized.status().message().c_str());
        return;
    }
    mirador::TrackerSession& session = *initialized.value();

    // Update 1: all cells tie at penalty*score (window_influence = 0), so the
    // frozen first-row-major-maximum rule must pick cell (0,0): gx = gy = 0,
    // pred box [-2,-1]..[4,3] -> center (1,1), size (6,4); scale_z = 1 and
    // half_instance = 16 map it to center (5,5) -> exact bounds {2,3,6,4}.
    const auto first = session.update(view, mirador::TrackerUpdateRequest{initial, {}}, clean_context());
    expect_true(first.ok(), "update 1 succeeds");
    if (!first.ok()) {
        std::printf("  update status: %s\n", first.status().message().c_str());
        return;
    }
    expect_true(bounds_eq(first.value().bounds, 2.0F, 3.0F, 6.0F, 4.0F),
                "update 1 decodes to the exact hand-derived bounds {2,3,6,4}");
    expect_true(conf_close(first.value().confidence, 1.0F / (1.0F + std::exp(-k_cls_foreground_logit))),
                "update 1 confidence is sigmoid(foreground logit)");

    // The position prior is the caller's (DEC-020): shifting the prior by
    // (+4,+4) shifts the unclamped decode by exactly (+4,+4) — checked on a
    // fresh session so both updates run from the same reference size.
    mirador::TrackerUpdateResult shifted_result;
    bool shifted_ok = false;
    auto shifted_session = backend.initialize(view, mirador::TrackerInitRequest{initial, {}}, clean_context());
    expect_true(shifted_session.ok(), "shift-check session initializes");
    if (shifted_session.ok()) {
        const auto shifted =
            shifted_session.value()->update(view, mirador::TrackerUpdateRequest{shifted_prior, {}}, clean_context());
        expect_true(shifted.ok(), "update with the shifted prior succeeds");
        if (shifted.ok()) {
            shifted_result = shifted.value();
            shifted_ok = true;
            expect_true(bounds_eq(shifted.value().bounds, 6.0F, 7.0F, 6.0F, 4.0F),
                        "prior shift (+4,+4) shifts the decoded bounds by exactly (+4,+4)");
        }
    }

    // The next update on the original prior: the committed reference size
    // moved, so the same prior no longer replays earlier results (sequence
    // dependence). Compared against an independent double-precision replay of
    // the decode steps documented in nanotrack_backend.hpp.
    const auto third = session.update(view, mirador::TrackerUpdateRequest{initial, {}}, clean_context());
    expect_true(third.ok(), "the repeat update succeeds");
    if (third.ok()) {
        // Replay of the main session's two updates (both use the original
        // prior; the shift check ran on a separate session): winner cell (0,0)
        // -> pred center (1,1), pred size (6,4) crop px (the crafted biases);
        // conf = sigmoid(2); rate = penalty*conf*lr; ref moves toward the
        // mapped pred size; scale_z = 16/sqrt(wc*hc).
        const double conf = 1.0 / (1.0 + std::exp(-2.0));
        const double pred_w = 6.0;
        const double pred_h = 4.0;
        double ref_w = 8.0;
        double ref_h = 8.0;
        const std::array<double, 2> priors_x = {20.0, 20.0};
        const std::array<double, 2> priors_y = {20.0, 20.0};
        std::array<double, 2> expected_x = {};
        std::array<double, 2> expected_y = {};
        std::array<double, 2> expected_w = {};
        std::array<double, 2> expected_h = {};
        for (int step = 0; step < 2; ++step) {
            const double wc = ref_w + 0.5 * (ref_w + ref_h);
            const double hc = ref_h + 0.5 * (ref_w + ref_h);
            const double scale_z = 16.0 / std::sqrt(wc * hc);
            const double ref_crop_w = ref_w * scale_z;
            const double ref_crop_h = ref_h * scale_z;
            const double ref_pad = (ref_crop_w + ref_crop_h) * 0.5;
            const double ref_size = std::sqrt((ref_crop_w + ref_pad) * (ref_crop_h + ref_pad));
            const double ref_ratio = ref_crop_w / ref_crop_h;
            const double pad = (pred_w + pred_h) * 0.5;
            const double size_change = std::sqrt((pred_w + pad) * (pred_h + pad)) / ref_size;
            const double ratio_change = ref_ratio / (pred_w / pred_h);
            const double s = std::max(size_change, 1.0 / size_change);
            const double q = std::max(ratio_change, 1.0 / ratio_change);
            const double penalty = std::exp(-(s * q - 1.0) * static_cast<double>(k_penalty_k));
            const double rate = penalty * conf * static_cast<double>(k_size_learning_rate);
            const double width = pred_w / scale_z;
            const double height = pred_h / scale_z;
            expected_x[step] = priors_x[step] + (1.0 - 16.0) / scale_z - width / 2.0;
            expected_y[step] = priors_y[step] + (1.0 - 16.0) / scale_z - height / 2.0;
            expected_w[step] = width;
            expected_h[step] = height;
            // The stored reference size moves toward the predicted size in
            // prepared pixel space (the mapped one), per the class comment.
            ref_w += (width - ref_w) * rate;
            ref_h += (height - ref_h) * rate;
        }
        // Per-step tolerance comparison (all small magnitudes).
        const auto close = [](double actual, double expected) { return std::fabs(actual - expected) <= 5.0e-3; };
        expect_true(close(first.value().bounds.x, expected_x[0]) && close(first.value().bounds.y, expected_y[0]) &&
                        close(first.value().bounds.width, expected_w[0]) &&
                        close(first.value().bounds.height, expected_h[0]),
                    "update 1 matches the independent decode replay");
        expect_true(shifted_ok && close(shifted_result.bounds.x, 6.0) && close(shifted_result.bounds.y, 7.0),
                    "the shift-check session decodes from the untouched reference size");
        expect_true(close(third.value().bounds.x, expected_x[1]) && close(third.value().bounds.y, expected_y[1]) &&
                        close(third.value().bounds.width, expected_w[1]) &&
                        close(third.value().bounds.height, expected_h[1]),
                    "the repeat prior no longer replays update 1 and matches the replay (reference size advanced)");
        expect_true(conf_close(third.value().confidence, first.value().confidence),
                    "confidence stays the sigmoid of the unchanged foreground logit");
    }

    // A valid request whose decode lands entirely off-image is a loud
    // kBackendFailure (degenerate clamp), never a fabricated result.
    const auto off = session.update(view, mirador::TrackerUpdateRequest{mirador::RectF{0.0F, 0.0F, 8.0F, 8.0F}, {}},
                                    clean_context());
    expect_code(off.status(), mirador::ErrorCode::kBackendFailure, "off-image decode -> kBackendFailure");

    // Flush-edge and full-view requests stay valid (contract block 6).
    auto flush_created =
        mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    if (!flush_created.ok()) {
        expect_true(false, "flush-edge backend loads");
        return;
    }
    auto flush_backend = std::move(flush_created).take_value();
    auto flush_session = flush_backend.initialize(
        view, mirador::TrackerInitRequest{mirador::RectF{0.0F, 0.0F, 48.0F, 36.0F}, {}}, clean_context());
    expect_true(flush_session.ok(), "full-view initial bounds are valid (flush edge)");
    if (flush_session.ok()) {
        const auto flushed = flush_session.value()->update(
            view, mirador::TrackerUpdateRequest{mirador::RectF{0.0F, 0.0F, 48.0F, 36.0F}, {}}, clean_context());
        // A valid flush-edge request must pass validation (never kInvalidArgument/
        // kUnsupportedFormat); whether its decode lands inside the image is a
        // geometry question answered by a documented outcome — here the tiny
        // synthetic scale_z maps the fixed box off-image -> kBackendFailure.
        expect_true(flushed.ok() || (flushed.status().code() != mirador::ErrorCode::kInvalidArgument &&
                                     flushed.status().code() != mirador::ErrorCode::kUnsupportedFormat),
                    "full-view prior passes validation and ends in a documented outcome");
        if (flushed.ok()) {
            expect_true(inside_image(flushed.value().bounds, k_base_width, k_base_height),
                        "decoded bounds stay fully inside the prepared image");
        }
    }
}

// 7. The frozen validation matrix (contract blocks 4/6).
void check_validation_matrix(const std::filesystem::path& dir) {
    auto created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    expect_true(created.ok(), "backend loads for the validation matrix");
    if (!created.ok()) {
        return;
    }
    auto backend = std::move(created).take_value();
    const RgbImage frame = make_frame(k_base_width, k_base_height, 0);
    const mirador::ImageView view = view_of(frame);
    const mirador::RectF valid{16.0F, 16.0F, 8.0F, 8.0F};

    mirador::ImageView null_view = view;
    null_view.data = nullptr;
    mirador::ImageView zero_view = view;
    zero_view.width = 0;

    const std::vector<std::pair<std::string, mirador::RectF>> bad_bounds = {
        {"nan x", mirador::RectF{std::nanf(""), 3.0F, 8.0F, 8.0F}},
        {"nan width", mirador::RectF{16.0F, 3.0F, std::nanf(""), 8.0F}},
        {"inf height", mirador::RectF{16.0F, 3.0F, 8.0F, std::numeric_limits<float>::infinity()}},
        {"zero area", mirador::RectF{16.0F, 16.0F, 0.0F, 8.0F}},
        {"negative origin", mirador::RectF{-0.5F, 16.0F, 8.0F, 8.0F}},
        {"past right edge", mirador::RectF{41.0F, 16.0F, 8.0F, 8.0F}},
        {"past bottom edge", mirador::RectF{16.0F, 29.0F, 8.0F, 8.0F}},
    };
    for (const auto& [name, bounds] : bad_bounds) {
        const auto initialized = backend.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context());
        expect_true(!initialized.ok() && initialized.status().code() == mirador::ErrorCode::kInvalidArgument,
                    "initialize rejects " + name + " with kInvalidArgument");
    }
    const auto null_init = backend.initialize(null_view, mirador::TrackerInitRequest{valid, {}}, clean_context());
    expect_code(null_init.status(), mirador::ErrorCode::kInvalidArgument, "initialize rejects a null-plane view");
    const auto zero_init = backend.initialize(zero_view, mirador::TrackerInitRequest{valid, {}}, clean_context());
    expect_code(zero_init.status(), mirador::ErrorCode::kInvalidArgument, "initialize rejects a zero-width view");

    // Undeclared formats are gated by accepted_formats (never converted).
    mirador::ImageView gray_view = view;
    gray_view.format = mirador::PixelFormat::kGray8;
    const auto gray_init = backend.initialize(gray_view, mirador::TrackerInitRequest{valid, {}}, clean_context());
    expect_code(gray_init.status(), mirador::ErrorCode::kUnsupportedFormat, "kGray8 -> kUnsupportedFormat");
    mirador::ImageView bgr_view = view;
    bgr_view.format = mirador::PixelFormat::kBgr8;
    const auto bgr_init = backend.initialize(bgr_view, mirador::TrackerInitRequest{valid, {}}, clean_context());
    expect_code(bgr_init.status(), mirador::ErrorCode::kUnsupportedFormat, "kBgr8 -> kUnsupportedFormat");

    auto initialized = backend.initialize(view, mirador::TrackerInitRequest{valid, {}}, clean_context());
    if (!initialized.ok()) {
        expect_true(false, "validation-matrix session initializes");
        return;
    }
    mirador::TrackerSession& session = *initialized.value();
    for (const auto& [name, bounds] : bad_bounds) {
        const auto updated = session.update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
        expect_true(!updated.ok() && updated.status().code() == mirador::ErrorCode::kInvalidArgument,
                    "update rejects " + name + " with kInvalidArgument");
    }
    const auto gray_update = session.update(gray_view, mirador::TrackerUpdateRequest{valid, {}}, clean_context());
    expect_code(gray_update.status(), mirador::ErrorCode::kUnsupportedFormat, "update on kGray8 -> kUnsupportedFormat");
}

// 8. The frozen ordering: validation precedes cancellation (block 4).
void check_frozen_ordering(const std::filesystem::path& dir) {
    auto created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    expect_true(created.ok(), "backend loads for the frozen-ordering checks");
    if (!created.ok()) {
        return;
    }
    auto backend = std::move(created).take_value();
    const RgbImage frame = make_frame(k_base_width, k_base_height, 0);
    const mirador::ImageView view = view_of(frame);
    const mirador::RectF valid{16.0F, 16.0F, 8.0F, 8.0F};
    const mirador::RectF malformed{60.0F, 16.0F, 8.0F, 8.0F};

    const auto malformed_init = backend.initialize(view, mirador::TrackerInitRequest{malformed, {}}, hostile_context());
    expect_code(malformed_init.status(), mirador::ErrorCode::kInvalidArgument,
                "malformed initialize on a cancelled+expired context still reports kInvalidArgument");
    const auto cancelled_init = backend.initialize(view, mirador::TrackerInitRequest{valid, {}}, cancelled_context());
    expect_code(cancelled_init.status(), mirador::ErrorCode::kCancelled,
                "valid initialize on a cancelled context -> kCancelled");
    const auto timed_out_init =
        backend.initialize(view, mirador::TrackerInitRequest{valid, {}}, expired_deadline_context());
    expect_code(timed_out_init.status(), mirador::ErrorCode::kTimeout,
                "valid initialize past the deadline -> kTimeout");

    auto initialized = backend.initialize(view, mirador::TrackerInitRequest{valid, {}}, clean_context());
    if (!initialized.ok()) {
        expect_true(false, "frozen-ordering session initializes");
        return;
    }
    mirador::TrackerSession& session = *initialized.value();
    const auto malformed_update = session.update(
        view, mirador::TrackerUpdateRequest{mirador::RectF{std::nanf(""), 0.0F, 8.0F, 8.0F}, {}}, hostile_context());
    expect_code(malformed_update.status(), mirador::ErrorCode::kInvalidArgument,
                "malformed update on a cancelled+expired context still reports kInvalidArgument");
    const auto cancelled_update = session.update(view, mirador::TrackerUpdateRequest{valid, {}}, cancelled_context());
    expect_code(cancelled_update.status(), mirador::ErrorCode::kCancelled,
                "valid update on a cancelled context -> kCancelled");
    const auto timed_out_update =
        session.update(view, mirador::TrackerUpdateRequest{valid, {}}, expired_deadline_context());
    expect_code(timed_out_update.status(), mirador::ErrorCode::kTimeout, "valid update past the deadline -> kTimeout");
}

// 9. kCancelled/kTimeout/kInvalidArgument/kUnsupportedFormat never advance the
// session (all-or-nothing, block 3); the reference is an error-free session.
void check_all_or_nothing(const std::filesystem::path& dir) {
    auto created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    expect_true(created.ok(), "backend loads for the all-or-nothing checks");
    if (!created.ok()) {
        return;
    }
    auto backend = std::move(created).take_value();
    const RgbImage frame = make_frame(k_base_width, k_base_height, 0);
    const mirador::ImageView view = view_of(frame);
    const mirador::RectF prior{16.0F, 16.0F, 8.0F, 8.0F};

    auto reference = backend.initialize(view, mirador::TrackerInitRequest{prior, {}}, clean_context());
    if (!reference.ok()) {
        expect_true(false, "reference session initializes");
        return;
    }
    const auto ref_first = reference.value()->update(view, mirador::TrackerUpdateRequest{prior, {}}, clean_context());
    const auto ref_second = reference.value()->update(view, mirador::TrackerUpdateRequest{prior, {}}, clean_context());
    if (!ref_first.ok() || !ref_second.ok()) {
        expect_true(false, "reference sequence updates");
        return;
    }

    auto initialized = backend.initialize(view, mirador::TrackerInitRequest{prior, {}}, clean_context());
    if (!initialized.ok()) {
        expect_true(false, "all-or-nothing session initializes");
        return;
    }
    mirador::TrackerSession& session = *initialized.value();
    const auto first = session.update(view, mirador::TrackerUpdateRequest{prior, {}}, clean_context());
    if (!first.ok()) {
        expect_true(false, "all-or-nothing first update succeeds");
        return;
    }
    expect_true(first.value() == ref_first.value(), "clean first update matches the reference");

    mirador::ImageView gray_view = view;
    gray_view.format = mirador::PixelFormat::kGray8;
    static_cast<void>(session.update(view, mirador::TrackerUpdateRequest{prior, {}}, cancelled_context()));
    static_cast<void>(session.update(view, mirador::TrackerUpdateRequest{prior, {}}, expired_deadline_context()));
    static_cast<void>(session.update(
        view, mirador::TrackerUpdateRequest{mirador::RectF{std::nanf(""), 0.0F, 8.0F, 8.0F}, {}}, clean_context()));
    static_cast<void>(session.update(gray_view, mirador::TrackerUpdateRequest{prior, {}}, clean_context()));

    const auto second = session.update(view, mirador::TrackerUpdateRequest{prior, {}}, clean_context());
    if (!second.ok()) {
        expect_true(false, "all-or-nothing second update succeeds");
        return;
    }
    expect_true(second.value() == ref_second.value(),
                "error attempts advanced nothing: the next success replays the reference sequence value");
}

// 10. Per-sequence bit determinism (block 8): identical initialize inputs plus
// identical frame sequences produce bit-identical result sequences across
// sessions and backend instances.
void check_per_sequence_determinism(const std::filesystem::path& dir) {
    const RgbImage frame_a = make_frame(k_base_width, k_base_height, 0);
    const RgbImage frame_b = make_frame(k_matrix_width, k_matrix_height, 0);
    const mirador::RectF prior{16.0F, 16.0F, 8.0F, 8.0F};

    std::vector<std::vector<mirador::TrackerUpdateResult>> sequences;
    for (int run = 0; run < 3; ++run) {
        auto created =
            mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
        if (!created.ok()) {
            expect_true(false, "determinism backend " + std::to_string(run) + " loads");
            return;
        }
        auto backend = std::move(created).take_value();
        auto initialized =
            backend.initialize(view_of(frame_a), mirador::TrackerInitRequest{prior, {}}, clean_context());
        if (!initialized.ok()) {
            expect_true(false, "determinism session " + std::to_string(run) + " initializes");
            return;
        }
        std::vector<mirador::TrackerUpdateResult> sequence;
        for (const RgbImage* frame : {&frame_a, &frame_b, &frame_a}) {
            const auto updated =
                initialized.value()->update(view_of(*frame), mirador::TrackerUpdateRequest{prior, {}}, clean_context());
            if (!updated.ok()) {
                expect_true(false, "determinism update on run " + std::to_string(run));
                return;
            }
            sequence.push_back(updated.value());
        }
        sequences.push_back(std::move(sequence));
    }
    for (size_t step = 0; step < 3U; ++step) {
        expect_true(sequences[0][step] == sequences[1][step] && sequences[1][step] == sequences[2][step],
                    "step " + std::to_string(step) + " is bit-identical across sessions and backends");
    }
    expect_true(!(sequences[0][0] == sequences[0][1]), "the sequence genuinely moves (state advanced between updates)");
}

// 11. Byte budget (RULE-06): a budget below one template crop fails
// initialize; a budget that fits initialize but not one search crop fails the
// first update — both loudly with kBudgetExceeded.
void check_work_budget(const std::filesystem::path& dir) {
    const RgbImage frame = make_frame(k_base_width, k_base_height, 0);
    const mirador::RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    auto starved_created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, 3000));
    expect_true(starved_created.ok(), "starved backend loads (budget checks are per call)");
    if (starved_created.ok()) {
        auto starved = std::move(starved_created).take_value();
        const auto initialized =
            starved.initialize(view_of(frame), mirador::TrackerInitRequest{bounds, {}}, clean_context());
        expect_code(initialized.status(), mirador::ErrorCode::kBudgetExceeded,
                    "budget below one template crop -> initialize fails with kBudgetExceeded");
    }

    auto tight_created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, 8000));
    expect_true(tight_created.ok(), "tight backend loads");
    if (tight_created.ok()) {
        auto tight = std::move(tight_created).take_value();
        auto initialized = tight.initialize(view_of(frame), mirador::TrackerInitRequest{bounds, {}}, clean_context());
        expect_true(initialized.ok(), "initialize fits the 8000-byte budget");
        if (initialized.ok()) {
            const auto updated =
                initialized.value()->update(view_of(frame), mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
            expect_code(updated.status(), mirador::ErrorCode::kBudgetExceeded,
                        "budget below one search crop -> update fails with kBudgetExceeded");
        }
    }
}

// 12. Failure is visible, never stale (block 3): broken-model variants fail
// loudly at the right stage and never fabricate a result.
void check_backend_failure_visibility(const std::filesystem::path& models_root) {
    const RgbImage frame = make_frame(k_base_width, k_base_height, 0);
    const mirador::ImageView view = view_of(frame);
    const mirador::RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    // No usable cell (all-zero box biases): the response scan rejects every
    // cell -> kBackendFailure on update, deterministically on every retry.
    auto no_cell_created = mirador::integrations::NanoTrackerBackend::create(
        make_options(models_root / "nocell", 0.0F, k_default_work_budget));
    expect_true(no_cell_created.ok(), "no-usable-cell model loads (initialize only runs the backbone)");
    if (no_cell_created.ok()) {
        auto no_cell = std::move(no_cell_created).take_value();
        auto initialized = no_cell.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context());
        expect_true(initialized.ok(), "no-usable-cell initialize succeeds (head not yet exercised)");
        if (initialized.ok()) {
            const auto updated =
                initialized.value()->update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
            expect_code(updated.status(), mirador::ErrorCode::kBackendFailure, "no usable cell -> kBackendFailure");
            const auto retried =
                initialized.value()->update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
            expect_code(retried.status(), mirador::ErrorCode::kBackendFailure,
                        "the failure is visible again on retry (never a stale success)");
        }
    }

    // Degenerate off-image box: the decode clamp leaves nothing.
    auto off_created = mirador::integrations::NanoTrackerBackend::create(
        make_options(models_root / "offimage", 0.0F, k_default_work_budget));
    expect_true(off_created.ok(), "off-image box model loads");
    if (off_created.ok()) {
        auto off_image = std::move(off_created).take_value();
        auto initialized = off_image.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context());
        if (initialized.ok()) {
            const auto updated =
                initialized.value()->update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
            expect_code(updated.status(), mirador::ErrorCode::kBackendFailure,
                        "degenerate off-image decode -> kBackendFailure");
        }
    }

    // Wrong head output blob names: the wrapper's kInvalidArgument must reach
    // the caller remapped as kBackendFailure (a broken model is never the
    // caller's argument error).
    auto blobs_created = mirador::integrations::NanoTrackerBackend::create(
        make_options(models_root / "wrongblobs", 0.0F, k_default_work_budget));
    expect_true(blobs_created.ok(), "wrong-blob model loads");
    if (blobs_created.ok()) {
        auto wrong_blobs = std::move(blobs_created).take_value();
        auto initialized = wrong_blobs.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context());
        expect_true(initialized.ok(), "wrong-blob initialize succeeds (backbone is intact)");
        if (initialized.ok()) {
            const auto updated =
                initialized.value()->update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
            expect_code(updated.status(), mirador::ErrorCode::kBackendFailure,
                        "unknown head output blob -> kBackendFailure (remapped)");
            expect_true(contains(updated.status().message(), "frozen blob contract"),
                        "the remapped failure names the model-contract violation");
        }
    }

    // 3-channel cls output: the frozen 2-channel shape gate.
    auto cls_created = mirador::integrations::NanoTrackerBackend::create(
        make_options(models_root / "wrongcls", 0.0F, k_default_work_budget));
    expect_true(cls_created.ok(), "wrong-cls model loads");
    if (cls_created.ok()) {
        auto wrong_cls = std::move(cls_created).take_value();
        auto initialized = wrong_cls.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context());
        if (initialized.ok()) {
            const auto updated =
                initialized.value()->update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
            expect_code(updated.status(), mirador::ErrorCode::kBackendFailure,
                        "3-channel cls output -> kBackendFailure (shape gate)");
        }
    }
}

// 13-14. Pixel sensitivity and the coordinate matrix (DOD-03): the decode
// follows the presented bytes inside the search crop and only those — stride
// padding and rotation metadata never change a bit.
void check_pixel_sensitivity_and_coordinates(const std::filesystem::path& dir) {
    auto created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    expect_true(created.ok(), "pixel-sensitive backend loads");
    if (!created.ok()) {
        return;
    }
    auto backend = std::move(created).take_value();

    const RgbImage base = make_frame(k_matrix_width, k_matrix_height, 0);
    // Pixel-model wiring: the winner cell is the maximum-R 4x4 block of the
    // STORED template (cls branch), and the winner's ltrb tracks its search
    // patch's mean R (box branch, +-1/64). Two independent probes:
    //  - a uniform +50 red shift over the whole search crop (prepared
    //    4..36 x 4..36) leaves every penalty unchanged but shifts the decoded
    //    box center by -0.5 px (update frame);
    //  - a +150 red block at prepared (24..28)x(24..28) = template block
    //    (3,3) makes that cell the strict maximum -> the winner flips
    //    (initialize frame).
    RgbImage uniform_shift = base;
    const int32_t crop_bottom = std::min(int32_t{36}, k_matrix_height);  // crop bottom rows fall off the image
    for (int32_t y = 4; y < crop_bottom; ++y) {
        for (int32_t x = 4; x < 36; ++x) {
            bump_red_pixel(uniform_shift, x, y, 50);
        }
    }
    RgbImage winner_flip = base;
    for (int32_t y = 24; y < 28; ++y) {
        for (int32_t x = 24; x < 28; ++x) {
            bump_red_pixel(winner_flip, x, y, 150);
        }
    }
    RgbImage out_crop = base;
    bump_red_pixel(out_crop, 44, 30, 100);

    const mirador::RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    // Initialize on `init_frame`, update on `update_frame`, both presented with
    // `rotation` metadata over `padding` stride bytes.
    const auto run_once = [&](const RgbImage& init_frame, const RgbImage& update_frame, mirador::Rotation rotation,
                              int64_t padding, mirador::TrackerUpdateResult& result) {
        const RgbImage padded_init = repad(init_frame, padding);
        const RgbImage padded_update = repad(update_frame, padding);
        auto initialized = backend.initialize(view_of(padded_init, rotation), mirador::TrackerInitRequest{bounds, {}},
                                              clean_context());
        if (!initialized.ok()) {
            return false;
        }
        const auto updated = initialized.value()->update(view_of(padded_update, rotation),
                                                         mirador::TrackerUpdateRequest{bounds, {}}, clean_context());
        if (!updated.ok()) {
            return false;
        }
        result = updated.value();
        return true;
    };

    mirador::TrackerUpdateResult reference{};
    expect_true(run_once(base, base, mirador::Rotation::k0, 0, reference), "pixel-model reference update succeeds");

    mirador::TrackerUpdateResult shifted_box{};
    expect_true(run_once(base, uniform_shift, mirador::Rotation::k0, 0, shifted_box),
                "search-crop-changed update succeeds");
    expect_true(!(shifted_box == reference), "a uniform in-crop byte shift moves the decoded box");

    mirador::TrackerUpdateResult flipped{};
    expect_true(run_once(winner_flip, base, mirador::Rotation::k0, 0, flipped), "winner-flip update succeeds");
    expect_true(!(flipped == reference), "the initialize frame's template bytes decide the winner cell");

    mirador::TrackerUpdateResult untouched{};
    expect_true(run_once(base, out_crop, mirador::Rotation::k0, 0, untouched), "out-of-crop-changed update succeeds");
    expect_true(untouched == reference, "an out-of-crop byte change never reaches the decode");

    // DOD-03 matrix: identical presented content is bit-identical under every
    // rotation metadata value and stride padding.
    for (const mirador::Rotation rotation :
         {mirador::Rotation::k0, mirador::Rotation::k90, mirador::Rotation::k180, mirador::Rotation::k270}) {
        for (const int64_t padding : {int64_t{0}, int64_t{16}}) {
            mirador::TrackerUpdateResult cell{};
            const bool ok = run_once(base, base, rotation, padding, cell);
            expect_true(ok, "matrix cell runs (rotation " + std::to_string(static_cast<int>(rotation)) + ", padding " +
                                std::to_string(padding) + ")");
            if (ok) {
                expect_true(cell == reference, "matrix cell is bit-identical to the reference (rotation " +
                                                   std::to_string(static_cast<int>(rotation)) + ", padding " +
                                                   std::to_string(padding) + ")");
            }
        }
    }

    // Mean-pad path: a corner target crops past the view and must still
    // initialize; its update ends in a documented outcome either way.
    auto corner = backend.initialize(
        view_of(base), mirador::TrackerInitRequest{mirador::RectF{0.0F, 0.0F, 8.0F, 8.0F}, {}}, clean_context());
    expect_true(corner.ok(), "corner initialize succeeds (crop samples are mean-padded outside the view)");
    if (corner.ok()) {
        const auto updated = corner.value()->update(
            view_of(base), mirador::TrackerUpdateRequest{mirador::RectF{0.0F, 0.0F, 8.0F, 8.0F}, {}}, clean_context());
        expect_true(updated.ok() || updated.status().code() == mirador::ErrorCode::kBackendFailure,
                    "corner update ends in a documented outcome (success or loud kBackendFailure)");
        if (updated.ok()) {
            expect_true(inside_image(updated.value().bounds, k_matrix_width, k_matrix_height),
                        "corner update bounds stay inside the image");
        }
    }
}

// 15. Input immutability: no call path ever writes to the caller's buffer
// (content and stride padding), including error paths.
void check_input_immutability(const std::filesystem::path& dir) {
    auto created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    expect_true(created.ok(), "backend loads for the immutability check");
    if (!created.ok()) {
        return;
    }
    auto backend = std::move(created).take_value();
    const RgbImage frame = make_frame(k_base_width, k_base_height, 16);
    const uint64_t before = buffer_hash(frame);
    const mirador::ImageView view = view_of(frame);
    const mirador::RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};

    static_cast<void>(backend.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context()));
    auto initialized = backend.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context());
    if (initialized.ok()) {
        mirador::TrackerSession& session = *initialized.value();
        static_cast<void>(session.update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context()));
        static_cast<void>(session.update(
            view, mirador::TrackerUpdateRequest{mirador::RectF{std::nanf(""), 0.0F, 1.0F, 1.0F}, {}}, clean_context()));
        static_cast<void>(session.update(view, mirador::TrackerUpdateRequest{bounds, {}}, cancelled_context()));
        static_cast<void>(session.update(view, mirador::TrackerUpdateRequest{bounds, {}}, expired_deadline_context()));
    }
    expect_true(buffer_hash(frame) == before, "input buffer (with stride padding) is byte-identical after every path");
}

// 16. Privacy (RULE-10/DOD-06): nothing is written around the run and no
// pixel-borne marker surfaces in any produced Status message.
void check_privacy(std::vector<std::string>& messages, const std::filesystem::path& dir) {
    const auto before = snapshot_own_temp_entries();

    auto created = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, k_default_work_budget));
    expect_true(created.ok(), "privacy backend loads");
    if (!created.ok()) {
        return;
    }
    auto backend = std::move(created).take_value();
    // The marker flows through the pixels; it must never surface in any
    // Status message (diagnostics are ids, counts and short messages).
    RgbImage frame = make_frame(k_base_width, k_base_height, 0);
    const std::string marker = "MIRADOR-NT-PIXEL-MARKER";
    for (size_t index = 0; index < marker.size(); ++index) {
        frame.pixels[index + 3U] = static_cast<std::byte>(marker[index]);
    }
    const mirador::ImageView view = view_of(frame);
    const mirador::RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    const auto record = [&messages](const auto& result) {
        if (!result.ok()) {
            messages.push_back(result.status().message());
        }
    };

    record(backend.initialize(view, mirador::TrackerInitRequest{bounds, {}}, cancelled_context()));
    record(backend.initialize(view, mirador::TrackerInitRequest{mirador::RectF{60.0F, 0.0F, 8.0F, 8.0F}, {}},
                              clean_context()));
    auto initialized = backend.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context());
    if (initialized.ok()) {
        mirador::TrackerSession& session = *initialized.value();
        record(session.update(view, mirador::TrackerUpdateRequest{bounds, {}}, clean_context()));
        record(session.update(view, mirador::TrackerUpdateRequest{bounds, {}}, cancelled_context()));
        record(session.update(view, mirador::TrackerUpdateRequest{mirador::RectF{0.0F, 0.0F, 8.0F, 8.0F}, {}},
                              clean_context()));
        record(session.update(view, mirador::TrackerUpdateRequest{bounds, {}}, expired_deadline_context()));
    }
    // Starved-budget initialize adds the kBudgetExceeded message to the pool.
    auto starved = mirador::integrations::NanoTrackerBackend::create(make_options(dir, 0.0F, 3000));
    if (starved.ok()) {
        auto starved_backend = std::move(starved).take_value();
        record(starved_backend.initialize(view, mirador::TrackerInitRequest{bounds, {}}, clean_context()));
    }

    expect_true(messages.size() >= 6U, "the privacy run harvested error messages from every error path");
    for (const std::string& message : messages) {
        expect_true(!contains(message, marker), "status message stays free of the pixel marker");
    }

    const auto after = snapshot_own_temp_entries();
    expect_true(after.size() == before.size(), "no mirador-prefixed temp entries appeared around the run");
}

}  // namespace

int main() {
    const std::filesystem::path dir = make_temp_dir();
    if (dir.empty()) {
        std::printf("FAIL: could not create a unique temp directory\n");
        return 1;
    }
    std::printf("ok: synthetic model root %s\n", dir.string().c_str());

    bool models_ok = true;
    for (const std::string_view subdirectory : {"constant", "pixel", "nocell", "offimage", "wrongblobs", "wrongcls"}) {
        std::error_code create_ec;
        models_ok = models_ok && std::filesystem::create_directory(dir / subdirectory, create_ec);
    }
    models_ok = models_ok && write_backbone(dir / "constant", false) &&
                write_head(dir / "constant", HeadVariant::kConstant) && write_backbone(dir / "pixel", true) &&
                write_head(dir / "pixel", HeadVariant::kPixel) && write_backbone(dir / "nocell", false) &&
                write_head(dir / "nocell", HeadVariant::kNoUsableCell) && write_backbone(dir / "offimage", false) &&
                write_head(dir / "offimage", HeadVariant::kOffImage) && write_backbone(dir / "wrongblobs", false) &&
                write_head(dir / "wrongblobs", HeadVariant::kWrongBlobs) && write_backbone(dir / "wrongcls", false) &&
                write_head(dir / "wrongcls", HeadVariant::kWrongCls);
    expect_true(models_ok, "all synthetic model variants written");
    if (models_ok) {
        check_factory_and_info(dir / "constant");
        check_factory_errors(dir / "constant");
        check_wrapper_constant_head(dir / "constant");
        check_wrapper_pixel_layout(dir / "pixel");
        check_wrapper_argument_errors(dir / "constant");
        check_frozen_decode(dir / "constant");
        check_validation_matrix(dir / "constant");
        check_frozen_ordering(dir / "constant");
        check_all_or_nothing(dir / "constant");
        check_per_sequence_determinism(dir / "constant");
        check_work_budget(dir / "constant");
        check_backend_failure_visibility(dir);
        check_pixel_sensitivity_and_coordinates(dir / "pixel");
        check_input_immutability(dir / "pixel");
        std::vector<std::string> messages;
        check_privacy(messages, dir / "nocell");
    }

    std::error_code cleanup_ec;
    static_cast<void>(std::filesystem::remove_all(dir, cleanup_ec));
    if (cleanup_ec) {
        std::printf("warning: temp cleanup of %s failed: %s\n", dir.string().c_str(), cleanup_ec.message().c_str());
    }

    if (g_failed_checks != 0) {
        std::printf("nanotrack smoke: FAIL (%d failed check(s))\n", g_failed_checks);
        return 1;
    }
    std::printf(
        "nanotrack smoke: PASS (decode, prior ownership, validation, ordering, budget, failure visibility, "
        "determinism, coordinates, immutability, privacy)\n");
    return 0;
}
