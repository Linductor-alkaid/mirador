# Budgeted OCR

## Use It For

Dense screens that cannot finish full OCR inside an interactive time budget: cap recognition work at K lines, prioritize caller-named regions, stop starting new work at a soft budget, and get back a report where every detected line has an explicit completion state — recognized with text, or deferred with its geometry still queryable. Frozen contract (DEC-025); the legacy `OcrBackend` all-or-nothing API is unchanged.

## Minimal Usage

```cpp
#include <mirador/perception_session.hpp>  // BudgetedOcrRequest/BudgetedOcrReport, run_ocr_budgeted

mirador::BudgetedOcrRequest request;
request.max_rec_lines = 64;                       // K; must be positive
request.selection = mirador::OcrSelectionStrategy::kReadingOrder;
request.soft_budget = std::chrono::milliseconds{3500};   // optional; stops NEW work, returns partial
request.finalize_reserve = std::chrono::milliseconds{500}; // optional; requires a hard deadline below

mirador::ExecutionContext context;
context.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};

// Focus: priority entries are ROIs in priority_space; matching lines run first.
mirador::OcrPriorityEntry focus;
focus.bounds = mirador::RectF{40, 120, 400, 60};
focus.priority = 1.0F;
request.priorities = {focus};
request.priority_space = mirador::CoordinateSpaceId::kOriented;

auto outcome = session.run_ocr_budgeted(frame, &budgeted_backend, request, context);
if (!outcome.ok()) {
    // kCancelled/kTimeout fail the WHOLE call — no partial report, no text.
    // kBackendUnavailable: the backend does not implement the budgeted SPI.
    return outcome.status();
}
const mirador::BudgetedOcrReport& report = outcome.value();
// report.batch.recognized_count + reused_count + deferred_count == detected_count
// report.geometry_space / executed_roi / source_id bind the report to the run.
for (const mirador::OcrLineRecord& line : report.batch.lines) {
    if (line.state == mirador::OcrCompletionState::kRecognized) {
        use(line.bounds, *line.rec_confidence, line.utf8_text);  // text may be legitimately empty
    } else if (line.state == mirador::OcrCompletionState::kDeferred) {
        query_later(line.bounds, line.det_confidence);           // geometry stays usable
    }
}
```

A direct backend call (`BudgetedOcrBackend::recognize_budgeted`) takes the core
`OcrBatchRequest`/`OcrBatchResult` in prepared-image space; the session entry
adds crop, downscale cap, coordinate recovery into `output_space`, priority
space conversion and request binding. Reference configuration frozen from the
M8-06 real-model benchmark: K=64, `finalize_reserve=500 ms`, single-thread rec;
`rec_unit_upper_bound` is a backend (not request) option.

## Integration Pitfalls

- Hard deadlines and cancellation never yield a report: `kTimeout`/`kCancelled` fail the whole call even when lines already executed. Only `kSoftBudget` (soft budget elapsed or the finalize guard tripped) and `kRecLineLimit` produce partial reports. Never map `kTimeout` to "no text found".
- `completion == kPartial` reports facts, not permission: deciding whether a downstream agent may still act on a partial screen is the caller's policy.
- "Not recognized" is `state == kDeferred`, never an empty `utf8_text` — a recognized line may decode to empty text.
- `kComplete` covers the detected candidates only. Check `det_limit_hit` before claiming full-screen recall: the det stage may have truncated candidates (`det_limit`, `det_limit_hit`).
- The budgeted entry never reads or writes the capability result cache, and cannot pollute the legacy `run_ocr` cache. Reuse (`kReused`) is reserved for a later decision; M8 implementations must not emit it.
- `finalize_reserve` without a hard deadline is rejected at the session entry (`kInvalidArgument`); budgets are never enlarged automatically.
- One rec forward is atomic and uninterruptible; size `soft_budget`/`finalize_reserve` with the backend's measured worst unit in mind (see the OCR budget benchmark protocol in `docs/benchmarks/`).

## Next

[Backends](backends.md) to implement the `BudgetedOcrBackend` SPI over a real
runtime; [Session and caching](session-and-caching.md) for the enclosing
session loop; [By requirement](by-requirement.md) for cancellation and budget
concerns.
