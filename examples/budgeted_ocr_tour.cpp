// M8-02/M8-04 (DEC-025): runtime-free example of the budgeted OCR loop. A
// deterministic fake BudgetedOcrBackend feeds six candidate lines through
// PerceptionSession::run_ocr_budgeted with a line limit (K), caller priority
// and a soft budget; the tour prints the per-line completion states, the
// deferred (unrecognized) geometry that stays queryable, and the explicit stop
// reason. It also shows the boundary rules: the budgeted entry never touches
// the capability result cache, a hard deadline fails with kTimeout instead of
// a partial report, and the old all-or-nothing run_ocr keeps working unchanged
// on the same session. No model, no runtime, no threads, nothing persisted.

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using mirador::BackendInfo;
using mirador::BudgetedOcrBackend;
using mirador::BudgetedOcrRequest;
using mirador::ExecutionContext;
using mirador::Frame;
using mirador::ImageView;
using mirador::OcrBackend;
using mirador::OcrBatchRequest;
using mirador::OcrBatchResult;
using mirador::OcrCompletionState;
using mirador::OcrLineRecord;
using mirador::OcrRequest;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::TextRegion;

constexpr int32_t kWidth = 320;
constexpr int32_t kHeight = 200;
constexpr int64_t kStride = static_cast<int64_t>(kWidth) * 4;
constexpr int32_t kLineCount = 6;

/// Six stacked candidate lines covering the upper half; every line scores 0.9.
std::vector<RectF> candidate_lines() {
    std::vector<RectF> lines;
    lines.reserve(kLineCount);
    for (int32_t index = 0; index < kLineCount; ++index) {
        lines.push_back(RectF{20.0F, 10.0F + static_cast<float>(index) * 15.0F, 280.0F, 12.0F});
    }
    return lines;
}

/// Old-style all-or-nothing stub: the DEC-012 contract, unchanged since M2.
class StubOcrBackend final : public OcrBackend {
public:
    [[nodiscard]] BackendInfo info() const override { return info_; }

    mirador::Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest& /*request*/,
                                                       const ExecutionContext& /*context*/) override {
        std::vector<TextRegion> regions;
        regions.reserve(kLineCount);
        const float scale_x = static_cast<float>(prepared.width) / static_cast<float>(kWidth);
        const float scale_y = static_cast<float>(prepared.height) / static_cast<float>(kHeight);
        for (const RectF& line : candidate_lines()) {
            TextRegion region;
            region.bounds = RectF{line.x * scale_x, line.y * scale_y, line.width * scale_x, line.height * scale_y};
            region.utf8_text = "stub";
            region.confidence = 0.9F;
            regions.push_back(std::move(region));
        }
        return regions;
    }

private:
    BackendInfo info_{"stub-ocr", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
};

/// Fake budgeted backend: fixed candidate set, recognition "costs" one unit,
/// honoring K, priorities, soft budget (zero budget defers everything after
/// det) and the hard deadline via the shared helpers' checks.
class StubBudgetedBackend final : public BudgetedOcrBackend {
public:
    [[nodiscard]] BackendInfo info() const override { return info_; }

