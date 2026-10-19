// M8-05 (DEC-025 section 7, AC8-09): independent recognition optimization
// experiment on REAL caller-provided PP-OCR weights — single-line cost
// decomposition, dynamic-vs-fixed width, width bucketing, horizontal line
// packing and ncnn thread scaling. The tool produces the measurement evidence
// for the M8-05 adopt/no-go decision; it changes no library code and freezes no
// default. Weights, dictionary and corpus stay caller-provided paths (DEC-015);
// nothing enters the repository.
//
// Arms (all share the frozen unit pipeline; only the width policy varies):
//   serial      dynamic width capped at rec_width (the frozen semantics)
//   threads2/4  serial width with ncnn num_threads 2/4 — contract tension: the
//               reference determinism contract (DEC-012) pins threads=1, so any
//               comparison here needs same-process ordering controls
//   pad320      aspect resize then right-pad zeros to exactly rec_width
//   bucket32/64 aspect resize then right-pad zeros to the next 32/64 px
//               multiple (cap-bound)
//   pack4cap    4 consecutive corpus lines joined horizontally, one forward,
//               the cap stays rec_width (contract-compliant packing)
//   pack4wide   the same join with the cap effectively removed (4096) — the
//               upper bound of what packing could reach, NOT a contract
//               setting
// Batching has no arm by construction: the pinned rec graph declares a 3-dim
// Input (no batch axis) and the pinned ncnn Extractor feeds one Mat per
// forward, so a "batch" arm would be fake — AC8-09 excludes fake batches; the
// capability evidence is documented in the experiment report instead.
//
// Exit codes: 0 ok, 2 CLI misuse, 3 missing input files, 4 sanity gate
// failure (cross-rep determinism, baseline text coverage), 5 data-format or
// I/O errors. Model weights are never written; the CSV carries timings and
// confidences, never image bytes.

#include "ctc_decode.hpp"
#include "ncnn_runtime.hpp"
#include "ppocr_backend.hpp"
#include "ppocr_budgeted.hpp"
#include "ppocr_internal.hpp"

#include <mirador/execution_context.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using mirador::ExecutionContext;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::OcrBatchRequest;
using mirador::OcrBatchResult;
using mirador::PixelFormat;
using mirador::Result;
using mirador::Status;
using mirador::integrations::ctc_decode_greedy;
using mirador::integrations::NcnnRuntime;
using mirador::integrations::NcnnRuntimeOptions;
using mirador::integrations::NcnnTensor;
using mirador::integrations::pack_image;
using mirador::integrations::PpOcrBudgetedBackend;
using mirador::integrations::PpOcrBudgetedOptions;
using mirador::integrations::PpOcrRecOptions;
using mirador::integrations::ppocr_detail::normalize_ppocr;
using mirador::integrations::ppocr_detail::swap_planes_to_bgr;
using RecLineOutput = mirador::integrations::ppocr_detail::RecLineOutput;
using RecUnitTiming = mirador::integrations::ppocr_detail::RecUnitTiming;
using mirador::integrations::ppocr_detail::kRecBlankClass;
using mirador::integrations::ppocr_detail::load_charset;
using mirador::integrations::ppocr_detail::run_rec_line;

constexpr int64_t kBufferBudgetBytes = int64_t{64} * 1024 * 1024;
constexpr int32_t kPackGapPixels = 8;
constexpr int32_t kPackWideCap = 4096;
constexpr size_t kClassCount = 4;

// ---------------------------------------------------------------------------
// Options and arms

struct Options {
    std::string rec_param;
    std::string rec_bin;
    std::string charset_path;
    std::string corpus_dir;
    std::vector<std::string> arms;
    int reps = 15;
    int warmup = 5;
    std::string csv_path;
    std::string summary_path;
    // Dense end-to-end sanity arm (optional): enabled when both det model
    // inputs are present.
    std::string det_param;
    std::string det_bin;
    int dense_reps = 5;
};

enum class WidthPolicy : std::uint8_t { kDynamic, kFixed, kBucket32, kBucket64 };

struct ArmSpec {
    const char* name = nullptr;
    WidthPolicy policy = WidthPolicy::kDynamic;
    int num_threads = 1;
    bool pack4 = false;
    int32_t rec_width = 320;
};

constexpr std::array<ArmSpec, 8> kArmTable{{
    {"serial", WidthPolicy::kDynamic, 1, false, 320},
    {"threads2", WidthPolicy::kDynamic, 2, false, 320},
    {"threads4", WidthPolicy::kDynamic, 4, false, 320},
    {"pad320", WidthPolicy::kFixed, 1, false, 320},
    {"bucket32", WidthPolicy::kBucket32, 1, false, 320},
    {"bucket64", WidthPolicy::kBucket64, 1, false, 320},
    {"pack4cap", WidthPolicy::kDynamic, 1, true, 320},
    {"pack4wide", WidthPolicy::kDynamic, 1, true, kPackWideCap},
}};

constexpr std::array<const char*, kClassCount> kClassNames{{"short", "medium", "long", "overlong"}};

// ---------------------------------------------------------------------------
// Small utilities

std::string path_in(const std::string& dir, const std::string& name) {
    std::string path = dir;
    path += '/';
    path += name;
    return path;
}

int64_t elapsed_ns(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
}

double ns_to_us(int64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

double percentile(std::vector<double>& values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank =
        std::max<size_t>(1, static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))));
    return values[rank - 1];
}

std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> parts;
    std::istringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

#ifdef __linux__
// Peak resident set of this process in KiB (measurement-environment note;
// runtime memory, not a Core budget guarantee).
bool read_vm_hwm_kib(int64_t& out) {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            status >> out;
            return true;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return false;
}
#else
bool read_vm_hwm_kib(int64_t& out) {
    out = -1;
    return false;
}
#endif

// ---------------------------------------------------------------------------
// Corpus loading (TSV manifests + packed raw RGB8 files)

