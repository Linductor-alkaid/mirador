#include "ppocr_budgeted.hpp"

#include "ncnn_runtime.hpp"
#include "ppocr_backend.hpp"
#include "ppocr_internal.hpp"

#include <mirador/crop.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/text_postprocess.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace mirador::integrations {

namespace {

/// Nanosecond accumulator for one diagnostics phase: `stop()` ends the phase
/// (idempotent); the destructor stops what is left, so error paths stay
/// accounted too. Benchmark accounting only, never results or logs.
class PhaseClock {
public:
    PhaseClock(PpOcrStageTimings* timings, int64_t PpOcrStageTimings::*slot) noexcept
        : timings_(timings), slot_(slot), begin_(std::chrono::steady_clock::now()) {}

    void stop() noexcept {
        if (timings_ != nullptr) {
            *timings_.*slot_ +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin_).count();
            timings_ = nullptr;
        }
    }
    ~PhaseClock() noexcept { stop(); }

    PhaseClock(const PhaseClock&) = delete;
    PhaseClock& operator=(const PhaseClock&) = delete;

private:
    PpOcrStageTimings* timings_;
    int64_t PpOcrStageTimings::*slot_;
    std::chrono::steady_clock::time_point begin_;
};

/// Runs the frozen det stage and returns the candidate boxes in prepared-image
/// space (post min_confidence, pad/scale recovered), together with the
/// truncation diagnostics. `max_boxes` is the caller's det policy: the DB
/// postprocess runs at its 65535-box domain maximum and this stage truncates
/// to the policy cap, so any natural density up to the domain bound yields a
/// kept prefix with an observable `det_limit_hit` (DEC-025: det coverage
/// uncertainty must be visible; M8-06 revised the former det_limit+1 probe
/// whose room the frozen postprocess could exceed into a whole-call failure).
/// The kept prefix stays bitwise identical to a plain limit-capped run
/// whenever that run would succeed; only true 65535-box saturation still
/// fails explicitly like the frozen postprocess.
struct DetStageOutput {
    std::vector<TextRegion> boxes;  ///< reading-order ready: recovered, filtered
    int32_t det_limit = 0;
    bool det_limit_hit = false;
};

Result<DetStageOutput> run_det_stage(const PpOcrDetOptions& options, const ImageView& prepared_image,
                                     const OcrBatchRequest& request, NcnnRuntime& runtime,
                                     const ExecutionContext& context, PpOcrStageTimings* timings) {
    PhaseClock canvas_phase{timings, &PpOcrStageTimings::det_canvas_ns};
    auto canvas = ppocr_detail::make_det_canvas(prepared_image, options.det_side, options.work_budget_bytes);
    if (!canvas.ok()) {
        return canvas.status();
    }
    const int32_t canvas_w = canvas.value().buffer.width();
    const int32_t canvas_h = canvas.value().buffer.height();
    canvas_phase.stop();

    PhaseClock pack_phase{timings, &PpOcrStageTimings::det_pack_normalize_ns};
    auto input_result = pack_image(canvas.value().buffer.view(), context);
    if (!input_result.ok()) {
        return input_result.status();
    }
    NcnnTensor input = std::move(input_result).take_value();
    if (const Result<void> swapped = ppocr_detail::swap_planes_to_bgr(input); !swapped.ok()) {
        return swapped.status();
    }
    if (const Result<void> normalized = ppocr_detail::normalize_ppocr(input, context); !normalized.ok()) {
        return normalized.status();
    }
    pack_phase.stop();

    PhaseClock forward_phase{timings, &PpOcrStageTimings::det_forward_ns};
    auto output = runtime.run("x", input, "out", context);
    if (!output.ok()) {
        return output.status();
    }
    forward_phase.stop();

    PhaseClock post_phase{timings, &PpOcrStageTimings::det_postprocess_ns};
    const NcnnTensor& probability = output.value();
    if (probability.channels != 1 || probability.width != canvas_w || probability.height != canvas_h) {
        return Status{ErrorCode::kBackendFailure, "det model must output a 1 x canvas_h x canvas_w probability map"};
    }

    const size_t plane = static_cast<size_t>(probability.width) * probability.height;
    std::vector<uint8_t> probability_bytes(plane);
    for (size_t i = 0; i < plane; ++i) {
        const float value = probability.data[i];
        const auto scaled = static_cast<int>(std::lround(value * 255.0F));
        probability_bytes[i] = static_cast<uint8_t>(std::clamp(scaled, 0, 255));
    }
    ImageView probability_view;
    probability_view.data = reinterpret_cast<const std::byte*>(probability_bytes.data());
    probability_view.width = probability.width;
    probability_view.height = probability.height;
    probability_view.row_stride_bytes = probability.width;
    probability_view.format = PixelFormat::kGray8;

    const int32_t det_limit = options.postprocess.max_boxes;
    DbPostprocessParams probe_params = options.postprocess;
    // Probe at the postprocess domain maximum and truncate here: any natural
    // density up to 65535 boxes must yield a kept prefix with a visible
    // det_limit_hit, not a whole-call failure. The former det_limit+1 probe
    // relied on the frozen postprocess saturating past its cap, but the frozen
    // semantics fail the call once components exceed the cap — M8-06 real
    // density hit exactly that (a 529-slot desktop grid yields 530 candidates
    // > cap+1 and the budgeted call died on the scenes it exists for; M8-03
    // deferred this boundary to real-density data). kBudgetExceeded is now
    // reserved for true 65535-box saturation. The kept prefix stays bitwise
    // identical to a plain limit-capped run whenever that run would succeed
    // (component scan order does not depend on the cap).
    probe_params.max_boxes = 65535;
    auto boxed = db_postprocess_aabb(probability_view, probe_params);
    if (!boxed.ok()) {
        return boxed.status();
    }

    DetStageOutput stage;
    stage.det_limit = det_limit;
    stage.det_limit_hit = static_cast<int32_t>(boxed.value().size()) > det_limit;
    const size_t kept = stage.det_limit_hit ? static_cast<size_t>(det_limit) : boxed.value().size();
    for (size_t index = 0; index < kept; ++index) {
        const TextRegion& box = boxed.value()[index];
        if (box.confidence < request.min_confidence) {
            continue;
        }
        auto mapped = ppocr_detail::unpad_bounds(box.bounds, canvas.value().scale, canvas.value().pad_left,
                                                 canvas.value().pad_top, prepared_image);
        if (!mapped.ok()) {
            continue;  // box lived entirely in padding; deterministic drop
        }
        TextRegion region;
        region.bounds = mapped.take_value();
        region.confidence = box.confidence;
        stage.boxes.push_back(std::move(region));
    }
    post_phase.stop();
    return stage;
}

