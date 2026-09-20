# Geometry

## Use It For

Line segments and closed-shape region proposals outside the agent loop: road boundaries, table separators, panel outlines, or anywhere "where are the rectangular structures" matters.

## Minimal Usage

```cpp
using namespace mirador;

SegmentGrowingLineDetector detector;   // first-party, dependency-free
LineSegmentSet segments = detector.detect(gray_view, LineDetectRequest{}, {}).value();

LineFilterParams horizon;              // keep near-horizontal, at least 30 px
horizon.max_angle_deg = 5.0;
horizon.min_length = 30.0;
std::vector<LineSegment> lines = filter_segments(segments.segments, horizon).value();

CollinearMergeParams merge;            // join dashed pieces into long lines
merge.distance_tolerance = 3.0;
merge.gap_tolerance = 8.0;
std::vector<LineSegment> merged = merge_collinear(lines, merge).value();

GeometricProposalParams proposal;
std::vector<GeometricRegionProposal> regions =
    propose_regions(merged, proposal, {}).value();
// each proposal: tight_bounds, context_bounds, closure_score, rectangularity,
// edge_support, oriented_bounds, supporting_segments
```

The runnable tour is `examples/road_segments_tour.cpp`.

## Integration Pitfalls

- The `LineDetector` SPI input is strictly `kGray8`; color conversion is a `mirador::image` (or caller) concern — feeding color returns `kUnsupportedFormat`.
- Results are geometric facts only: a proposal never carries a semantic label. Deciding "this is a dialog" belongs to OCR, detection, accessibility or fusion downstream.
- `propose_regions` links segments by endpoints only; a T-contact through a segment's interior does not join two structures.
- `context_bounds` is `tight_bounds` grown by padding ratios and is not clamped to any frame — intersect it with your frame ROI yourself.
- Budgets are errors, not truncation: more than `max_segments` (1024, the linking pass is quadratic) or more than `max_proposals` (64) qualifying structures fail with `kBudgetExceeded`.
- The proposal contract is frozen and covered by the compatibility register since v0.3.0 (DEC-018 stage 1); temporal stability and fusion integration are deliberately absent for now.

## Next

Return to the entry router, or read [visual index](visual-index.md) to search patches by appearance instead of geometry.