struct LineImage {
    int32_t id = 0;
    int32_t width = 0;
    int32_t height = 0;
    size_t width_class = 0;  // index into kClassNames
    std::vector<std::byte> pixels;
};

// Non-owning RGB8 view of a corpus line image.
ImageView line_view(const LineImage& line) noexcept {
    ImageView view;
    view.data = line.pixels.data();
    view.width = line.width;
    view.height = line.height;
    view.row_stride_bytes = static_cast<int64_t>(line.width) * 3;
    view.format = PixelFormat::kRgb8;
    return view;
}

struct Corpus {
    std::vector<LineImage> lines;
    std::array<size_t, kClassCount> class_counts{};
    ImageBuffer dense_frame;  // empty when the dense arm is not configured
    size_t dense_count = 0;
};

Result<std::vector<std::byte>> read_raw_bytes(const std::string& path, size_t bytes) {
    std::ifstream raw(path, std::ios::binary);
    if (!static_cast<bool>(raw)) {
        return Status{mirador::ErrorCode::kBackendUnavailable, "cannot open " + path};
    }
    std::vector<std::byte> data(bytes);
    raw.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(bytes));
    if (static_cast<size_t>(raw.gcount()) != bytes) {
        return Status{mirador::ErrorCode::kInvalidArgument, "truncated image file: " + path};
    }
    return data;
}

// Parses one lines.tsv row (id, file, width, height, width_class) into `line`
// and returns the corpus-relative file name; every range violation is an
// error, never a fallback.
Result<std::string> parse_line_row(const std::string& row, int32_t expected_id, LineImage& line) {
    std::istringstream fields(row);
    std::string id_field;
    std::string file_field;
    std::string width_field;
    std::string height_field;
    std::string class_field;
    if (!std::getline(fields, id_field, '\t') || !std::getline(fields, file_field, '\t') ||
        !std::getline(fields, width_field, '\t') || !std::getline(fields, height_field, '\t') ||
        !std::getline(fields, class_field, '\t')) {
        return Status{mirador::ErrorCode::kInvalidArgument, "malformed line row: " + row};
    }
    line.id = std::atoi(id_field.c_str());
    line.width = std::atoi(width_field.c_str());
    line.height = std::atoi(height_field.c_str());
    line.width_class = static_cast<size_t>(std::atoi(class_field.c_str()));
    if (line.width <= 0 || line.height <= 0 || line.width_class >= kClassCount || line.id != expected_id) {
        return Status{mirador::ErrorCode::kInvalidArgument, "out-of-range line metadata: " + row};
    }
    return file_field;
}

Result<Corpus> load_line_corpus(const std::string& corpus_dir) {
    Corpus corpus;
    const std::string manifest_path = path_in(corpus_dir, "lines.tsv");
    std::ifstream lines_stream(manifest_path);
    if (!static_cast<bool>(lines_stream)) {
        return Status{mirador::ErrorCode::kBackendUnavailable, "cannot open " + manifest_path};
    }
    std::string row;
    while (std::getline(lines_stream, row)) {
        if (row.empty()) {
            continue;
        }
        LineImage line;
        auto file = parse_line_row(row, static_cast<int32_t>(corpus.lines.size()), line);
        if (!file.ok()) {
            return file.status();
        }
        const size_t bytes = static_cast<size_t>(line.width) * static_cast<size_t>(line.height) * 3U;
        auto pixels = read_raw_bytes(path_in(corpus_dir, file.value()), bytes);
        if (!pixels.ok()) {
            return pixels.status();
        }
        line.pixels = std::move(pixels).take_value();
        ++corpus.class_counts[line.width_class];
        corpus.lines.push_back(std::move(line));
    }
    if (corpus.lines.empty()) {
        return Status{mirador::ErrorCode::kInvalidArgument, "corpus has no lines"};
    }
    return corpus;
}

// Parses the single dense.tsv row (file, width, height, text_count).
Result<std::tuple<std::string, int32_t, int32_t, int32_t>> parse_dense_row(const std::string& row) {
    std::istringstream fields(row);
    std::string file_field;
    std::string width_field;
    std::string height_field;
    std::string count_field;
    if (!std::getline(fields, file_field, '\t') || !std::getline(fields, width_field, '\t') ||
        !std::getline(fields, height_field, '\t') || !std::getline(fields, count_field, '\t')) {
        return Status{mirador::ErrorCode::kInvalidArgument, "malformed dense row"};
    }
    const int32_t width = std::atoi(width_field.c_str());
    const int32_t height = std::atoi(height_field.c_str());
    const int32_t count = std::atoi(count_field.c_str());
    if (file_field.empty() || width <= 0 || height <= 0 || count <= 0) {
        return Status{mirador::ErrorCode::kInvalidArgument, "out-of-range dense metadata"};
    }
    return std::make_tuple(file_field, width, height, count);
}

Result<std::pair<ImageBuffer, size_t>> load_dense_frame(const std::string& corpus_dir) {
    const std::string manifest_path = path_in(corpus_dir, "dense.tsv");
    std::ifstream dense_stream(manifest_path);
    if (!static_cast<bool>(dense_stream)) {
        return Status{mirador::ErrorCode::kBackendUnavailable, "cannot open " + manifest_path};
    }
    std::string row;
    if (!std::getline(dense_stream, row)) {
        return Status{mirador::ErrorCode::kInvalidArgument, "dense.tsv is empty"};
    }
    auto meta = parse_dense_row(row);
    if (!meta.ok()) {
        return meta.status();
    }
    const auto [file, width, height, count] = std::move(meta).take_value();
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3U;
    auto raw = read_raw_bytes(path_in(corpus_dir, file), bytes);
    if (!raw.ok()) {
        return raw.status();
    }
    auto frame = ImageBuffer::create(PixelFormat::kRgb8, width, height, kBufferBudgetBytes);
    if (!frame.ok()) {
        return frame.status();
    }
    ImageBuffer buffer = std::move(frame).take_value();
    std::memcpy(buffer.data(), raw.value().data(), bytes);
    return std::make_pair(std::move(buffer), static_cast<size_t>(count));
}