    mirador::Result<OcrBatchResult> recognize_budgeted(const ImageView& prepared, const OcrBatchRequest& request,
                                                       const ExecutionContext& context) override {
        if (const mirador::Result<void> valid = mirador::validate_ocr_batch_request(request); !valid.ok()) {
            return valid.status();
        }
        if (prepared.format != PixelFormat::kRgba8) {
            return mirador::Status{mirador::ErrorCode::kUnsupportedFormat, "stub accepts kRgba8 only"};
        }

        // Candidates in reading order with candidate ids.
        std::vector<OcrLineRecord> lines;
        const float scale_x = static_cast<float>(prepared.width) / static_cast<float>(kWidth);
        const float scale_y = static_cast<float>(prepared.height) / static_cast<float>(kHeight);
        for (const RectF& line : candidate_lines()) {
            OcrLineRecord record;
            record.bounds = RectF{line.x * scale_x, line.y * scale_y, line.width * scale_x, line.height * scale_y};
            record.det_confidence = 0.9F;
            lines.push_back(record);
        }
        std::sort(lines.begin(), lines.end(), mirador::ocr_line_reading_less);
        for (size_t index = 0; index < lines.size(); ++index) {
            lines[index].candidate_id = static_cast<int32_t>(index);
        }

        // Execution order with priorities, then bounded recognition.
        std::vector<size_t> execution(lines.size());
        for (size_t index = 0; index < execution.size(); ++index) {
            execution[index] = index;
        }
        std::sort(execution.begin(), execution.end(), [&](size_t lhs, size_t rhs) {
            const float p_lhs = mirador::resolve_ocr_line_priority(lines[lhs].bounds, request.priorities);
            const float p_rhs = mirador::resolve_ocr_line_priority(lines[rhs].bounds, request.priorities);
            return mirador::ocr_line_executes_before(lines[lhs], lines[rhs], p_lhs, p_rhs, request.selection);
        });
        const int32_t selected = std::min(request.max_rec_lines, static_cast<int32_t>(lines.size()));

        OcrBatchResult result;
        int32_t executed = 0;
        bool soft_stopped = false;
        for (int32_t order = 0; order < selected; ++order) {
            if (mirador::is_cancelled(context)) {
                return mirador::Status{mirador::ErrorCode::kCancelled, "stub cancelled"};
            }
            if (mirador::deadline_reached(context)) {
                return mirador::Status{mirador::ErrorCode::kTimeout, "stub deadline reached"};
            }
            if (request.soft_budget.has_value() && *request.soft_budget == std::chrono::steady_clock::duration{0}) {
                soft_stopped = true;  // scripted: no budget left for any unit
                break;
            }
            OcrLineRecord& record = lines[execution[static_cast<size_t>(order)]];
            record.state = OcrCompletionState::kRecognized;
            record.utf8_text = "stub line";
            record.rec_confidence = 0.9F;
            record.execution_order = order;
            ++executed;
        }

        for (const OcrLineRecord& line : lines) {
            if (line.state == OcrCompletionState::kRecognized) {
                ++result.recognized_count;
            } else {
                ++result.deferred_count;
            }
        }
        result.lines = std::move(lines);
        result.detected_count = kLineCount;
        result.rec_forward_count = executed;
        if (soft_stopped) {
            result.stop_reason = mirador::OcrBatchStopReason::kSoftBudget;
        } else if (executed < kLineCount) {
            result.stop_reason = mirador::OcrBatchStopReason::kRecLineLimit;
        } else {
            result.stop_reason = mirador::OcrBatchStopReason::kCompleted;
        }
        result.completion =
            result.deferred_count > 0 ? mirador::OcrBatchCompletion::kPartial : mirador::OcrBatchCompletion::kComplete;
        return result;
    }

private:
    BackendInfo info_{"stub-budgeted-ocr", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
};

Frame frame_over(std::vector<std::byte>& bytes, uint64_t sequence) {
    Frame frame;
    frame.image.data = bytes.data();
    frame.image.width = kWidth;
    frame.image.height = kHeight;
    frame.image.row_stride_bytes = kStride;
    frame.image.format = PixelFormat::kRgba8;
    frame.sequence = sequence;
    frame.source_id = "demo-budget";
    frame.owner = std::make_shared<const std::vector<std::byte>>(bytes);
    return frame;
}

void print_report(const mirador::BudgetedOcrReport& report, const char* label) {
    std::printf("%s: detected=%d recognized=%d deferred=%d stop=%d completion=%d forwards=%d\n", label,
                report.batch.detected_count, report.batch.recognized_count, report.batch.deferred_count,
                static_cast<int>(report.batch.stop_reason), static_cast<int>(report.batch.completion),
                report.batch.rec_forward_count);
    for (const OcrLineRecord& line : report.batch.lines) {
        std::printf("  candidate %d state=%d order=%d bounds=(%.0f,%.0f %.0fx%.0f)%s%s\n", line.candidate_id,
                    static_cast<int>(line.state), line.execution_order, line.bounds.x, line.bounds.y, line.bounds.width,
                    line.bounds.height,
                    line.utf8_text.empty() ? "" : " text=", line.utf8_text.empty() ? "" : line.utf8_text.c_str());
    }
}

}  // namespace

