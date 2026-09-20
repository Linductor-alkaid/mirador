# Backends

## Use It For

Connecting a real OCR engine, object/UI detector, or line detector. Mirador ships no models; you implement the small synchronous SPI and inject the instance.

## Minimal Usage

```cpp
class PpOcrBackend final : public OcrBackend {
public:
    BackendInfo info() const override {
        // name/version/model/revision must reflect the currently loaded weights:
        // every string feeds the result-cache keys (RULE-07).
        return {"ppocr-mobile", "1.2.0", "ppocrv4-det-rec", "815a02cd",
                {PixelFormat::kBgr8, PixelFormat::kGray8}, false};
    }

    Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest& request,
                                              const ExecutionContext& context) override {
        // `prepared` is already cropped, format-converted and downscaled.
        // Return boxes in prepared-image pixel space ([0,w) x [0,h]);
        // the session recovers them to request.output_space.
        ...
    }
};

PpOcrBackend ocr;
std::vector<TextRegion> lines = session.run_ocr(frame, &ocr, request).value();
```

`DetectorBackend` mirrors this with `detect(prepared, DetectionRequest, context) -> vector<DetectionRegion>`. `LineDetector::detect` takes a `kGray8` view (convert first; anything else returns `kUnsupportedFormat`) and returns a `LineSegmentSet` in presented-view pixel space. Reference ncnn implementations for PP-OCR and YOLO live under `integrations/` (opt-in build).

## Integration Pitfalls

- Backends must be deterministic for equal inputs: results are cached and replayed, so nondeterminism silently corrupts reuse.
- Honor `request.min_confidence`, poll `context`, and return `kCancelled`/`kTimeout` explicitly; never swallow them or run past a deadline.
- Never interpret the pipeline fields (`roi`, `roi_space`, `max_side`, `output_space`, `cache_policy`) — cropping, letterboxing, crop-refine and coordinate recovery belong to the session.
- Bump `BackendInfo::model_revision` whenever loaded weights change; a stale revision keeps serving old cached results with no error.
- `accepted_formats` is preference order; the session converts to the first accepted format it can reach. `validate(info)` failing means `kBackendUnavailable` — the backend cannot be used at all.
- `thread_safe = false` (the default) means one call at a time; sharing an instance across threads requires the implementation to declare it. Mirador never locks around backend calls.
- Every `info()` call must reflect the currently loaded model state; it is queried by value on the hot path for cache keys and gating.

## Next

Return to the entry router, or read [session and caching](session-and-caching.md) for how prepared views and cached results are produced.