Result<Corpus> load_corpus(const std::string& corpus_dir, bool want_dense) {
    auto lines = load_line_corpus(corpus_dir);
    if (!lines.ok()) {
        return lines.status();
    }
    Corpus corpus = std::move(lines).take_value();
    if (want_dense) {
        auto dense = load_dense_frame(corpus_dir);
        if (!dense.ok()) {
            return dense.status();
        }
        auto frame = std::move(dense).take_value();
        corpus.dense_frame = std::move(frame.first);
        corpus.dense_count = frame.second;
    }
    return corpus;
}

// Joins 4 consecutive corpus lines into one strip with a white gap (the pack
// arms' unit input). Corpus lines share one rendered height, so the join is a
// plain horizontal concatenation.
Result<ImageBuffer> join_lines(const std::vector<LineImage>& lines, size_t first) {
    const LineImage& head = lines[first];
    int32_t total_width = kPackGapPixels * 3;
    for (size_t i = first; i < first + 4; ++i) {
        if (lines[i].height != head.height) {
            return Status{mirador::ErrorCode::kInvalidArgument, "pack corpus lines must share one height"};
        }
        total_width += lines[i].width;
    }
    auto joined = ImageBuffer::create(PixelFormat::kRgb8, total_width, head.height, kBufferBudgetBytes);
    if (!joined.ok()) {
        return joined.status();
    }
    ImageBuffer buffer = std::move(joined).take_value();
    // ImageBuffer::create zero-initializes; pre-fill everything with white so
    // the gap columns read as background, not as black padding.
    const int64_t stride = buffer.row_stride_bytes();
    for (int32_t y = 0; y < head.height; ++y) {
        std::memset(buffer.data() + static_cast<int64_t>(y) * stride, 255, static_cast<size_t>(stride));
    }
    int32_t cursor = 0;
    for (size_t i = first; i < first + 4; ++i) {
        const LineImage& line = lines[i];
        for (int32_t y = 0; y < line.height; ++y) {
            std::memcpy(buffer.data() + static_cast<int64_t>(y) * stride + static_cast<int64_t>(cursor) * 3,
                        line.pixels.data() + static_cast<int64_t>(y) * static_cast<int64_t>(line.width) * 3,
                        static_cast<size_t>(line.width) * 3U);
        }
        cursor += line.width + kPackGapPixels;
    }
    return buffer;
}

// ---------------------------------------------------------------------------
// Width-policy variant of the recognition unit. The frozen `run_rec_line`
// stays untouched; the variant reproduces its pipeline (resize -> pack -> BGR
// swap -> normalize -> one forward -> CTC decode) with a different destination
// width, so an arm difference is attributable to the width policy alone.

int32_t dynamic_rec_width(const ImageView& line_image, const PpOcrRecOptions& options) {
    const int64_t scaled_width = static_cast<int64_t>(line_image.width) * static_cast<int64_t>(options.rec_height);
    const auto width = static_cast<int32_t>(std::lround(static_cast<double>(scaled_width) / line_image.height));
    return std::clamp<int32_t>(width, 1, options.rec_width);
}

Result<RecLineOutput> run_padded_line(const PpOcrRecOptions& options, const std::vector<std::string>& charset,
                                      const NcnnRuntime& runtime, const ImageView& line_image, int32_t padded_width,
                                      RecUnitTiming& timing, const ExecutionContext& context) {
    const auto begin = std::chrono::steady_clock::now();
    const int32_t line_width = dynamic_rec_width(line_image, options);
    auto resized = mirador::resize_area(line_image, line_width, options.rec_height, options.work_budget_bytes);
    if (!resized.ok()) {
        return resized.status();
    }
    auto padded = ImageBuffer::create(PixelFormat::kRgb8, padded_width, options.rec_height, options.work_budget_bytes);
    if (!padded.ok()) {
        return padded.status();
    }
    ImageBuffer padded_buffer = std::move(padded).take_value();
    // The zero-initialized buffer already carries the right-pad value (pixel 0
    // -> normalized -1, the pad-0 letterbox convention of normalize_ppocr).
    const int64_t padded_stride = padded_buffer.row_stride_bytes();
    const int64_t resized_stride = resized.value().row_stride_bytes();
    for (int32_t y = 0; y < options.rec_height; ++y) {
        std::memcpy(padded_buffer.data() + static_cast<int64_t>(y) * padded_stride,
                    resized.value().view().data + static_cast<int64_t>(y) * resized_stride,
                    static_cast<size_t>(line_width) * 3U);
    }
    auto input_result = pack_image(padded_buffer.view(), context);
    if (!input_result.ok()) {
        return input_result.status();
    }
    NcnnTensor input = std::move(input_result).take_value();
    if (const Result<void> swapped = swap_planes_to_bgr(input); !swapped.ok()) {
        return swapped.status();
    }
    if (const Result<void> normalized = normalize_ppocr(input, context); !normalized.ok()) {
        return normalized.status();
    }
    timing.preprocess_ns = elapsed_ns(begin);

    const auto forward_begin = std::chrono::steady_clock::now();
    auto output = runtime.run("x", input, "out", context);
    if (!output.ok()) {
        return output.status();
    }
    timing.forward_ns = elapsed_ns(forward_begin);

    const auto decode_begin = std::chrono::steady_clock::now();
    const NcnnTensor& scores = output.value();
    if (scores.height != 1 || scores.width < 2 || scores.channels < 1 ||
        scores.data.size() != static_cast<size_t>(scores.channels) * static_cast<size_t>(scores.width)) {
        return Status{mirador::ErrorCode::kBackendFailure,
                      "rec model output must be CHW with channels=time steps and width=class count"};
    }
    auto line = ctc_decode_greedy(scores.data.data(), scores.channels, scores.width, kRecBlankClass);
    if (!line.ok()) {
        return line.status();
    }
    RecLineOutput decoded;
    for (const int32_t class_index : line.value().class_indices) {
        if (class_index < 1 || static_cast<size_t>(class_index) > charset.size()) {
            return Status{mirador::ErrorCode::kBackendFailure, "model emitted a class outside the charset"};
        }
        decoded.utf8_text += charset[static_cast<size_t>(class_index) - 1];
    }
    decoded.confidence = line.value().confidence;
    timing.decode_ns = elapsed_ns(decode_begin);
    return decoded;
}

