// M8-01 (DEC-025, ocr-budget-protocol.md): interactive OCR budget benchmark
// harness. Reproduces the measurement side of the OCR budget protocol WITHOUT
// weights: deterministic synthetic dense/sparse/long-line scenes, the frozen
// baseline path (all-or-nothing PpOcrBackend) and the budgeted path
// (PpOcrBudgetedBackend through PerceptionSession::run_ocr_budgeted), stage
// timing decomposition, nearest-rank p50/p95/max, per-call no-content CSV rows
// and explicit gate reporting. Model files are passed by explicit path; a run
// without them exits non-zero (missing data is never reported as a pass).
//
// Scene caveats (protocol section 1): the in-repo generators draw pseudo-glyph
// patterns for pressure reproduction only — they are NOT readable text, so a
// passing run here does not satisfy the "readable line" gates of AC8-02; those
// need the real captured patterns/annotation data provided out of band.

#include "ppocr_backend.hpp"
#include "ppocr_budgeted.hpp"

#include <mirador/cache_policy.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kSceneWidth = 1600;
constexpr int32_t kSceneHeight = 1000;

// Deterministic LCG scene generator: identical inputs on every run/platform
// ordering considered, no randomness, no external data.
class SceneGenerator {
public:
    explicit SceneGenerator(uint64_t seed) noexcept : state_(seed) {}

    uint32_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state_ >> 33);
    }

private:
    uint64_t state_;
};

