# Fusion And Snapshots

## Use It For

Merging OCR text, detections, visual-index template hits, and caller-supplied regions (accessibility services) into one `SemanticSnapshot`; keeping region identities across frames; refusing stale references.

## Minimal Usage

```cpp
ExternalRegion button;                 // platform facts only Mirador can never infer
button.bounds = RectF{40.0F, 120.0F, 80.0F, 24.0F};
button.role = "button";
button.interactive = true;

EvidenceSet evidence;
static_cast<void>(evidence.add_external(button));    // space defaults to kOriented
for (const TextRegion& line : lines) {
    static_cast<void>(evidence.add_text(line));
}
// add_detection(DetectionRegion) / add_template(entry_id, bounds, similarity) likewise

SemanticSnapshot snapshot = session.fuse(frame, evidence).value();
for (const VisualRegion& region : snapshot.regions) {
    // region.stable_id survives small layout shifts across fusions
}
```

`session.fuse` assigns stable IDs through the session tracker and publishes the snapshot; `session.latest_snapshot()` returns the current immutable one. A stored reference stays valid while `is_generation_current(snapshot, generation_held_by_the_reference)` holds; check it before acting.

## Integration Pitfalls

- External regions carry the platform semantics (`role`, `interactive`, `enabled`); vision output never produces these. Do not infer clickability from a detector label.
- Evidence may live in `kFrame`, `kOriented`, or `kDisplay` space (declared per item), but any `kDisplay` involvement requires `FusionOptions::display_transform` (frozen as kOriented -> kDisplay) — even when no numeric conversion is needed — or the call fails with `kInvalidArgument`.
- Association runs IoU and containment gates only; center proximity alone never merges two evidence items. Tune `iou_threshold` (0.5) / `containment_threshold` (0.8) rather than pre-merging boxes yourself.
- Fusion is deterministic for equal input, and the returned `FusionTrace` explains every merge; treat it as diagnostics you own — never log evidence text by default (privacy rule).
- `stable_id` values are unique within one session only; never compare IDs across sessions. Large changes allocate a fresh ID and bump the snapshot `generation`.
- The stateless `fuse_evidence()` free function returns regions with `stable_id == 0` — use `PerceptionSession::fuse` when you need tracked identities.
- Budgets: an `EvidenceSet` holds at most 4096 items and fusion emits at most `max_regions` (1024); exceeding either fails with `kBudgetExceeded`, nothing is silently dropped.

## Next

Return to the entry router, or read [set-of-mark](set-of-mark.md) to render the snapshot for a multimodal model.