// One arm unit: a single corpus line (normal arms) or a joined strip (pack
// arms), driven through the frozen unit path with the arm's width settings.
struct UnitOutput {
    std::string text;
    float confidence = 0.0F;
    int64_t total_ns = 0;
    int64_t preprocess_ns = 0;
    int64_t forward_ns = 0;
    int64_t decode_ns = 0;
};

Result<UnitOutput> run_unit(const PpOcrRecOptions& options, const std::vector<std::string>& charset,
                            const NcnnRuntime& runtime, const LineImage* line, const ImageBuffer* joined,
                            WidthPolicy policy, const ExecutionContext& context) {
    UnitOutput out;
    RecUnitTiming timing;
    const auto begin = std::chrono::steady_clock::now();
    Result<RecLineOutput> decoded = Status{mirador::ErrorCode::kInvalidArgument, "no unit input selected"};
    if (joined != nullptr) {
        decoded = run_rec_line(options, charset, runtime, joined->view(), context, &timing);
    } else if (line == nullptr) {
        return Status{mirador::ErrorCode::kInvalidArgument, "unit needs a line or a joined strip"};
    } else if (policy == WidthPolicy::kDynamic) {
        decoded = run_rec_line(options, charset, runtime, line_view(*line), context, &timing);
    } else {
        int32_t padded_width = options.rec_width;
        if (policy == WidthPolicy::kBucket32 || policy == WidthPolicy::kBucket64) {
            const int32_t multiple = policy == WidthPolicy::kBucket32 ? 32 : 64;
            const int32_t dynamic_width = dynamic_rec_width(line_view(*line), options);
            padded_width = std::min<int32_t>(((dynamic_width + multiple - 1) / multiple) * multiple, options.rec_width);
        }
        decoded = run_padded_line(options, charset, runtime, line_view(*line), padded_width, timing, context);
    }
    if (!decoded.ok()) {
        return decoded.status();
    }
    RecLineOutput value = std::move(decoded).take_value();
    out.text = std::move(value.utf8_text);
    out.confidence = value.confidence;
    out.total_ns = elapsed_ns(begin);
    out.preprocess_ns = timing.preprocess_ns;
    out.forward_ns = timing.forward_ns;
    out.decode_ns = timing.decode_ns;
    return out;
}

// ---------------------------------------------------------------------------
// Arm execution

struct ArmSample {
    int64_t total_ns = 0;
    int64_t preprocess_ns = 0;
    int64_t forward_ns = 0;
    int64_t decode_ns = 0;
};

struct ArmResult {
    const ArmSpec* spec = nullptr;
    std::vector<ArmSample> samples;  // (unit, rep), excluding warmup
    std::vector<std::string> rep0_texts;
    std::vector<float> rep0_confs;
    int64_t wall_ns = 0;
    int64_t vmhwm_kib = -1;
    // Quality against the serial baseline (filled after all arms ran).
    double text_match_frac = 0.0;
    double prefix_match_frac = -1.0;  // pack arms only; -1 = n/a
    float conf_max_abs_diff = -1.0F;  // pack arms: n/a
};

size_t unit_count(const ArmSpec& spec, size_t line_count) {
    return spec.pack4 ? line_count / 4 : line_count;
}

PpOcrRecOptions arm_rec_options(const PpOcrRecOptions& base, const ArmSpec& spec) {
    PpOcrRecOptions options = base;
    options.rec_width = spec.rec_width;
    return options;
}

struct UnitInput {
    const LineImage* line = nullptr;
    const ImageBuffer* strip = nullptr;
};

UnitInput unit_input(const ArmSpec& spec, const Corpus& corpus, const std::vector<ImageBuffer>& joined, size_t unit) {
    if (spec.pack4) {
        return UnitInput{nullptr, &joined[unit]};
    }
    return UnitInput{&corpus.lines[unit], nullptr};
}

int warmup_arm(const ArmSpec& spec, const PpOcrRecOptions& arm_options, const std::vector<std::string>& charset,
               const Corpus& corpus, const std::vector<ImageBuffer>& joined, const Options& options,
               const NcnnRuntime& runtime) {
    const size_t units = unit_count(spec, corpus.lines.size());
    const ExecutionContext context;
    for (int w = 0; w < options.warmup; ++w) {
        for (size_t u = 0; u < units; ++u) {
            const UnitInput input = unit_input(spec, corpus, joined, u);
            const auto decoded = run_unit(arm_options, charset, runtime, input.line, input.strip, spec.policy, context);
            if (!decoded.ok()) {
                std::fprintf(stderr, "arm %s: warmup unit %zu failed: %s\n", spec.name, u,
                             decoded.status().message().c_str());
                return 5;
            }
        }
    }
    return 0;
}