int main() {
    mirador::PerceptionSessionOptions options;
    options.source_id = "demo-budget";
    mirador::PerceptionSession session = mirador::PerceptionSession::create(options).take_value();

    std::vector<std::byte> bytes(static_cast<size_t>(kStride) * kHeight, std::byte{230});
    const Frame frame = frame_over(bytes, 1);

    // 1. The old all-or-nothing entry keeps working unchanged on this session
    //    (DEC-025 section 2: old backends and consumers need no adaptation).
    StubOcrBackend old_backend;
    OcrRequest old_request;
    const std::vector<TextRegion> complete = session.run_ocr(frame, &old_backend, old_request).value();
    std::printf("old run_ocr: %zu region(s) — all-or-nothing semantics unchanged\n", complete.size());
    const size_t cache_entries_after_old = session.result_cache().entry_count();

    // 2. Budgeted run with K=4: four lines recognized, two deferred — but the
    //    report keeps all six geometries, so deferred lines stay queryable.
    StubBudgetedBackend budgeted;
    BudgetedOcrRequest request;
    request.max_rec_lines = 4;
    const mirador::BudgetedOcrReport limited = session.run_ocr_budgeted(frame, &budgeted, request).value();
    print_report(limited, "k=4");
    std::printf("k=4 deferred bounds remain queryable: candidate %d at (%.0f, %.0f)\n",
                limited.batch.lines.back().candidate_id, limited.batch.lines.back().bounds.x,
                limited.batch.lines.back().bounds.y);

    // 3. A priority ROI boosts the LAST line into the selected set although it
    //    would execute last in plain reading order.
    BudgetedOcrRequest prioritized;
    prioritized.max_rec_lines = 2;
    mirador::OcrPriorityEntry focus;
    focus.roi = RectF{20.0F, 85.0F, 280.0F, 15.0F};  // covers line index 5
    focus.priority = 1.0F;
    prioritized.priorities = {focus};
    const mirador::BudgetedOcrReport boosted = session.run_ocr_budgeted(frame, &budgeted, prioritized).value();
    const OcrLineRecord& first_executed =
        *std::find_if(boosted.batch.lines.begin(), boosted.batch.lines.end(),
                      [](const OcrLineRecord& line) { return line.execution_order == 0; });
    std::printf("priority: candidate %d executes first (bounds y=%.0f)\n", first_executed.candidate_id,
                first_executed.bounds.y);

    // 4. A zero soft budget stops before any recognition unit: everything is
    //    deferred with an explicit kSoftBudget stop — geometry-only report.
    BudgetedOcrRequest starved;
    starved.max_rec_lines = 4;
    starved.soft_budget = std::chrono::steady_clock::duration{0};
    const mirador::BudgetedOcrReport deferred = session.run_ocr_budgeted(frame, &budgeted, starved).value();
    print_report(deferred, "soft-budget=0");

    // 5. A hard deadline fails the whole call with kTimeout — a partial report
    //    is never wrapped as success, and nothing lands in the caches.
    BudgetedOcrRequest timed_out;
    timed_out.max_rec_lines = 4;
    timed_out.finalize_reserve = std::chrono::milliseconds{100};
    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{1};
    const mirador::Result<mirador::BudgetedOcrReport> failed =
        session.run_ocr_budgeted(frame, &budgeted, timed_out, expired);
    std::printf("expired deadline: ok=%d code=%d\n", failed.ok() ? 1 : 0,
                failed.ok() ? 0 : static_cast<int>(failed.status().code()));

    // 6. Cache isolation: the budgeted entries never touched the capability
    //    cache (only the old run_ocr insertion is visible), and a null
    //    budgeted backend fails explicitly instead of falling back.
    std::printf("cache entries: after old run=%zu, after budgeted runs=%zu\n", cache_entries_after_old,
                session.result_cache().entry_count());
    const mirador::Result<mirador::BudgetedOcrReport> unavailable = session.run_ocr_budgeted(frame, nullptr, request);
    std::printf("null budgeted backend: code=%d (kBackendUnavailable=%d)\n",
                unavailable.ok() ? 0 : static_cast<int>(unavailable.status().code()),
                static_cast<int>(mirador::ErrorCode::kBackendUnavailable));
    return 0;
}