struct LineSpec {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

/// Grid of synthetic text lines: `columns` x `rows` line slots with jittered
/// positions, each drawn as dark pseudo-glyph blocks on a light background.
std::vector<LineSpec> grid_lines(int32_t columns, int32_t rows, uint64_t seed) {
    SceneGenerator rng(seed);
    std::vector<LineSpec> lines;
    lines.reserve(static_cast<size_t>(columns) * static_cast<size_t>(rows));
    const int32_t slot_w = (kSceneWidth - 40) / columns;
    const int32_t slot_h = (kSceneHeight - 40) / rows;
    for (int32_t row = 0; row < rows; ++row) {
        for (int32_t column = 0; column < columns; ++column) {
            LineSpec line;
            line.width = slot_w * 2 / 3 + static_cast<int32_t>(rng.next() % 32U);
            line.height = 18 + static_cast<int32_t>(rng.next() % 14U);
            line.x = 20 + column * slot_w + static_cast<int32_t>(rng.next() % 16U);
            line.y = 20 + row * slot_h + static_cast<int32_t>(rng.next() % 12U);
            lines.push_back(line);
        }
    }
    return lines;
}

/// A few very long lines (worst-case dynamic rec width, unit calibration).
std::vector<LineSpec> long_lines(int32_t count, uint64_t seed) {
    SceneGenerator rng(seed);
    std::vector<LineSpec> lines;
    lines.reserve(static_cast<size_t>(count));
    for (int32_t index = 0; index < count; ++index) {
        LineSpec line;
        line.width = 1400 + static_cast<int32_t>(rng.next() % 120U);
        line.height = 20 + static_cast<int32_t>(rng.next() % 10U);
        line.x = 30 + static_cast<int32_t>(rng.next() % 40U);
        line.y = 40 + index * (kSceneHeight - 80) / count;
        lines.push_back(line);
    }
    return lines;
}

void draw_line(mirador::ImageBuffer& scene, const LineSpec& line, uint64_t seed) {
    SceneGenerator rng(seed ^ (static_cast<uint64_t>(line.x) << 32U) ^ static_cast<uint64_t>(line.y));
    const int64_t stride = scene.row_stride_bytes();
    std::byte* data = scene.data();
    // Glyph blocks: alternating dark segments with deterministic gaps — dense
    // high-contrast structure so a real det model produces one box per line.
    const int32_t block_count = std::max(1, line.width / 14);
    for (int32_t block = 0; block < block_count; ++block) {
        const int32_t bx = line.x + block * (line.width / block_count);
        const int32_t bw = std::max(2, line.width / block_count - 4);
        const auto shade = static_cast<uint8_t>(30 + rng.next() % 120U);
        for (int32_t y = line.y; y < line.y + line.height; ++y) {
            if (y < 0 || y >= kSceneHeight) {
                continue;
            }
            for (int32_t x = bx; x < bx + bw; ++x) {
                if (x < 0 || x >= kSceneWidth) {
                    continue;
                }
                std::byte* pixel = data + static_cast<int64_t>(y) * stride + static_cast<int64_t>(x) * 3;
                pixel[0] = static_cast<std::byte>(shade);
                pixel[1] = static_cast<std::byte>(shade);
                pixel[2] = static_cast<std::byte>(shade);
            }
        }
    }
}

mirador::Result<mirador::ImageBuffer> make_scene(const std::vector<LineSpec>& lines, uint64_t seed) {
    auto created =
        mirador::ImageBuffer::create(mirador::PixelFormat::kRgb8, kSceneWidth, kSceneHeight, int64_t{64} * 1024 * 1024);
    if (!created.ok()) {
        return created.status();
    }
    mirador::ImageBuffer scene = created.take_value();
    const int64_t stride = scene.row_stride_bytes();
    std::byte* data = scene.data();
    for (int32_t y = 0; y < kSceneHeight; ++y) {
        std::memset(data + static_cast<int64_t>(y) * stride, 235, static_cast<size_t>(stride));
    }
    for (size_t index = 0; index < lines.size(); ++index) {
        draw_line(scene, lines[index], seed + index);
    }
    return scene;
}

std::vector<LineSpec> scenario_lines(const std::string& scenario, int32_t sparse_n) {
    if (scenario == "dense359") {
        // ~359 slots: 19 columns x 19 rows minus the last 2 slots; the real
        // candidate count comes from the det chain and is reported.
        auto lines = grid_lines(19, 19, 20261019ULL);
        lines.resize(359);
        return lines;
    }
    if (scenario == "dense512") {
        auto lines = grid_lines(23, 23, 20261020ULL);
        lines.resize(529);  // > 512 slots so a max_boxes=512 chain can hit the cap
        return lines;
    }
    if (scenario == "longlines") {
        return long_lines(8, 20261021ULL);
    }
    // sparseN
    const int32_t n = sparse_n;
    std::vector<LineSpec> lines;
    lines.reserve(static_cast<size_t>(n));
    for (int32_t index = 0; index < n; ++index) {
        LineSpec line;
        line.width = 320 + static_cast<int32_t>((index * 37) % 320);
        line.height = 24;
        line.x = 60 + (index % 4) * 340;
        line.y = 50 + (index / 4) * 60;
        lines.push_back(line);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Readable caller-provided scenes (M8-06; ocr-budget-protocol.md section 1).
// `render_ocr_scenarios.py` writes scene.tsv / lines.tsv / scene.rgb; the
// drawn-line boxes are the ground-truth geometry for the priority and
// consistency gates below. Scenes are caller paths and never enter the repo.

struct SceneLine {
    mirador::RectF bounds{};
    std::string text;
    bool priority = false;  ///< designated caller-priority line (lines.tsv flag)
};

struct LoadedScene {
    mirador::ImageBuffer image;
    std::vector<SceneLine> lines;
    int32_t priority_count = 0;
    std::string scenario;  ///< gate class from scene.tsv (empty for older scenes)
};

std::string path_in(const std::string& dir, const std::string& file) {
    return dir + "/" + file;
}

mirador::Result<std::vector<std::byte>> read_raw_bytes(const std::string& path, size_t bytes) {
    std::ifstream raw(path, std::ios::binary);
    if (!static_cast<bool>(raw)) {
        return mirador::Status{mirador::ErrorCode::kBackendUnavailable, "cannot open " + path};
    }
    std::vector<std::byte> data(bytes);
    raw.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(bytes));
    if (static_cast<size_t>(raw.gcount()) != bytes) {
        return mirador::Status{mirador::ErrorCode::kInvalidArgument, "truncated scene file: " + path};
    }
    return data;
}

/// Parses the single scene.tsv row (width, height, line_count[, scenario]);
/// line_count may be 0 (empty sparse scenes are a legal scenario). The
/// scenario name is the gate class source of truth (IVA round 1, defect 4);
/// scenes rendered by older renderer versions may omit it.
mirador::Result<std::tuple<int32_t, int32_t, int32_t, std::string>> parse_scene_row(const std::string& row) {
    std::istringstream fields(row);
    std::string width_field;
    std::string height_field;
    std::string count_field;
    std::string scenario_field;
    if (!std::getline(fields, width_field, '\t') || !std::getline(fields, height_field, '\t') ||
        !std::getline(fields, count_field, '\t')) {
        return mirador::Status{mirador::ErrorCode::kInvalidArgument, "malformed scene.tsv row"};
    }
    std::getline(fields, scenario_field, '\t');
    const int32_t width = std::atoi(width_field.c_str());
    const int32_t height = std::atoi(height_field.c_str());
    const int32_t count = std::atoi(count_field.c_str());
    if (width <= 0 || height <= 0 || count < 0 || width > 100000 || height > 100000) {
        return mirador::Status{mirador::ErrorCode::kInvalidArgument, "out-of-range scene metadata"};
    }
    return std::make_tuple(width, height, count, scenario_field);
}

/// Parses one lines.tsv row (id, x, y, w, h, priority, text).
mirador::Result<bool> parse_scene_line_row(const std::string& row, int32_t expected_id, SceneLine& line) {
    std::istringstream fields(row);
    std::string id_field;
    std::string x_field;
    std::string y_field;
    std::string w_field;
    std::string h_field;
    std::string priority_field;
    std::string text_field;
    if (!std::getline(fields, id_field, '\t') || !std::getline(fields, x_field, '\t') ||
        !std::getline(fields, y_field, '\t') || !std::getline(fields, w_field, '\t') ||
        !std::getline(fields, h_field, '\t') || !std::getline(fields, priority_field, '\t') ||
        !std::getline(fields, text_field, '\t')) {
        return mirador::Status{mirador::ErrorCode::kInvalidArgument, "malformed lines.tsv row: " + row};
    }
    const int32_t id = std::atoi(id_field.c_str());
    const int32_t x = std::atoi(x_field.c_str());
    const int32_t y = std::atoi(y_field.c_str());
    const int32_t w = std::atoi(w_field.c_str());
    const int32_t h = std::atoi(h_field.c_str());
    const int32_t priority = std::atoi(priority_field.c_str());
    if (id != expected_id || w <= 0 || h <= 0 || priority < 0 || priority > 1) {
        return mirador::Status{mirador::ErrorCode::kInvalidArgument, "out-of-range line metadata: " + row};
    }
    line.bounds =
        mirador::RectF{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
    line.text = text_field;
    return priority != 0;
}

mirador::Result<LoadedScene> load_scene_dir(const std::string& dir) {
    const std::string manifest_path = path_in(dir, "scene.tsv");
    std::ifstream scene_stream(manifest_path);
    if (!static_cast<bool>(scene_stream)) {
        return mirador::Status{mirador::ErrorCode::kBackendUnavailable, "cannot open " + manifest_path};
    }
    std::string row;
    if (!std::getline(scene_stream, row)) {
        return mirador::Status{mirador::ErrorCode::kInvalidArgument, "scene.tsv is empty"};
    }
    auto meta = parse_scene_row(row);
    if (!meta.ok()) {
        return meta.status();
    }
    const auto [width, height, count, scenario] = std::move(meta).take_value();

    std::vector<SceneLine> lines;
    lines.reserve(static_cast<size_t>(count));
    int32_t priority_count = 0;
    std::ifstream lines_stream(path_in(dir, "lines.tsv"));
    if (!static_cast<bool>(lines_stream)) {
        return mirador::Status{mirador::ErrorCode::kBackendUnavailable, "cannot open " + path_in(dir, "lines.tsv")};
    }
    while (std::getline(lines_stream, row)) {
        if (row.empty()) {
            continue;
        }
        SceneLine line;
        auto priority = parse_scene_line_row(row, static_cast<int32_t>(lines.size()), line);
        if (!priority.ok()) {
            return priority.status();
        }
        line.priority = priority.value();
        priority_count += priority.value() ? 1 : 0;
        lines.push_back(std::move(line));
    }
    if (static_cast<int32_t>(lines.size()) != count) {
        return mirador::Status{
            mirador::ErrorCode::kInvalidArgument,
            "lines.tsv row count " + std::to_string(lines.size()) + " != scene.tsv count " + std::to_string(count)};
    }

    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3U;
    auto raw = read_raw_bytes(path_in(dir, "scene.rgb"), bytes);
    if (!raw.ok()) {
        return raw.status();
    }
    auto buffer = mirador::ImageBuffer::create(mirador::PixelFormat::kRgb8, width, height, int64_t{64} * 1024 * 1024);
    if (!buffer.ok()) {
        return buffer.status();
    }
    mirador::ImageBuffer image = std::move(buffer).take_value();
    std::memcpy(image.data(), raw.value().data(), bytes);
    LoadedScene scene;
    scene.image = std::move(image);
    scene.lines = std::move(lines);
    scene.priority_count = priority_count;
    scene.scenario = scenario;
    return scene;
}

// ---------------------------------------------------------------------------
// CLI

struct Options {
    std::string det_param;
    std::string det_bin;
    std::string rec_param;
    std::string rec_bin;
    std::string charset;
    std::string model_id = "unspecified";
    std::string model_revision = "unspecified";
    std::string scenario = "dense359";
    std::string mode = "budgeted";  // budgeted | baseline
    std::string csv_path;
    std::string scene_dir;
    int32_t sparse_n = 32;
    int32_t det_threads = 1;
    int32_t rec_threads = 1;
    int32_t max_boxes = 512;
    int32_t k = 64;
    int32_t warmup = 2;
    int32_t iterations = 20;
    int32_t expect_boxes = 0;          // 0 disables the exact-candidate-count gate
    int64_t soft_budget_ms = -1;       // < 0 disables
    int64_t finalize_reserve_ms = -1;  // < 0 disables
    bool compare_baseline = false;
};

void print_usage() {
    std::printf(
        "mirador_bench_ocr_budget — M8 interactive OCR budget harness (DEC-025)\n\n"
        "Required model inputs (explicit paths; weights are never fetched):\n"
        "  --det-param PATH --det-bin PATH --rec-param PATH --rec-bin PATH --charset PATH\n"
        "  --model-id ID --model-revision REV   (recorded in every row)\n\n"
        "Scenario:\n"
        "  --scenario dense359|dense512|longlines|sparse  (default dense359; selects\n"
        "               the in-memory generator AND the gate class below)\n"
        "  --sparse-n N                                    (with --scenario sparse)\n"
        "  --scene-dir DIR   readable caller scene from render_ocr_scenarios.py\n"
        "               (scene.tsv/lines.tsv/scene.rgb); overrides the in-memory\n"
        "               generator and enables the M8-06 priority/consistency gates.\n"
        "               --scenario must match the rendered scenario (gate class);\n"
        "               consistency gates apply to budgeted mode only\n"
        "  --expect-boxes N  gate: the det chain must report exactly N candidates\n\n"
        "Configuration:\n"
        "  --mode budgeted|baseline   baseline = all-or-nothing PpOcrBackend\n"
        "               (cache-refresh per call: every call exercises the backend)\n"
        "  --det-threads T --rec-threads T --max-boxes B --k K\n"
        "  --soft-budget-ms MS --finalize-reserve-ms MS  (< 0 = disabled;\n"
        "               enabling the reserve also sets the 5000 ms hard deadline)\n"
        "  --warmup W --iterations N --csv PATH --compare-baseline\n\n"
        "Scene-mode gates (budgeted): every call <= 5000 ms (deadline on), zero\n"
        "text mismatches against an untimed baseline reference pass, >= 32\n"
        "non-empty matched lines for the dense scenarios, all designated\n"
        "priority lines recognized; sparse additionally requires full completion\n"
        "(K >= N, no soft budget) and bbox delta <= 1e-4 px. Failures exit 4 after\n"
        "the CSV is written so the evidence is preserved.\n\n"
        "Exit codes: 0 ok; 2 bad arguments; 3 missing model inputs or scene files;\n"
        "4 gate failure (scenario gate not met, e.g. the 512-box stress scene did\n"
        "not hit the cap); 5 backend failure during measurement.\n");
}

/// Outcome of one option-table scan over argv[i].
enum class ConsumeResult : uint8_t { kConsumed, kNoMatch, kMissingValue };

std::string identity_string(const char* value) {
    return value;
}

int32_t parse_int32(const char* value) {
    return std::atoi(value);
}

int64_t parse_int64(const char* value) {
    return std::atoll(value);
}

/// Scans one option table for argv[i]; on a match, parses and stores the next
/// argument into the bound field and advances `i` past it.
template <typename T, size_t N>
ConsumeResult scan_option_table(char** argv, int& i, int argc, Options& options,
                                const std::array<std::pair<const char*, T Options::*>, N>& table,
                                T (*parse)(const char*)) {
    for (const auto& entry : table) {
        if (std::strcmp(argv[i], entry.first) != 0) {
            continue;
        }
        if (i + 1 >= argc) {
            return ConsumeResult::kMissingValue;
        }
        options.*(entry.second) = parse(argv[++i]);
        return ConsumeResult::kConsumed;
    }
    return ConsumeResult::kNoMatch;
}

const std::array<std::pair<const char*, std::string Options::*>, 11>& string_options() noexcept {
    static const std::array<std::pair<const char*, std::string Options::*>, 11> table = {{
        {"--det-param", &Options::det_param},
        {"--det-bin", &Options::det_bin},
        {"--rec-param", &Options::rec_param},
        {"--rec-bin", &Options::rec_bin},
        {"--charset", &Options::charset},
        {"--model-id", &Options::model_id},
        {"--model-revision", &Options::model_revision},
        {"--scenario", &Options::scenario},
        {"--mode", &Options::mode},
        {"--csv", &Options::csv_path},
        {"--scene-dir", &Options::scene_dir},
    }};
    return table;
}

const std::array<std::pair<const char*, int32_t Options::*>, 8>& int32_options() noexcept {
    static const std::array<std::pair<const char*, int32_t Options::*>, 8> table = {{
        {"--sparse-n", &Options::sparse_n},
        {"--det-threads", &Options::det_threads},
        {"--rec-threads", &Options::rec_threads},
        {"--max-boxes", &Options::max_boxes},
        {"--k", &Options::k},
        {"--warmup", &Options::warmup},
        {"--iterations", &Options::iterations},
        {"--expect-boxes", &Options::expect_boxes},
    }};
    return table;
}

const std::array<std::pair<const char*, int64_t Options::*>, 2>& int64_options() noexcept {
    static const std::array<std::pair<const char*, int64_t Options::*>, 2> table = {{
        {"--soft-budget-ms", &Options::soft_budget_ms},
        {"--finalize-reserve-ms", &Options::finalize_reserve_ms},
    }};
    return table;
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        ConsumeResult result = scan_option_table(argv, i, argc, options, string_options(), identity_string);
        if (result == ConsumeResult::kNoMatch) {
            result = scan_option_table(argv, i, argc, options, int32_options(), parse_int32);
        }
        if (result == ConsumeResult::kNoMatch) {
            result = scan_option_table(argv, i, argc, options, int64_options(), parse_int64);
        }
        if (result == ConsumeResult::kNoMatch) {
            if (std::strcmp(argv[i], "--compare-baseline") == 0) {
                options.compare_baseline = true;
                continue;
            }
            std::printf("unknown argument: %s\n", argv[i]);
            return false;
        }
        if (result == ConsumeResult::kMissingValue) {
            std::printf("missing value for argument: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Measurement

struct CallRecord {
    int32_t index = 0;
    double total_ms = 0.0;
    double det_canvas_ms = 0.0;
    double det_pack_ms = 0.0;
    double det_forward_ms = 0.0;
    double det_post_ms = 0.0;
    double select_ms = 0.0;
    double crop_ms = 0.0;
    double rec_pre_ms = 0.0;
    double rec_forward_ms = 0.0;
    double rec_decode_ms = 0.0;
    double finalize_ms = 0.0;
    int32_t detected = 0;
    int32_t recognized = 0;
    int32_t deferred = 0;
    int32_t forwards = 0;
    int32_t nonempty_lines = 0;
    int32_t stop = 0;  // OcrBatchStopReason value
    bool det_limit_hit = false;
    bool partial = false;
    // Scene-mode consistency columns (all zero without --scene-dir):
    int32_t nonempty_matched = 0;    ///< recognized, non-empty, text == baseline reference
    int32_t mismatched = 0;          ///< recognized lines whose text != baseline reference
    int32_t priority_covered = 0;    ///< designated priority lines with a recognized intersecting line
    double max_bbox_delta_px = 0.0;  ///< max per-component bbox delta vs the reference
    uint64_t content_digest = 0;     ///< FNV-1a over the call's result identity (cross-run equality evidence)
};

double ms_of(int64_t nanoseconds) noexcept {
    return static_cast<double>(nanoseconds) / 1.0e6;
}

double percentile(std::vector<double>& values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<size_t>(std::ceil(p * static_cast<double>(values.size())));
    return values[std::min(values.size(), std::max<size_t>(rank, 1)) - 1];
}

mirador::integrations::PpOcrDetOptions det_options(const Options& options) {
    mirador::integrations::PpOcrDetOptions det;
    det.param_path = options.det_param;
    det.bin_path = options.det_bin;
    det.identity = mirador::integrations::PpOcrModelIdentity{options.model_id + "-det", options.model_revision};
    det.num_threads = options.det_threads;
    det.postprocess.max_boxes = options.max_boxes;
    return det;
}

mirador::integrations::PpOcrRecOptions rec_options(const Options& options) {
    mirador::integrations::PpOcrRecOptions rec;
    rec.param_path = options.rec_param;
    rec.bin_path = options.rec_bin;
    rec.charset_path = options.charset;
    rec.identity = mirador::integrations::PpOcrModelIdentity{options.model_id + "-rec", options.model_revision};
    rec.num_threads = options.rec_threads;
    return rec;
}

}  // namespace

namespace {

constexpr int32_t kBudgetDeadlineMs = 5000;

// ---------------------------------------------------------------------------
// Content digest (cross-run equality evidence for the protocol's "repeated
// non-timed results must be bit-identical" checks). FNV-1a 64 over the call's
// result identity — counts, per-candidate state/geometry/confidence bits and
// text bytes. Text bytes are one-way digested and never printed, so the
// privacy boundary (no OCR content in logs) holds.

uint64_t digest_bytes(uint64_t hash, const void* data, size_t bytes) noexcept {
    const auto* cursor = static_cast<const std::byte*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= static_cast<uint8_t>(cursor[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t digest_uint64(uint64_t hash, uint64_t value) noexcept {
    return digest_bytes(hash, &value, sizeof(value));
}

uint64_t digest_float(uint64_t hash, float value) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return digest_uint64(hash, bits);
}

uint64_t digest_text(uint64_t hash, const std::string& text) noexcept {
    hash = digest_uint64(hash, static_cast<uint64_t>(text.size()));
    return digest_bytes(hash, text.data(), text.size());
}

/// Result identity of one budgeted report.
uint64_t digest_report(const mirador::BudgetedOcrReport& report) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    hash = digest_uint64(hash, static_cast<uint64_t>(report.batch.lines.size()));
    hash = digest_uint64(hash, static_cast<uint64_t>(report.batch.detected_count));
    hash = digest_uint64(hash, static_cast<uint64_t>(report.batch.recognized_count));
    hash = digest_uint64(hash, static_cast<uint64_t>(report.batch.deferred_count));
    hash = digest_uint64(hash, static_cast<uint64_t>(report.batch.rec_forward_count));
    hash = digest_uint64(hash, static_cast<uint64_t>(report.batch.stop_reason));
    hash = digest_uint64(hash, static_cast<uint64_t>(report.batch.completion));
    hash = digest_uint64(hash, report.batch.det_limit_hit ? 1ULL : 0ULL);
    for (const mirador::OcrLineRecord& line : report.batch.lines) {
        hash = digest_uint64(hash, static_cast<uint64_t>(line.candidate_id));
        hash = digest_float(hash, line.bounds.x);
        hash = digest_float(hash, line.bounds.y);
        hash = digest_float(hash, line.bounds.width);
        hash = digest_float(hash, line.bounds.height);
        hash = digest_float(hash, line.det_confidence);
        hash = digest_uint64(hash, static_cast<uint64_t>(line.state));
        hash = digest_uint64(hash, static_cast<uint64_t>(line.execution_order));
        hash = digest_uint64(hash, line.rec_confidence.has_value() ? 1ULL : 0ULL);
        if (line.rec_confidence.has_value()) {
            hash = digest_float(hash, *line.rec_confidence);
        }
        hash = digest_text(hash, line.utf8_text);
    }
    return hash;
}

/// Result identity of one baseline call.
uint64_t digest_regions(const std::vector<mirador::TextRegion>& regions) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    hash = digest_uint64(hash, static_cast<uint64_t>(regions.size()));
    for (const mirador::TextRegion& region : regions) {
        hash = digest_float(hash, region.bounds.x);
        hash = digest_float(hash, region.bounds.y);
        hash = digest_float(hash, region.bounds.width);
        hash = digest_float(hash, region.bounds.height);
        hash = digest_float(hash, region.confidence);
        hash = digest_text(hash, region.utf8_text);
    }
    return hash;
}

/// Runs the all-or-nothing baseline measurement loop; returns an exit code
/// (0 ok) and appends one record per measured call. Every measured call uses
/// kRefresh: with the default kReadWrite policy the first call would populate
/// the capability cache and every later call would time a cache hit instead of
/// the backend (AC8-01 needs real forwards in every cell).
int measure_baseline(mirador::PerceptionSession& session, const mirador::Frame& frame, const Options& options,
                     std::vector<CallRecord>& records) {
    auto backend = mirador::integrations::PpOcrBackend::create(det_options(options), rec_options(options));
    if (!backend.ok()) {
        std::printf("FAIL: baseline backend creation failed: %s\n", backend.status().message().c_str());
        return 3;
    }
    mirador::integrations::PpOcrBackend ocr = std::move(backend).take_value();
    const mirador::OcrRequest request = [] {
        mirador::OcrRequest value;
        value.cache_policy = mirador::CachePolicy::kRefresh;
        return value;
    }();
    for (int32_t call = 0; call < options.warmup + options.iterations; ++call) {
        const auto begin = std::chrono::steady_clock::now();
        auto regions = session.run_ocr(frame, &ocr, request);
        const auto end = std::chrono::steady_clock::now();
        if (!regions.ok()) {
            std::printf("FAIL: baseline call %d failed: %s\n", call, regions.status().message().c_str());
            return 5;
        }
        if (call >= options.warmup) {
            CallRecord record;
            record.index = call - options.warmup;
            record.total_ms =
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()) / 1.0e6;
            record.detected = static_cast<int32_t>(regions.value().size());
            record.recognized = static_cast<int32_t>(regions.value().size());
            record.nonempty_lines = static_cast<int32_t>(
                std::count_if(regions.value().begin(), regions.value().end(),
                              [](const mirador::TextRegion& region) { return !region.utf8_text.empty(); }));
            record.content_digest = digest_regions(regions.value());
            records.push_back(record);
        }
    }
    return 0;
}

/// Builds the budgeted request from the CLI options; scene-mode priorities are
/// attached by the caller (they need the loaded scene).
mirador::BudgetedOcrRequest budget_request(const Options& options) {
    mirador::BudgetedOcrRequest request;
    request.max_rec_lines = options.k;
    request.max_side = 0;
    if (options.soft_budget_ms >= 0) {
        request.soft_budget = std::chrono::milliseconds(options.soft_budget_ms);
    }
    if (options.finalize_reserve_ms >= 0) {
        request.finalize_reserve = std::chrono::milliseconds(options.finalize_reserve_ms);
    }
    return request;
}

/// True when the two rects share a positive-area intersection.
bool rects_intersect(const mirador::RectF& a, const mirador::RectF& b) noexcept {
    const float w = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const float h = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    return w > 0.0F && h > 0.0F;
}

/// Max per-component |a-b| across the rect fields.
double bbox_delta_px(const mirador::RectF& a, const mirador::RectF& b) noexcept {
    const double dx = std::fabs(static_cast<double>(a.x) - b.x);
    const double dy = std::fabs(static_cast<double>(a.y) - b.y);
    const double dw = std::fabs(static_cast<double>(a.width) - b.width);
    const double dh = std::fabs(static_cast<double>(a.height) - b.height);
    return std::max(std::max(dx, dy), std::max(dw, dh));
}

/// Fills the scene-mode consistency columns of one budgeted call: text and
/// geometry are compared against the untimed baseline reference pass by
/// candidate index (both paths share the same deterministic det prefix; the
/// session recovers both to kOriented).
void fill_consistency(CallRecord& record, const mirador::BudgetedOcrReport& report,
                      const std::vector<mirador::TextRegion>& reference, const LoadedScene& scene) {
    for (const mirador::OcrLineRecord& line : report.batch.lines) {
        if (line.state != mirador::OcrCompletionState::kRecognized) {
            continue;
        }
        if (line.candidate_id < 0 || static_cast<size_t>(line.candidate_id) >= reference.size()) {
            ++record.mismatched;
            continue;
        }
        const mirador::TextRegion& ref = reference[static_cast<size_t>(line.candidate_id)];
        record.max_bbox_delta_px = std::max(record.max_bbox_delta_px, bbox_delta_px(line.bounds, ref.bounds));
        if (line.utf8_text == ref.utf8_text) {
            if (!line.utf8_text.empty()) {
                ++record.nonempty_matched;
            }
        } else {
            ++record.mismatched;
        }
    }
    for (const SceneLine& gt : scene.lines) {
        if (!gt.priority) {
            continue;
        }
        // Priority ROIs arrive in kOriented frame space — the scene's own
        // coordinate space (identity mapping).
        const bool covered =
            std::any_of(report.batch.lines.begin(), report.batch.lines.end(), [&](const mirador::OcrLineRecord& line) {
                return line.state == mirador::OcrCompletionState::kRecognized &&
                       rects_intersect(line.bounds, gt.bounds);
            });
        record.priority_covered += covered ? 1 : 0;
    }
}

CallRecord budget_record(int32_t index, const mirador::integrations::PpOcrBudgetedBackend& backend,
                         const mirador::BudgetedOcrReport& report, double total_ms) {
    const auto& timings = backend.last_stage_timings();
    CallRecord record;
    record.index = index;
    record.total_ms = total_ms;
    record.det_canvas_ms = ms_of(timings.det_canvas_ns);
    record.det_pack_ms = ms_of(timings.det_pack_normalize_ns);
    record.det_forward_ms = ms_of(timings.det_forward_ns);
    record.det_post_ms = ms_of(timings.det_postprocess_ns);
    record.select_ms = ms_of(timings.select_ns);
    record.crop_ms = ms_of(timings.crop_ns);
    record.rec_pre_ms = ms_of(timings.rec_preprocess_ns);
    record.rec_forward_ms = ms_of(timings.rec_forward_ns);
    record.rec_decode_ms = ms_of(timings.rec_decode_ns);
    record.finalize_ms = ms_of(timings.finalize_ns);
    record.detected = report.batch.detected_count;
    record.recognized = report.batch.recognized_count;
    record.deferred = report.batch.deferred_count;
    record.forwards = report.batch.rec_forward_count;
    record.nonempty_lines =
        static_cast<int32_t>(std::count_if(report.batch.lines.begin(), report.batch.lines.end(),
                                           [](const mirador::OcrLineRecord& line) { return !line.utf8_text.empty(); }));
    record.stop = static_cast<int32_t>(report.batch.stop_reason);
    record.det_limit_hit = report.batch.det_limit_hit;
    record.partial = report.batch.completion == mirador::OcrBatchCompletion::kPartial;
    record.content_digest = digest_report(report);
    return record;
}

/// Runs the budgeted measurement loop (session entry timing boundary). With a
/// loaded scene, caller priorities are attached and an untimed baseline
/// reference pass provides the per-candidate texts the consistency gates
/// compare against.
int measure_budgeted(mirador::PerceptionSession& session, const mirador::Frame& frame, const Options& options,
                     const LoadedScene* scene, std::vector<CallRecord>& records) {
    auto backend = mirador::integrations::PpOcrBudgetedBackend::create([&] {
        mirador::integrations::PpOcrBudgetedOptions budgeted;
        budgeted.det = det_options(options);
        budgeted.rec = rec_options(options);
        return budgeted;
    }());
    if (!backend.ok()) {
        std::printf("FAIL: budgeted backend creation failed: %s\n", backend.status().message().c_str());
        return 3;
    }
    mirador::integrations::PpOcrBudgetedBackend ocr = std::move(backend).take_value();
    mirador::BudgetedOcrRequest request = budget_request(options);
    std::vector<mirador::TextRegion> reference;
    if (scene != nullptr) {
        // Only the designated priority lines become caller priorities; with
        // K < N this is what guarantees they are recognized first.
        for (const SceneLine& line : scene->lines) {
            if (!line.priority) {
                continue;
            }
            mirador::OcrPriorityEntry entry;
            entry.roi = line.bounds;
            entry.priority = 1.0F;
            request.priorities.push_back(entry);
        }
    }
    const bool has_deadline = options.finalize_reserve_ms >= 0;

    // Untimed baseline reference pass (kRefresh: a real forward, never a cache
    // read); it also fails the run early when the complete path cannot.
    if (scene != nullptr) {
        mirador::integrations::PpOcrDetOptions reference_det = det_options(options);
        // The all-or-nothing reference must see the full candidate set: the
        // frozen old postprocess fails the whole call (kBudgetExceeded) when
        // boxes exceed its cap, which would break the untimed pass exactly on
        // the overflow scenes the budgeted path is built for (IVA round 1,
        // defect 2). Raise the reference cap to the postprocess domain maximum;
        // the budgeted run keeps the caller's policy and compares per
        // candidate_id against the shared det prefix order.
        reference_det.postprocess.max_boxes = 65535;
        auto reference_backend = mirador::integrations::PpOcrBackend::create(reference_det, rec_options(options));
        if (!reference_backend.ok()) {
            std::printf("FAIL: reference backend creation failed: %s\n", reference_backend.status().message().c_str());
            return 3;
        }
        mirador::integrations::PpOcrBackend ref_ocr = std::move(reference_backend).take_value();
        mirador::OcrRequest ref_request;
        ref_request.cache_policy = mirador::CachePolicy::kRefresh;
        auto regions = session.run_ocr(frame, &ref_ocr, ref_request);
        if (!regions.ok()) {
            std::printf("FAIL: baseline reference pass failed: %s\n", regions.status().message().c_str());
            return 5;
        }
        reference = std::move(regions).take_value();
        std::printf("SUMMARY reference_candidates=%zu scene_lines=%zu priority_entries=%zu\n", reference.size(),
                    scene->lines.size(), request.priorities.size());
    }

    for (int32_t call = 0; call < options.warmup + options.iterations; ++call) {
        mirador::ExecutionContext context;
        if (has_deadline) {
            context.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBudgetDeadlineMs);
        }
        const auto begin = std::chrono::steady_clock::now();
        auto report = session.run_ocr_budgeted(frame, &ocr, request, context);
        const auto end = std::chrono::steady_clock::now();
        if (!report.ok()) {
            std::printf("FAIL: budgeted call %d failed: %s\n", call, report.status().message().c_str());
            return 5;
        }
        if (call >= options.warmup) {
            const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
            CallRecord record =
                budget_record(call - options.warmup, ocr, report.value(), static_cast<double>(elapsed_ns) / 1.0e6);
            if (scene != nullptr) {
                fill_consistency(record, report.value(), reference, *scene);
            }
            records.push_back(record);
        }
    }
    return 0;
}

/// Builds the no-content record of one budgeted call from the stage timings.

void print_last_record(const CallRecord& last) {
    std::printf(
        "SUMMARY last detected=%d recognized=%d deferred=%d forwards=%d nonempty=%d stop=%d "
        "partial=%d det_limit_hit=%d\n",
        last.detected, last.recognized, last.deferred, last.forwards, last.nonempty_lines, last.stop,
        last.partial ? 1 : 0, last.det_limit_hit ? 1 : 0);
}

/// Prints the aggregated no-content summary lines.
void print_summary(const std::vector<CallRecord>& records, const Options& options) {
    std::vector<double> totals;
    totals.reserve(records.size());
    for (const CallRecord& record : records) {
        totals.push_back(record.total_ms);
    }
    std::printf("SUMMARY mode=%s scenario=%s model=%s@%s det_threads=%d rec_threads=%d max_boxes=%d k=%d\n",
                options.mode.c_str(), options.scenario.c_str(), options.model_id.c_str(),
                options.model_revision.c_str(), options.det_threads, options.rec_threads, options.max_boxes, options.k);
    const double max_ms = totals.empty() ? 0.0 : *std::max_element(totals.begin(), totals.end());
    std::printf("SUMMARY calls=%zu p50_ms=%.3f p95_ms=%.3f max_ms=%.3f\n", records.size(), percentile(totals, 0.50),
                percentile(totals, 0.95), max_ms);
    if (!records.empty()) {
        print_last_record(records.back());
        std::printf("SUMMARY content_digest=%016llx (last call; identical across repeated runs)\n",
                    static_cast<unsigned long long>(records.back().content_digest));
    }
}

/// Writes the per-call no-content CSV; returns false when the file cannot be
/// opened. Scene-mode consistency columns are always present (zero without
/// --scene-dir) so downstream cross-checks see one stable schema.
bool write_csv(const std::vector<CallRecord>& records, const std::string& path) {
    FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        std::printf("FAIL: cannot open csv output %s\n", path.c_str());
        return false;
    }
    std::fprintf(out,
                 "index,total_ms,det_canvas_ms,det_pack_ms,det_forward_ms,det_post_ms,select_ms,crop_ms,"
                 "rec_pre_ms,rec_forward_ms,rec_decode_ms,finalize_ms,detected,recognized,deferred,forwards,"
                 "nonempty_lines,stop,partial,det_limit_hit,nonempty_matched,mismatched,priority_covered,"
                 "max_bbox_delta_px,content_digest\n");
    for (const CallRecord& record : records) {
        std::fprintf(out,
                     "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                     "%.6f,%016llx\n",
                     record.index, record.total_ms, record.det_canvas_ms, record.det_pack_ms, record.det_forward_ms,
                     record.det_post_ms, record.select_ms, record.crop_ms, record.rec_pre_ms, record.rec_forward_ms,
                     record.rec_decode_ms, record.finalize_ms, record.detected, record.recognized, record.deferred,
                     record.forwards, record.nonempty_lines, record.stop, record.partial ? 1 : 0,
                     record.det_limit_hit ? 1 : 0, record.nonempty_matched, record.mismatched, record.priority_covered,
                     record.max_bbox_delta_px, static_cast<unsigned long long>(record.content_digest));
    }
    std::fclose(out);
    std::printf("SUMMARY csv=%s\n", path.c_str());
    return true;
}

/// Scenario gates reported explicitly; failures exit 4 (never a silent pass).
/// The readable-text gates live in check_scene_gates; this covers the cap and
/// candidate-count gates that also apply to the in-memory pressure scenes.
int check_scenario_gates(const std::vector<CallRecord>& records, const Options& options) {
    if (records.empty()) {
        std::printf("GATE FAIL: no measured calls recorded\n");
        return 4;
    }
    const CallRecord& last = records.back();
    if (options.expect_boxes > 0 && last.detected != options.expect_boxes) {
        std::printf("GATE FAIL: --expect-boxes %d but the det chain reported %d candidates\n", options.expect_boxes,
                    last.detected);
        return 4;
    }
    if (options.scenario == "dense512" && last.detected < options.max_boxes) {
        std::printf(
            "GATE FAIL: dense512 scene produced %d candidates < max_boxes %d; "
            "the stress gate needs a real 512-candidate chain\n",
            last.detected, options.max_boxes);
        return 4;
    }
    return 0;
}

/// Scene-mode gates (M8-06): AC8-02 time/consistency/priority coverage for the
/// dense scenarios (the >= 32 readable-line requirement is dense-only; the
/// long-lines scenario is unit calibration with ~10 lines), AC8-05
/// full-completion/text/geometry identity for the sparse scenarios. Runs after
/// the CSV is written so failing evidence is preserved.
int check_scene_gates(const std::vector<CallRecord>& records, const Options& options, const LoadedScene& scene) {
    const bool dense = options.scenario == "dense359" || options.scenario == "dense512";
    const bool sparse = options.scenario == "sparse";
    const bool deadline_on = options.finalize_reserve_ms >= 0;
    for (const CallRecord& record : records) {
        if (deadline_on && record.total_ms > static_cast<double>(kBudgetDeadlineMs)) {
            std::printf("GATE FAIL: call %d took %.3f ms > %d ms budget\n", record.index, record.total_ms,
                        kBudgetDeadlineMs);
            return 4;
        }
        if (record.mismatched != 0) {
            std::printf("GATE FAIL: call %d has %d recognized lines that disagree with the baseline reference\n",
                        record.index, record.mismatched);
            return 4;
        }
        if (dense && record.nonempty_matched < 32) {
            std::printf("GATE FAIL: call %d matched only %d non-empty lines (need >= 32)\n", record.index,
                        record.nonempty_matched);
            return 4;
        }
        if (dense && record.priority_covered != scene.priority_count) {
            std::printf("GATE FAIL: call %d covered %d of %d designated priority lines\n", record.index,
                        record.priority_covered, scene.priority_count);
            return 4;
        }
        if (sparse) {
            if (record.max_bbox_delta_px > 1e-4) {
                std::printf("GATE FAIL: call %d bbox delta %.6f px > 1e-4\n", record.index, record.max_bbox_delta_px);
                return 4;
            }
            const bool full_selection = options.k >= record.detected && options.soft_budget_ms < 0;
            if (full_selection && (record.stop != 0 || record.deferred != 0)) {
                std::printf(
                    "GATE FAIL: sparse call %d with K >= N and no soft budget must complete fully "
                    "(stop=%d deferred=%d)\n",
                    record.index, record.stop, record.deferred);
                return 4;
            }
        }
    }
    std::printf("SUMMARY scene_gates=pass scenario=%s records=%zu priority_count=%d\n", options.scenario.c_str(),
                records.size(), scene.priority_count);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 2;
    }
    if (options.det_param.empty() || options.det_bin.empty() || options.rec_param.empty() || options.rec_bin.empty() ||
        options.charset.empty()) {
        std::printf("FAIL: missing model inputs; weights are never fetched or embedded (exit 3)\n");
        print_usage();
        return 3;
    }
    if (options.mode != "budgeted" && options.mode != "baseline") {
        std::printf("FAIL: --mode must be budgeted or baseline\n");
        return 2;
    }

    // Scene: a caller-provided readable scene (M8-06) or the deterministic
    // in-memory pressure generator; both stay in memory, nothing persists.
    LoadedScene loaded;
    const LoadedScene* scene_ptr = nullptr;
    mirador::ImageBuffer generated;
    if (!options.scene_dir.empty()) {
        auto loaded_result = load_scene_dir(options.scene_dir);
        if (!loaded_result.ok()) {
            std::printf("FAIL: cannot load scene dir %s: %s\n", options.scene_dir.c_str(),
                        loaded_result.status().message().c_str());
            return 3;
        }
        loaded = std::move(loaded_result).take_value();
        scene_ptr = &loaded;
        // The rendered scenario is the gate class source of truth (IVA round 1,
        // defect 4): a --scene-dir whose scenario disagrees with --scenario is
        // a caller error, never a silently mis-classified gate.
        if (!loaded.scenario.empty() && loaded.scenario != options.scenario) {
            std::printf("FAIL: --scenario %s does not match the rendered scene scenario %s\n", options.scenario.c_str(),
                        loaded.scenario.c_str());
            return 2;
        }
        std::printf("SUMMARY scene_dir=%s scene_lines=%zu priority_lines=%d\n", options.scene_dir.c_str(),
                    loaded.lines.size(), loaded.priority_count);
    } else {
        const std::vector<LineSpec> specs = scenario_lines(options.scenario, options.sparse_n);
        const uint64_t scene_seed = options.scenario == "dense512" ? 20261020ULL : 20261019ULL;
        auto scene_result = make_scene(specs, scene_seed);
        if (!scene_result.ok()) {
            std::printf("FAIL: scene allocation failed\n");
            return 5;
        }
        generated = scene_result.take_value();
    }
    const mirador::ImageBuffer& scene = scene_ptr != nullptr ? scene_ptr->image : generated;

    mirador::Frame frame;
    frame.image = scene.view();
    frame.sequence = 1;
    frame.source_id = "ocr-budget-bench";
    mirador::PerceptionSession session =
        mirador::PerceptionSession::create({frame.source_id, int64_t{4} * 1024 * 1024, int64_t{64} * 1024 * 1024})
            .take_value();

    std::vector<CallRecord> records;
    records.reserve(static_cast<size_t>(options.iterations));
    const int measured = options.mode == "baseline" ? measure_baseline(session, frame, options, records)
                                                    : measure_budgeted(session, frame, options, scene_ptr, records);
    if (measured != 0) {
        return measured;
    }

    print_summary(records, options);
    if (!options.csv_path.empty() && !write_csv(records, options.csv_path)) {
        return 5;
    }
    int gates = check_scenario_gates(records, options);
    // Scene consistency gates compare budgeted calls against the baseline
    // reference; baseline mode has no reference and its consistency columns
    // stay zero, so applying them there would fail every run (IVA round 1,
    // defect 3). AC8-01 baseline evidence carries the candidate-count gates.
    if (gates == 0 && scene_ptr != nullptr && options.mode == "budgeted") {
        gates = check_scene_gates(records, options, *scene_ptr);
    }
    return gates;
}