int run_arm(const ArmSpec& spec, const PpOcrRecOptions& base_options, const std::vector<std::string>& charset,
            const Corpus& corpus, const std::vector<ImageBuffer>& joined, const Options& options, ArmResult& result) {
    auto runtime =
        NcnnRuntime::create(NcnnRuntimeOptions{base_options.param_path, base_options.bin_path, spec.num_threads});
    if (!runtime.ok()) {
        std::fprintf(stderr, "arm %s: runtime creation failed: %s\n", spec.name, runtime.status().message().c_str());
        return 3;
    }
    const PpOcrRecOptions arm_options = arm_rec_options(base_options, spec);
    const size_t units = unit_count(spec, corpus.lines.size());
    const int warmed = warmup_arm(spec, arm_options, charset, corpus, joined, options, runtime.value());
    if (warmed != 0) {
        return warmed;
    }

    result.spec = &spec;
    result.samples.reserve(units * static_cast<size_t>(options.reps));
    result.rep0_texts.resize(units);
    result.rep0_confs.resize(units);
    const ExecutionContext context;
    const auto wall_begin = std::chrono::steady_clock::now();
    for (int rep = 0; rep < options.reps; ++rep) {
        for (size_t u = 0; u < units; ++u) {
            const UnitInput input = unit_input(spec, corpus, joined, u);
            const auto decoded =
                run_unit(arm_options, charset, runtime.value(), input.line, input.strip, spec.policy, context);
            if (!decoded.ok()) {
                std::fprintf(stderr, "arm %s: unit %zu rep %d failed: %s\n", spec.name, u, rep,
                             decoded.status().message().c_str());
                return 5;
            }
            ArmSample sample;
            sample.total_ns = decoded.value().total_ns;
            sample.preprocess_ns = decoded.value().preprocess_ns;
            sample.forward_ns = decoded.value().forward_ns;
            sample.decode_ns = decoded.value().decode_ns;
            result.samples.push_back(sample);
            if (rep == 0) {
                result.rep0_texts[u] = decoded.value().text;
                result.rep0_confs[u] = decoded.value().confidence;
            } else if (result.rep0_texts[u] != decoded.value().text ||
                       result.rep0_confs[u] != decoded.value().confidence) {
                // threads=1 is the frozen deterministic configuration (DEC-012):
                // a cross-rep mismatch there invalidates the measurement. With
                // ncnn num_threads > 1 a mismatch is a REPORTED finding about
                // that configuration, not a harness failure.
                if (spec.num_threads == 1) {
                    std::fprintf(stderr, "arm %s: nondeterministic output at unit %zu rep %d\n", spec.name, u, rep);
                    return 4;
                }
                std::fprintf(stderr,
                             "NOTE arm %s: output changed at unit %zu rep %d (num_threads=%d) — "
                             "reported, measurement continues\n",
                             spec.name, u, rep, spec.num_threads);
            }
        }
    }
    result.wall_ns = elapsed_ns(wall_begin);
    std::ignore = read_vm_hwm_kib(result.vmhwm_kib);
    return 0;
}

// ---------------------------------------------------------------------------
// Reporting

// Writes one formatted line to stdout and, when open, the summary file.
class SummarySink {
public:
    explicit SummarySink(FILE* stdout_file) noexcept : stdout_file_(stdout_file) {}
    SummarySink(const SummarySink&) = delete;
    SummarySink& operator=(const SummarySink&) = delete;
    SummarySink(SummarySink&&) = delete;
    SummarySink& operator=(SummarySink&&) = delete;
    ~SummarySink() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    [[nodiscard]] bool open(const std::string& path) {
        file_ = std::fopen(path.c_str(), "w");
        return file_ != nullptr;
    }

    void line(const char* format, ...) {
        std::array<char, 2048> buffer{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer.data(), buffer.size(), format, args);
        va_end(args);
        std::fputs(buffer.data(), stdout_file_);
        if (file_ != nullptr) {
            std::fputs(buffer.data(), file_);
        }
    }

private:
    FILE* stdout_file_ = nullptr;
    FILE* file_ = nullptr;
};

void print_arm_summary(SummarySink& sink, const ArmResult& arm) {
    std::vector<double> totals;
    totals.reserve(arm.samples.size());
    int64_t pre_sum = 0;
    int64_t fwd_sum = 0;
    int64_t dec_sum = 0;
    for (const ArmSample& sample : arm.samples) {
        totals.push_back(ns_to_us(sample.total_ns));
        pre_sum += sample.preprocess_ns;
        fwd_sum += sample.forward_ns;
        dec_sum += sample.decode_ns;
    }
    const int64_t stage_sum = pre_sum + fwd_sum + dec_sum;
    const double pre_pct = stage_sum > 0 ? 100.0 * static_cast<double>(pre_sum) / static_cast<double>(stage_sum) : 0.0;
    const double fwd_pct = stage_sum > 0 ? 100.0 * static_cast<double>(fwd_sum) / static_cast<double>(stage_sum) : 0.0;
    const double dec_pct = stage_sum > 0 ? 100.0 * static_cast<double>(dec_sum) / static_cast<double>(stage_sum) : 0.0;
    const double max_us = totals.empty() ? 0.0 : *std::max_element(totals.begin(), totals.end());
    sink.line(
        "ARM %s units=%zu samples=%zu wall_ms=%.1f p50_us=%.1f p95_us=%.1f max_us=%.1f pre_pct=%.1f "
        "fwd_pct=%.1f dec_pct=%.1f vmhwm_kib=%lld\n",
        arm.spec->name, arm.rep0_texts.size(), arm.samples.size(), ns_to_us(arm.wall_ns) / 1000.0,
        percentile(totals, 0.50), percentile(totals, 0.95), max_us, pre_pct, fwd_pct, dec_pct,
        static_cast<long long>(arm.vmhwm_kib));
}

void print_quality_line(SummarySink& sink, const ArmResult& arm) {
    if (arm.prefix_match_frac >= 0.0) {
        sink.line("QUALITY %s group_text_match=%.3f first_line_prefix_match=%.3f\n", arm.spec->name,
                  arm.text_match_frac, arm.prefix_match_frac);
    } else {
        sink.line("QUALITY %s text_match=%.4f conf_max_abs_diff=%.6f\n", arm.spec->name, arm.text_match_frac,
                  static_cast<double>(arm.conf_max_abs_diff));
    }
}