/// Clamps a det box into a representable crop ROI (same rule as the frozen
/// combined backend, M5-03).
RectI line_crop_roi(const RectF& bounds, const ImageView& image) noexcept {
    const auto crop_x = static_cast<int32_t>(std::lround(bounds.x));
    const auto crop_y = static_cast<int32_t>(std::lround(bounds.y));
    const auto crop_w = static_cast<int32_t>(std::lround(bounds.width));
    const auto crop_h = static_cast<int32_t>(std::lround(bounds.height));
    RectI roi;
    roi.x = std::clamp(crop_x, 0, static_cast<int32_t>(image.width) - 1);
    roi.y = std::clamp(crop_y, 0, static_cast<int32_t>(image.height) - 1);
    roi.width = std::clamp(crop_w, 1, static_cast<int32_t>(image.width) - roi.x);
    roi.height = std::clamp(crop_h, 1, static_cast<int32_t>(image.height) - roi.y);
    return roi;
}

}  // namespace

Result<PpOcrBudgetedBackend> PpOcrBudgetedBackend::create(const PpOcrBudgetedOptions& options) {
    if (options.det.det_side < 32 || options.det.det_side > 4096 || (options.det.det_side % 32) != 0) {
        return Status{ErrorCode::kInvalidArgument, "det_side must be a multiple of 32 in [32, 4096]"};
    }
    if (options.det.postprocess.max_boxes < 1 || options.det.postprocess.max_boxes > 65535) {
        return Status{ErrorCode::kInvalidArgument, "det max_boxes must be in [1, 65535]"};
    }
    if (options.rec.rec_height < 16 || options.rec.rec_height > 128 || options.rec.rec_width < 32 ||
        options.rec.rec_width > 1024) {
        return Status{ErrorCode::kInvalidArgument, "rec input size out of range"};
    }
    if (options.rec_unit_upper_bound.count() < 0) {
        return Status{ErrorCode::kInvalidArgument, "rec_unit_upper_bound must be non-negative"};
    }

    NcnnRuntimeOptions det_runtime_options;
    det_runtime_options.param_path = options.det.param_path;
    det_runtime_options.bin_path = options.det.bin_path;
    det_runtime_options.num_threads = options.det.num_threads;
    auto det_runtime = NcnnRuntime::create(det_runtime_options);
    if (!det_runtime.ok()) {
        return det_runtime.status();
    }

    NcnnRuntimeOptions rec_runtime_options;
    rec_runtime_options.param_path = options.rec.param_path;
    rec_runtime_options.bin_path = options.rec.bin_path;
    rec_runtime_options.num_threads = options.rec.num_threads;
    auto rec_runtime = NcnnRuntime::create(rec_runtime_options);
    if (!rec_runtime.ok()) {
        return rec_runtime.status();
    }

    auto charset = ppocr_detail::load_charset(options.rec.charset_path);
    if (!charset.ok()) {
        return charset.status();
    }

    PpOcrBudgetedBackend backend;
    backend.options_ = options;
    backend.det_runtime_ = std::move(det_runtime).take_value();
    backend.rec_runtime_ = std::move(rec_runtime).take_value();
    backend.charset_ = charset.take_value();
    backend.info_ = ppocr_detail::make_info("ppocr-budgeted-ncnn-reference", PpOcrModelIdentity{});
    backend.info_.model_id = options.det.identity.model_id + "+" + options.rec.identity.model_id;
    backend.info_.model_revision = options.det.identity.model_revision + "+" + options.rec.identity.model_revision;
    return backend;
}

