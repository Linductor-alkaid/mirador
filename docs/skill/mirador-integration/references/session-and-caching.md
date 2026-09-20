# Session And Caching

## Use It For

The per-source perception loop: gate work on change detection, reuse results while the screen is unchanged, force a fresh run for one request, and exclude animation regions.

## Minimal Usage

```cpp
ChangeDetectionParams params;
params.ignored_regions = {RectI{280, 0, 40, 20}};  // clock area, current-frame pixels

ChangeReport change = session.analyze_change(frame, params).value();
if (change.classification != ChangeClassification::kNone) {
    OcrRequest request;
    request.roi = RectF{0.0F, 100.0F, 320.0F, 100.0F};  // nullopt = whole frame
    request.output_space = CoordinateSpaceId::kOriented;
    request.cache_policy = CachePolicy::kReadWrite;     // kRefresh forces a re-run
    std::vector<TextRegion> lines = session.run_ocr(frame, &ocr, request).value();
}
```

`run_detector` is identical for `DetectionRequest`. Cancellation and deadlines ride in an `ExecutionContext` you pass per call: `{}` for none, or `{.is_cancelled = flag, .deadline = std::chrono::steady_clock::now() + 500ms}` — a hit surfaces as `kCancelled`/`kTimeout`, never a hang.

## Integration Pitfalls

- One session per image source (window, display, camera). A session is deliberately not thread-safe; keep it in one scheduling context. Published snapshots are the concurrently readable part.
- The first `analyze_change` of a session reports `kGlobal` with reason `kFirstFrame`: treat every pre-existing result as stale.
- Changing frames never evicts cached results — keys carry the frame fingerprint, so identical content re-hits later and changed content misses naturally. Do not build eviction logic on top of the classification.
- `CachePolicy::kRefresh` overwrites the entry other `kReadWrite` requests read; the policy is execution control, not part of the key. A different `roi`, `max_side`, `backend_params`, or a new `model_revision` already produces a different key.
- Cache hits return regions identical to the stored ones; if you need to know that a call executed, count backend invocations, not region differences.
- Cache budgets (`frame_cache_bytes`, `result_cache_bytes`) default to 4/16 MiB and non-positive values are rejected, not clamped. Overflow surfaces as `kBudgetExceeded` — budgets are explicit errors, never silent truncation.
- `analyze_change` stores only a compact signature of the frame (fingerprint + thumbnail), never the pixels.

## Next

Return to the entry router, or read [fusion and snapshots](fusion-and-snapshots.md) to turn these results into a tracked snapshot.