void print_serial_class_summary(SummarySink& sink, const ArmResult& serial, const PpOcrRecOptions& serial_options,
                                const Corpus& corpus, int reps) {
    const size_t units = serial.rep0_texts.size();
    for (size_t klass = 0; klass < kClassCount; ++klass) {
        if (corpus.class_counts[klass] == 0) {
            continue;
        }
        std::vector<double> totals;
        std::vector<double> widths;
        for (size_t u = 0; u < units; ++u) {
            const LineImage& line = corpus.lines[u];
            if (line.width_class != klass) {
                continue;
            }
            widths.push_back(static_cast<double>(dynamic_rec_width(line_view(line), serial_options)));
            for (int rep = 0; rep < reps; ++rep) {
                totals.push_back(ns_to_us(serial.samples[static_cast<size_t>(rep) * units + u].total_ns));
            }
        }
        double width_sum = 0.0;
        for (const double width : widths) {
            width_sum += width;
        }
        sink.line("CLASS serial %s lines=%zu resized_w_avg=%.1f p50_us=%.1f p95_us=%.1f\n", kClassNames[klass],
                  widths.size(), widths.empty() ? 0.0 : width_sum / static_cast<double>(widths.size()),
                  percentile(totals, 0.50), percentile(totals, 0.95));
    }
}

// ---------------------------------------------------------------------------
// Dense end-to-end sanity arm (optional)

int run_dense_arm(const Options& options, const Corpus& corpus) {
    PpOcrBudgetedOptions budgeted;
    budgeted.det.param_path = options.det_param;
    budgeted.det.bin_path = options.det_bin;
    budgeted.det.identity.model_id = "ocr-rec-experiment-det";
    budgeted.det.identity.model_revision = "caller-provided";
    // The generated dense frame carries more text boxes than the default
    // detection budget (256); this arm measures end-to-end throughput, so the
    // box budget is raised instead of shaping the scene to the default.
    budgeted.det.postprocess.max_boxes = 1024;
    budgeted.rec.param_path = options.rec_param;
    budgeted.rec.bin_path = options.rec_bin;
    budgeted.rec.charset_path = options.charset_path;
    budgeted.rec.identity.model_id = "ocr-rec-experiment-rec";
    budgeted.rec.identity.model_revision = "caller-provided";
    auto backend = PpOcrBudgetedBackend::create(budgeted);
    if (!backend.ok()) {
        std::fprintf(stderr, "dense arm: backend creation failed: %s\n", backend.status().message().c_str());
        return 3;
    }
    auto budgeted_backend = std::move(backend).take_value();

    OcrBatchRequest request;
    request.max_rec_lines = 512;
    const ImageView frame = corpus.dense_frame.view();
    std::printf("DENSE frame=%dx%d declared_lines=%zu reps=%d\n", frame.width, frame.height, corpus.dense_count,
                options.dense_reps);
    std::optional<OcrBatchResult> rep0;
    for (int rep = 0; rep < options.dense_reps; ++rep) {
        const ExecutionContext context;
        const auto begin = std::chrono::steady_clock::now();
        auto result = budgeted_backend.recognize_budgeted(frame, request, context);
        if (!result.ok()) {
            std::fprintf(stderr, "dense arm: rep %d failed: %s\n", rep, result.status().message().c_str());
            return 5;
        }
        const double wall_ms = ns_to_us(elapsed_ns(begin)) / 1000.0;
        const auto& timings = budgeted_backend.last_stage_timings();
        std::printf(
            "DENSE rep=%d wall_ms=%.1f detected=%d recognized=%d deferred=%d stop=%d det_fwd_us=%.1f "
            "rec_fwd_sum_us=%.1f rec_pre_sum_us=%.1f rec_dec_sum_us=%.1f\n",
            rep, wall_ms, result.value().detected_count, result.value().recognized_count, result.value().deferred_count,
            static_cast<int>(result.value().stop_reason), ns_to_us(timings.det_forward_ns),
            ns_to_us(timings.rec_forward_ns), ns_to_us(timings.rec_preprocess_ns), ns_to_us(timings.rec_decode_ns));
        if (rep == 0) {
            rep0 = result.value();
        } else if (!(*rep0 == result.value())) {
            std::printf("DENSE determinism=different\n");
            return 4;
        }
    }
    std::printf("DENSE determinism=identical\n");
    return 0;
}

// ---------------------------------------------------------------------------
// CLI and main phases

int parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; i += 2) {
        const std::string name = argv[i];
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", name.c_str());
            return 2;
        }
        const std::string value = argv[i + 1];
        if (name == "--rec-param") {
            options.rec_param = value;
        } else if (name == "--rec-bin") {
            options.rec_bin = value;
        } else if (name == "--charset") {
            options.charset_path = value;
        } else if (name == "--corpus-dir") {
            options.corpus_dir = value;
        } else if (name == "--arms") {
            options.arms = split_csv(value);
        } else if (name == "--reps") {
            options.reps = std::atoi(value.c_str());
        } else if (name == "--warmup") {
            options.warmup = std::atoi(value.c_str());
        } else if (name == "--csv") {
            options.csv_path = value;
        } else if (name == "--summary") {
            options.summary_path = value;
        } else if (name == "--det-param") {
            options.det_param = value;
        } else if (name == "--det-bin") {
            options.det_bin = value;
        } else if (name == "--dense-reps") {
            options.dense_reps = std::atoi(value.c_str());
        } else {
            std::fprintf(stderr, "unknown option %s\n", name.c_str());
            return 2;
        }
    }
    if (options.rec_param.empty() || options.rec_bin.empty() || options.charset_path.empty() ||
        options.corpus_dir.empty()) {
        std::fprintf(stderr,
                     "usage: mirador_bench_ocr_rec_experiment --rec-param P --rec-bin B --charset C "
                     "--corpus-dir DIR [--arms serial,threads2,threads4,pad320,bucket32,bucket64,pack4cap,"
                     "pack4wide] [--reps N] [--warmup N] [--csv PATH] [--summary PATH] [--det-param P "
                     "--det-bin B] [--dense-reps N]\n");
        return 2;
    }
    if (options.reps < 1 || options.reps > 1000 || options.warmup < 0 || options.warmup > 100) {
        std::fprintf(stderr, "reps must be in [1,1000] and warmup in [0,100]\n");
        return 2;
    }
    if (options.arms.empty()) {
        for (const ArmSpec& spec : kArmTable) {
            options.arms.emplace_back(spec.name);
        }
    }
    return 0;
}

