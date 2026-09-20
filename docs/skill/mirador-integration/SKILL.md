---
name: mirador-integration
description: Integrate the Mirador C++20 vision library into an application. Use when adding change detection, OCR or object detection through injected backends, result caching across unchanged frames, evidence fusion with accessibility data, stable region IDs and generations, Set-of-Mark overlays for a VLM, line segments or geometric region proposals, or visual index lookups to an application that depends on Mirador.
---

# Mirador Integration

Use this skill as an application developer. Public headers under `include/mirador/` are authoritative. Load one router and one card only; do not read unrelated cards or implementation sources.

## Route The Request

| Request contains | Read exactly this next |
| --- | --- |
| First integration, CMake target, first frame, or the minimal session loop | [Quick start](references/quick-start.md) |
| Capture buffers, rotation, stride, ownership, or coordinate spaces | [Frames and coordinates](references/frames-and-coordinates.md) |
| A real OCR, detector, or line-detector implementation | [Backends](references/backends.md) |
| Change gating, cache reuse, refresh, or ignore regions | [Session and caching](references/session-and-caching.md) |
| Fusion, stable IDs, snapshots, or stale-reference detection | [Fusion and snapshots](references/fusion-and-snapshots.md) |
| A numbered overlay for a VLM or resolving "click item N" | [Set-of-Mark](references/set-of-mark.md) |
| Line segments, filtering, merging, or closed-shape proposals | [Geometry](references/geometry.md) |
| Icon or patch lookup across frames | [Visual index](references/visual-index.md) |
| An application feature or workload not named above | [By scenario](references/scenarios.md) |
| A requirement, constraint, or failure concern | [By requirement](references/by-requirement.md) |
| A known API, type, status, or error term | [By API](references/by-api.md) |

After a router selects a card, implement its minimal usage, preserve its integration pitfalls, and build the application. Observe success through the card's named `Result` values, status codes, or counters; a cache hit is proven by backend call count, and staleness by `is_generation_current`, never inferred from silence.

## Downstream Use

Read [adoption](references/adoption.md) only when the AI runs from a downstream project and cannot already access this skill. Do not load implementation internals unless reproducing a library defect.
