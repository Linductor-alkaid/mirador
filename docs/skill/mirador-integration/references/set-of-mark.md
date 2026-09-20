# Set-Of-Mark

## Use It For

A numbered overlay that lets a multimodal model answer "click item 3" and your code resolve that number back to a region — without the model ever inventing coordinates.

## Minimal Usage

```cpp
// Fuse in the same space as the view you render (regions are read as
// background pixel coordinates).
SemanticSnapshot snapshot = session.fuse(frame, evidence, fusion_options).value();

SetOfMarkResult marks = render_set_of_mark(frame.image, snapshot).value();
ImageView marked = marked_image_view(marks.image);  // packed RGB8, non-owning view
for (const SoMMark& mark : marks.marks) {
    // mark.mark_id is the 1-based snapshot region order (deterministic);
    // mark.stable_id is what it refers to; mark.anchor is the click target.
}
```

Resolving a VLM answer — carry the generation with the number and refuse stale ones:

```cpp
bool resolve(const SemanticSnapshot& snapshot, uint64_t claimed_generation, uint32_t mark_id) {
    if (!is_generation_current(snapshot, claimed_generation)) {
        return false;  // the reference predates a big change: re-observe, never act
    }
    const SoMMark* mark = lookup(marks, mark_id);          // your map from the render call
    return mark != nullptr && find_region(snapshot, mark->stable_id) != nullptr;
}
```

The runnable end-to-end version, including a refused stale reference, is `examples/hybrid_localization_tour.cpp`.

## Integration Pitfalls

- The snapshot's regions are interpreted in background pixel coordinates: fuse with `FusionOptions::target_space` matching the view you pass, or the boxes land in the wrong place with no error.
- `background` must be a valid single-plane 8-bit view in rotation `k0`; NV12 and rotated buffers are rejected.
- `mark_id` numbering is the snapshot's region order — stable for a given snapshot, not across snapshots. Persist `(generation, mark_id)` pairs, never bare numbers.
- Regions outside the image still emit their mapping with an empty `drawn_bounds`; check it before drawing your own markers.
- The renderer never calls a VLM and never executes actions — model calls and clicking are your integration's job.
- Budgets: output over `max_bytes` (64 MiB default) or more than `max_marks` (256) regions fails with `kBudgetExceeded`.

## Next

Return to the entry router, or read [fusion and snapshots](fusion-and-snapshots.md) for how the generation counter behaves.