bool file_readable(const std::string& path) {
    return static_cast<bool>(std::ifstream(path, std::ios::binary));
}

int check_inputs(const Options& options, bool want_dense) {
    const std::vector<std::string> required{options.rec_param, options.rec_bin, options.charset_path,
                                            path_in(options.corpus_dir, "lines.tsv")};
    for (const std::string& path : required) {
        if (!file_readable(path)) {
            std::fprintf(stderr, "missing required input: %s\n", path.c_str());
            return 3;
        }
    }
    if (want_dense) {
        const std::string dense_manifest = path_in(options.corpus_dir, "dense.tsv");
        if (!file_readable(dense_manifest)) {
            std::fprintf(stderr, "missing required input: %s\n", dense_manifest.c_str());
            return 3;
        }
    }
    return 0;
}

int resolve_arms(const Options& options, std::vector<const ArmSpec*>& arms) {
    for (const std::string& name : options.arms) {
        const auto* const found = std::find_if(kArmTable.begin(), kArmTable.end(),
                                               [&name](const ArmSpec& spec) { return name == spec.name; });
        if (found == kArmTable.end()) {
            std::fprintf(stderr, "unknown arm '%s'\n", name.c_str());
            return 2;
        }
        arms.push_back(&*found);
    }
    const auto serial_order = std::find(arms.begin(), arms.end(), kArmTable.data());
    if (serial_order != arms.end() && serial_order != arms.begin()) {
        std::fprintf(stderr, "the serial arm must come first when requested\n");
        return 2;
    }
    return 0;
}

int build_joined_strips(const std::vector<const ArmSpec*>& arms, const Corpus& corpus,
                        std::vector<ImageBuffer>& joined) {
    for (const ArmSpec* spec : arms) {
        if (!spec->pack4 || !joined.empty()) {
            continue;
        }
        const size_t groups = unit_count(*spec, corpus.lines.size());
        joined.reserve(groups);
        for (size_t g = 0; g < groups; ++g) {
            auto strip = join_lines(corpus.lines, g * 4);
            if (!strip.ok()) {
                std::fprintf(stderr, "join failed: %s\n", strip.status().message().c_str());
                return 5;
            }
            joined.push_back(std::move(strip).take_value());
        }
    }
    return 0;
}

// Pack arms compare joined-group text against the concatenation of the four
// serial texts; per-line confidences are meaningless there (n/a marker kept).
void fill_pack_quality(ArmResult& arm, const ArmResult& serial) {
    const size_t groups = arm.rep0_texts.size();
    size_t matches = 0;
    size_t prefixes = 0;
    for (size_t g = 0; g < groups; ++g) {
        std::string expected;
        for (size_t i = g * 4; i < g * 4 + 4; ++i) {
            expected += serial.rep0_texts[i];
        }
        if (arm.rep0_texts[g] == expected) {
            ++matches;
        }
        const std::string& first = serial.rep0_texts[g * 4];
        if (!first.empty() && arm.rep0_texts[g].starts_with(first)) {
            ++prefixes;
        }
    }
    arm.text_match_frac = groups > 0 ? static_cast<double>(matches) / static_cast<double>(groups) : 0.0;
    arm.prefix_match_frac = groups > 0 ? static_cast<double>(prefixes) / static_cast<double>(groups) : 0.0;
}

// Normal arms compare unit-by-unit against the serial baseline.
void fill_line_quality(ArmResult& arm, const ArmResult& serial) {
    const size_t units = arm.rep0_texts.size();
    size_t matches = 0;
    float conf_diff = 0.0F;
    for (size_t u = 0; u < units; ++u) {
        if (arm.rep0_texts[u] == serial.rep0_texts[u]) {
            ++matches;
        }
        conf_diff = std::max(conf_diff, std::fabs(arm.rep0_confs[u] - serial.rep0_confs[u]));
    }
    arm.text_match_frac = units > 0 ? static_cast<double>(matches) / static_cast<double>(units) : 0.0;
    arm.conf_max_abs_diff = conf_diff;
}

void fill_quality_fractions(std::vector<ArmResult>& results) {
    ArmResult* serial = nullptr;
    for (ArmResult& arm : results) {
        if (arm.spec == kArmTable.data()) {
            serial = &arm;
        }
    }
    if (serial == nullptr) {
        return;
    }
    serial->text_match_frac = 1.0;
    serial->conf_max_abs_diff = 0.0F;
    for (ArmResult& arm : results) {
        if (arm.spec->pack4) {
            fill_pack_quality(arm, *serial);
        } else if (&arm != serial) {
            fill_line_quality(arm, *serial);
        }
    }
}