BackendInfo PpOcrBudgetedBackend::info() const {
    return info_;
}

/// Deterministic reading order over the det candidates; candidate_id is the
/// index in this order (the report's line order).
std::vector<OcrLineRecord> reading_order_lines(std::vector<TextRegion>&& boxes) {
    std::vector<OcrLineRecord> lines;
    lines.reserve(boxes.size());
    for (const TextRegion& box : boxes) {
        OcrLineRecord record;
        record.bounds = box.bounds;
        record.det_confidence = box.confidence;
        lines.push_back(std::move(record));
    }
    std::sort(lines.begin(), lines.end(), ocr_line_reading_less);
    for (size_t index = 0; index < lines.size(); ++index) {
        lines[index].candidate_id = static_cast<int32_t>(index);
    }
    return lines;
}

/// Execution order over the candidates (DEC-025 section 4): caller priority
/// descending, then the strategy key, then the reading-order total order.
std::vector<size_t> execution_order(const std::vector<OcrLineRecord>& lines, const OcrBatchRequest& request) {
    std::vector<float> priorities(lines.size());
    for (size_t index = 0; index < lines.size(); ++index) {
        priorities[index] = resolve_ocr_line_priority(lines[index].bounds, request.priorities);
    }
    std::vector<size_t> execution(lines.size());
    std::iota(execution.begin(), execution.end(), size_t{0});
    std::sort(execution.begin(), execution.end(), [&](size_t lhs, size_t rhs) {
        return ocr_line_executes_before(lines[lhs], lines[rhs], priorities[lhs], priorities[rhs], request.selection);
    });
    return execution;
}

/// Outcome of the bounded recognition loop.
struct RecLoopOutcome {
    int32_t executed = 0;
    OcrBatchStopReason stop = OcrBatchStopReason::kCompleted;
};

