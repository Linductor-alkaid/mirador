# Visual Index

## Use It For

Recognizing "is this the same icon/patch I enrolled" across frames under noise — exact hash, perceptual hash, then template NCC as a three-tier fallback — without running a model.

## Minimal Usage

```cpp
using namespace mirador;

VisualIndex index = VisualIndex::create(int64_t{64} * 1024, /*thumb_side=*/32).take_value();

PatchFingerprintParams fp_params;
fp_params.thumb_side = 32;

// Enrollment: one fingerprint per known icon state.
auto on_fp = make_visual_patch_fingerprint(view_of(icon_on), fp_params, 1 << 20).value();
static_cast<void>(index.insert(1001 /* entry id */, on_fp));  // Result<void>: check ok()

// A later frame: fingerprint the patch the same way and query.
VisualQueryParams q;
q.perceptual_similarity_threshold = 0.9;
q.template_ncc_threshold = 0.9;
q.max_candidates = 4;
std::vector<VisualCandidate> hits = index.query(probe_fp, q).value();
// hits[].entry_id / .evidence (kExactContent, kPerceptualHash, kTemplate) / similarity
```

The runnable tour with a noise-robustness check is `examples/icon_state_index_tour.cpp`.

## Integration Pitfalls

- Build enrollment and probe fingerprints with identical `PatchFingerprintParams`; the index is created for one `thumb_side` and mismatched thumbnails weaken every tier.
- Define your reuse policy caller-side. Accept a hit as the same icon only above a strict similarity (the example demands >= 0.95); weaker hits stay ambiguous and belong to real recognition.
- `query` is non-const — it promotes entries between tiers — so keep the index mutable through the query path.
- Fingerprinting the raw patch allocates bounded intermediates; pass an honest `max_bytes` and expect `kBudgetExceeded` rather than truncation.
- The index is bounded by `max_bytes` with defined eviction; check `contains(entry_id)` before trusting an old enrollment still exists.
- Index hits are evidence, not truth: feed strong matches into fusion via `EvidenceSet::add_template(entry_id, bounds, similarity)` so they land in the snapshot with source `kTemplate` and a stable ID.

## Next

Return to the entry router, or read [fusion and snapshots](fusion-and-snapshots.md) to fold template hits into the tracked snapshot.