int write_csv(const Options& options, const std::vector<ArmResult>& results) {
    if (options.csv_path.empty()) {
        return 0;
    }
    // CSV rows carry timings and confidences only — never image bytes.
    std::ofstream csv(options.csv_path, std::ios::binary | std::ios::trunc);
    if (!static_cast<bool>(csv)) {
        std::fprintf(stderr, "cannot open csv output: %s\n", options.csv_path.c_str());
        return 5;
    }
    csv << "arm,unit,rep,total_ns,preprocess_ns,forward_ns,decode_ns,confidence,text_len\n";
    for (const ArmResult& arm : results) {
        const size_t units = arm.rep0_texts.size();
        for (int rep = 0; rep < options.reps; ++rep) {
            for (size_t u = 0; u < units; ++u) {
                const ArmSample& sample = arm.samples[static_cast<size_t>(rep) * units + u];
                csv << arm.spec->name << ',' << u << ',' << rep << ',' << sample.total_ns << ',' << sample.preprocess_ns
                    << ',' << sample.forward_ns << ',' << sample.decode_ns << ',' << arm.rep0_confs[u] << ','
                    << arm.rep0_texts[u].size() << '\n';
            }
        }
    }
    if (!static_cast<bool>(csv)) {
        std::fprintf(stderr, "csv write failed: %s\n", options.csv_path.c_str());
        return 5;
    }
    return 0;
}

int measure_all_arms(const Options& options, const PpOcrRecOptions& rec_options,
                     const std::vector<std::string>& charset, const Corpus& corpus,
                     const std::vector<ImageBuffer>& joined, const std::vector<const ArmSpec*>& arms,
                     std::vector<ArmResult>& results) {
    results.reserve(arms.size());
    for (const ArmSpec* spec : arms) {
        ArmResult result;
        const int rc = run_arm(*spec, rec_options, charset, corpus, joined, options, result);
        if (rc != 0) {
            return rc;
        }
        results.push_back(std::move(result));
    }
    fill_quality_fractions(results);
    return 0;
}

// Fraction of non-empty baseline (serial) texts — the model/charset sanity
// gate. nullopt when the serial arm was not requested.
std::optional<double> baseline_coverage(const std::vector<ArmResult>& results) {
    const auto serial_it =
        std::find_if(results.begin(), results.end(), [](const ArmResult& arm) { return arm.spec == kArmTable.data(); });
    if (serial_it == results.end()) {
        return std::nullopt;
    }
    size_t non_empty = 0;
    for (const std::string& text : serial_it->rep0_texts) {
        if (!text.empty()) {
            ++non_empty;
        }
    }
    return static_cast<double>(non_empty) / static_cast<double>(serial_it->rep0_texts.size());
}

}  // namespace

// Measurement CLI: the process owns no invariants that require unwinding; an
// allocation failure aborting the run is an acceptable harness outcome.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    Options options;
    if (const int parsed = parse_args(argc, argv, options); parsed != 0) {
        return parsed;
    }

    // Dense mode is opt-in through both det model inputs.
    const bool want_dense = !options.det_param.empty() || !options.det_bin.empty();
    if (want_dense && (options.det_param.empty() || options.det_bin.empty())) {
        std::fprintf(stderr, "dense arm needs --det-param and --det-bin together\n");
        return 2;
    }
    if (const int checked = check_inputs(options, want_dense); checked != 0) {
        return checked;
    }

    auto charset = load_charset(options.charset_path);
    if (!charset.ok()) {
        std::fprintf(stderr, "charset load failed: %s\n", charset.status().message().c_str());
        return 3;
    }
    auto corpus_result = load_corpus(options.corpus_dir, want_dense);
    if (!corpus_result.ok()) {
        std::fprintf(stderr, "corpus load failed: %s\n", corpus_result.status().message().c_str());
        return corpus_result.status().code() == mirador::ErrorCode::kBackendUnavailable ? 3 : 5;
    }
    const Corpus corpus = std::move(corpus_result).take_value();

    std::vector<const ArmSpec*> arms;
    if (const int resolved = resolve_arms(options, arms); resolved != 0) {
        return resolved;
    }
    std::vector<ImageBuffer> joined;
    if (const int built = build_joined_strips(arms, corpus, joined); built != 0) {
        return built;
    }

    PpOcrRecOptions rec_options;
    rec_options.param_path = options.rec_param;
    rec_options.bin_path = options.rec_bin;
    rec_options.charset_path = options.charset_path;
    rec_options.identity.model_id = "ocr-rec-experiment-rec";
    rec_options.identity.model_revision = "caller-provided";

    std::printf("MODEL rec_param=%s rec_bin=%s charset_entries=%zu corpus_lines=%zu reps=%d warmup=%d\n",
                options.rec_param.c_str(), options.rec_bin.c_str(), charset.value().size(), corpus.lines.size(),
                options.reps, options.warmup);

    std::vector<ArmResult> results;
    if (const int measured = measure_all_arms(options, rec_options, charset.value(), corpus, joined, arms, results);
        measured != 0) {
        return measured;
    }

    if (const int written = write_csv(options, results); written != 0) {
        return written;
    }

    SummarySink sink{stdout};
    if (!options.summary_path.empty() && !sink.open(options.summary_path)) {
        std::fprintf(stderr, "cannot open summary output: %s\n", options.summary_path.c_str());
        return 5;
    }
    for (const ArmResult& arm : results) {
        print_arm_summary(sink, arm);
        print_quality_line(sink, arm);
    }
    const ArmSpec serial_spec = kArmTable[0];
    const auto serial_it =
        std::find_if(results.begin(), results.end(), [](const ArmResult& arm) { return arm.spec == kArmTable.data(); });
    if (serial_it != results.end()) {
        print_serial_class_summary(sink, *serial_it, arm_rec_options(rec_options, serial_spec), corpus, options.reps);
    }

    // The sanity gate covers measurement validity only; an arm's quality
    // numbers are the experiment OUTPUT, never a gate.
    int exit_code = 0;
    if (const auto coverage = baseline_coverage(results); coverage.has_value()) {
        sink.line("GATE determinism=pass baseline_text_coverage=%.4f\n", coverage.value());
        if (coverage.value() < 0.5) {
            std::fprintf(stderr, "gate failure: baseline text coverage %.3f below 0.5\n", coverage.value());
            exit_code = 4;
        }
    }
    if (exit_code == 0 && want_dense) {
        exit_code = run_dense_arm(options, corpus);
    }
    if (exit_code == 0) {
        sink.line("RESULT ok\n");
    }
    return exit_code;
}