/// Bounded recognition loop: per-unit cancel/deadline checks, then the soft
/// budget guards; a tripped guard stops NEW work and reports partial. Executed
/// lines are stamped kRecognized with their execution order.
Result<RecLoopOutcome> run_bounded_rec_loop(const PpOcrRecOptions& rec_options, const std::vector<std::string>& charset,
                                            const NcnnRuntime& rec_runtime, const ImageView& prepared_image,
                                            std::vector<OcrLineRecord>& lines, const std::vector<size_t>& execution,
                                            const OcrBatchRequest& request, const ExecutionContext& context,
                                            std::chrono::steady_clock::time_point entry_time, int32_t selected_count,
                                            std::chrono::steady_clock::duration rec_unit_upper_bound,
                                            PpOcrStageTimings& timings) {
    RecLoopOutcome outcome;
    for (int32_t order = 0; order < selected_count; ++order) {
        if (is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled during budgeted rec stage"};
        }
        if (deadline_reached(context)) {
            return Status{ErrorCode::kTimeout, "deadline reached during budgeted rec stage"};
        }
        const auto now = std::chrono::steady_clock::now();
        if (request.soft_budget.has_value() && now - entry_time > *request.soft_budget) {
            outcome.stop = OcrBatchStopReason::kSoftBudget;
            return outcome;
        }
        if (context.deadline.has_value()) {
            const auto remaining = *context.deadline - now;
            const auto unit_guard =
                rec_unit_upper_bound + request.finalize_reserve.value_or(std::chrono::steady_clock::duration{0});
            if (remaining < unit_guard) {
                outcome.stop = OcrBatchStopReason::kSoftBudget;
                return outcome;
            }
        }
        const size_t index = execution[static_cast<size_t>(order)];
        PhaseClock crop_phase{&timings, &PpOcrStageTimings::crop_ns};
        const RectI roi = line_crop_roi(lines[index].bounds, prepared_image);
        auto line_image = crop(prepared_image, roi, rec_options.work_budget_bytes);
        if (!line_image.ok()) {
            return line_image.status();
        }
        crop_phase.stop();
        ppocr_detail::RecUnitTiming unit_timing;
        auto line = ppocr_detail::run_rec_line(rec_options, charset, rec_runtime, line_image.value().view(), context,
                                               &unit_timing);
        timings.rec_preprocess_ns += unit_timing.preprocess_ns;
        timings.rec_forward_ns += unit_timing.forward_ns;
        timings.rec_decode_ns += unit_timing.decode_ns;
        if (!line.ok()) {
            return line.status();
        }
        auto decoded = std::move(line).take_value();
        OcrLineRecord& record = lines[index];
        record.state = OcrCompletionState::kRecognized;
        record.utf8_text = std::move(decoded.utf8_text);
        record.rec_confidence = decoded.confidence;
        record.execution_order = order;
        ++outcome.executed;
    }
    return outcome;
}

Result<OcrBatchResult> PpOcrBudgetedBackend::recognize_budgeted(const ImageView& prepared_image,
                                                                const OcrBatchRequest& request,
                                                                const ExecutionContext& context) {
    if (Status status = ppocr_detail::precheck(prepared_image, context); !status.ok()) {
        return status;
    }
    if (const Result<void> request_status = validate_ocr_batch_request(request); !request_status.ok()) {
        return request_status.status();
    }
    const auto entry_time = std::chrono::steady_clock::now();
    PpOcrStageTimings& timings = timings_;
    timings = PpOcrStageTimings{};

    // Detection stage (one atomic forward; kTimeout/kCancelled propagate).
    auto stage = run_det_stage(options_.det, prepared_image, request, det_runtime_, context, &timings);
    if (!stage.ok()) {
        return stage.status();
    }
    DetStageOutput det = std::move(stage).take_value();

    PhaseClock select_phase{&timings, &PpOcrStageTimings::select_ns};
    std::vector<OcrLineRecord> lines = reading_order_lines(std::move(det.boxes));
    const std::vector<size_t> execution = execution_order(lines, request);
    const int32_t selected_count = std::min<int32_t>(request.max_rec_lines, static_cast<int32_t>(lines.size()));
    select_phase.stop();

    auto loop = run_bounded_rec_loop(options_.rec, charset_, rec_runtime_, prepared_image, lines, execution, request,
                                     context, entry_time, selected_count, options_.rec_unit_upper_bound, timings);
    if (!loop.ok()) {
        return loop.status();
    }

    PhaseClock finalize_phase{&timings, &PpOcrStageTimings::finalize_ns};
    OcrBatchResult result;
    for (const OcrLineRecord& line : lines) {
        switch (line.state) {
            case OcrCompletionState::kRecognized:
                ++result.recognized_count;
                break;
            case OcrCompletionState::kReused:
                ++result.reused_count;
                break;
            case OcrCompletionState::kDeferred:
                ++result.deferred_count;
                break;
        }
    }
    result.lines = std::move(lines);
    result.detected_count = static_cast<int32_t>(result.lines.size());
    result.det_limit = det.det_limit;
    result.det_limit_hit = det.det_limit_hit;
    result.rec_forward_count = loop.value().executed;
    // Stop reason: an early soft stop wins; otherwise the line limit bound
    // (K < N with every selected line executed) or normal completion.
    if (loop.value().executed < selected_count) {
        result.stop_reason = loop.value().stop;
    } else if (selected_count < static_cast<int32_t>(result.lines.size())) {
        result.stop_reason = OcrBatchStopReason::kRecLineLimit;
    } else {
        result.stop_reason = OcrBatchStopReason::kCompleted;
    }
    result.completion = result.deferred_count > 0 ? OcrBatchCompletion::kPartial : OcrBatchCompletion::kComplete;
    finalize_phase.stop();
    return result;
}

}  // namespace mirador::integrations
