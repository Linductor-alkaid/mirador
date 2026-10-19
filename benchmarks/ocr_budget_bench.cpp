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

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
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
#include <optional>
#include <string>
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
    int32_t sparse_n = 32;
    int32_t det_threads = 1;
    int32_t rec_threads = 1;
    int32_t max_boxes = 512;
    int32_t k = 64;
    int32_t warmup = 2;
    int32_t iterations = 20;
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
        "  --scenario dense359|dense512|longlines|sparse  (default dense359)\n"
        "  --sparse-n N                                    (with --scenario sparse)\n\n"
        "Configuration:\n"
        "  --mode budgeted|baseline   baseline = all-or-nothing PpOcrBackend\n"
        "  --det-threads T --rec-threads T --max-boxes B --k K\n"
        "  --soft-budget-ms MS --finalize-reserve-ms MS  (< 0 = disabled)\n"
        "  --warmup W --iterations N --csv PATH --compare-baseline\n\n"
        "Exit codes: 0 ok; 2 bad arguments; 3 missing model inputs; 4 gate failure\n"
        "(scenario gate not met, e.g. the 512-box stress scene did not hit the cap);\n"
        "5 backend failure during measurement.\n");
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

const std::array<std::pair<const char*, std::string Options::*>, 10>& string_options() noexcept {
    static const std::array<std::pair<const char*, std::string Options::*>, 10> table = {{
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
    }};
    return table;
}

const std::array<std::pair<const char*, int32_t Options::*>, 7>& int32_options() noexcept {
    static const std::array<std::pair<const char*, int32_t Options::*>, 7> table = {{
        {"--sparse-n", &Options::sparse_n},
        {"--det-threads", &Options::det_threads},
        {"--rec-threads", &Options::rec_threads},
        {"--max-boxes", &Options::max_boxes},
        {"--k", &Options::k},
        {"--warmup", &Options::warmup},
        {"--iterations", &Options::iterations},
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

/// Runs the all-or-nothing baseline measurement loop; returns an exit code
/// (0 ok) and appends one record per measured call.
int measure_baseline(mirador::PerceptionSession& session, const mirador::Frame& frame, const Options& options,
                     std::vector<CallRecord>& records) {
    auto backend = mirador::integrations::PpOcrBackend::create(det_options(options), rec_options(options));
    if (!backend.ok()) {
        std::printf("FAIL: baseline backend creation failed: %s\n", backend.status().message().c_str());
        return 3;
    }
    mirador::integrations::PpOcrBackend ocr = std::move(backend).take_value();
    const mirador::OcrRequest request;
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
            records.push_back(record);
        }
    }
    return 0;
}

/// Builds the budgeted request from the CLI options.
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
    return record;
}

/// Runs the budgeted measurement loop (session entry timing boundary).
int measure_budgeted(mirador::PerceptionSession& session, const mirador::Frame& frame, const Options& options,
                     std::vector<CallRecord>& records) {
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
    const mirador::BudgetedOcrRequest request = budget_request(options);
    const bool has_deadline = options.finalize_reserve_ms >= 0;

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
            records.push_back(
                budget_record(call - options.warmup, ocr, report.value(), static_cast<double>(elapsed_ns) / 1.0e6));
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
    }
}

/// Writes the per-call no-content CSV; returns false when the file cannot be
/// opened.
bool write_csv(const std::vector<CallRecord>& records, const std::string& path) {
    FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        std::printf("FAIL: cannot open csv output %s\n", path.c_str());
        return false;
    }
    std::fprintf(out,
                 "index,total_ms,det_canvas_ms,det_pack_ms,det_forward_ms,det_post_ms,select_ms,crop_ms,"
                 "rec_pre_ms,rec_forward_ms,rec_decode_ms,finalize_ms,detected,recognized,deferred,forwards,"
                 "nonempty_lines,stop,partial,det_limit_hit\n");
    for (const CallRecord& record : records) {
        std::fprintf(out, "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d,%d,%d\n",
                     record.index, record.total_ms, record.det_canvas_ms, record.det_pack_ms, record.det_forward_ms,
                     record.det_post_ms, record.select_ms, record.crop_ms, record.rec_pre_ms, record.rec_forward_ms,
                     record.rec_decode_ms, record.finalize_ms, record.detected, record.recognized, record.deferred,
                     record.forwards, record.nonempty_lines, record.stop, record.partial ? 1 : 0,
                     record.det_limit_hit ? 1 : 0);
    }
    std::fclose(out);
    std::printf("SUMMARY csv=%s\n", path.c_str());
    return true;
}

/// Scenario gates reported explicitly; failures exit 4 (never a silent pass).
/// The readable-text gates of AC8-02 are out of scope: these scenes are
/// pressure patterns.
int check_scenario_gates(const std::vector<CallRecord>& records, const Options& options) {
    if (options.scenario != "dense512" || records.empty()) {
        return 0;
    }
    const CallRecord& last = records.back();
    if (last.detected >= options.max_boxes) {
        return 0;
    }
    std::printf(
        "GATE FAIL: dense512 scene produced %d candidates < max_boxes %d; "
        "the stress gate needs a real 512-candidate chain\n",
        last.detected, options.max_boxes);
    return 4;
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

    // Scene (deterministic, generated in memory, never persisted).
    const std::vector<LineSpec> specs = scenario_lines(options.scenario, options.sparse_n);
    const uint64_t scene_seed = options.scenario == "dense512" ? 20261020ULL : 20261019ULL;
    auto scene_result = make_scene(specs, scene_seed);
    if (!scene_result.ok()) {
        std::printf("FAIL: scene allocation failed\n");
        return 5;
    }
    const mirador::ImageBuffer scene = scene_result.take_value();

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
                                                    : measure_budgeted(session, frame, options, records);
    if (measured != 0) {
        return measured;
    }

    print_summary(records, options);
    if (!options.csv_path.empty() && !write_csv(records, options.csv_path)) {
        return 5;
    }
    return check_scenario_gates(records, options);
}
