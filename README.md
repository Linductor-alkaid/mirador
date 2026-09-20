[English](README.md) | [简体中文](README_zh.md)

# Mirador

A C++20 library that helps programs understand what is on the screen: it detects when the display changes, finds text, UI elements and line segments, and keeps track of each region across frames.

[![CI](https://github.com/Linductor-alkaid/mirador/actions/workflows/ci.yml/badge.svg)](https://github.com/Linductor-alkaid/mirador/actions/workflows/ci.yml)
[![Latest tag](https://img.shields.io/github/v/tag/Linductor-alkaid/mirador)](https://github.com/Linductor-alkaid/mirador/tags)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20Android-blue)

Mirador sits between your screen-capture code and your automation logic: you submit frames, and it tells you what changed, what is where, and which of your previous results are still valid. To keep repeated frames cheap, it fingerprints every frame and skips OCR / detection entirely when nothing relevant changed; when something did change, it runs the backends you injected and merges their output with regions you already know about (for example from an accessibility service) into a single snapshot. Every region in that snapshot carries a stable ID that survives small changes, so a button found in the previous frame can still be addressed after the layout shifts a little; when a region changes beyond recognition it gets a new ID, and old references can detect that they are outdated. The core library is plain C++20 with no model runtime, no threads and no network access — you supply the actual models through a small synchronous backend interface. It targets Android, Linux and Windows.

## Features

- **Change detection** — fingerprint each frame and classify it as unchanged / partially changed / fully changed; ignore regions (clocks, cursors, video overlays) can be excluded
- **Result caching** — while the screen is unchanged, previous OCR / detection results are reused; caches have byte limits, and cache keys cover model revision, ROI and preprocessing version
- **OCR / detection / line-detector SPI** — a few small synchronous interfaces; you inject the implementation (your own runtime, or the ncnn examples under `integrations/`)
- **Evidence fusion** — combine OCR text, detections, line segments and externally supplied regions into one snapshot
- **Stable region IDs** — regions keep their ID across frames; major changes get a new ID and bump a `generation` counter, so outdated references can be recognized
- **Set-of-Mark rendering** — draws a numbered box on each region and returns the number → ID map
- **Line segments & region proposals** — first-party line detector, angle/length filtering, collinear merging, closed-shape region proposals
- **Image utilities** — color conversion, resize/crop/letterbox, NMS, DB post-processing, text normalization
- **Visual index** — look up image patches by exact hash, perceptual hash, or template matching
- **Small footprint** — the core depends only on the C++20 standard library; the public API does not throw exceptions

## Quick Start

Build and run the perception-session example (Linux/macOS; on Windows use the `windows-debug` preset):

```bash
git clone --recursive https://github.com/Linductor-alkaid/mirador
cd mirador
cmake --preset release
cmake --build --preset release
ctest --preset release                                          # optional: full test suite
./build/release/examples/mirador_example_perception_session    # change → stub OCR → cache hit → refresh
```

Requirements: CMake ≥ 3.21 for the one-line presets (CMake ≥ 3.16 with `cmake -S . -B <dir>`), and GCC/Clang ≥ 10 (libstdc++ ≥ 10) or MSVC. The default build only needs the pinned GoogleTest submodule for tests; `-DMIRADOR_BUILD_TESTS=OFF` removes even that.

The same flow in code — condensed from [`examples/perception_session_tour.cpp`](examples/perception_session_tour.cpp), which is the runnable, fully commented version:

```cpp
#include <mirador/backend_info.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace mirador;

// Any real OCR engine can be injected through the synchronous SPI. This stub
// always reports one line covering the center of whatever view it receives.
class StubOcrBackend final : public OcrBackend {
public:
    BackendInfo info() const override {
        return {"stub-ocr", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
    }

    Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest&,
                                              const ExecutionContext&) override {
        TextRegion line;
        line.bounds = RectF{static_cast<float>(prepared.width) / 4.0F,
                            static_cast<float>(prepared.height) / 4.0F,
                            static_cast<float>(prepared.width) / 2.0F,
                            static_cast<float>(prepared.height) / 2.0F};
        line.utf8_text = "demo line";
        line.confidence = 0.8F;
        return std::vector<TextRegion>{std::move(line)};
    }
};

int main() {
    PerceptionSessionOptions options;
    options.source_id = "demo-screen";
    PerceptionSession session = PerceptionSession::create(options).take_value();
    StubOcrBackend ocr;

    // One 320x200 RGBA8 frame. `owner` keeps the pixels alive while the frame
    // is in flight; the view itself is non-owning.
    std::vector<std::byte> pixels(320 * 200 * 4, std::byte{0});
    Frame frame;
    frame.image.data = pixels.data();
    frame.image.width = 320;
    frame.image.height = 200;
    frame.image.row_stride_bytes = 320 * 4;
    frame.image.format = PixelFormat::kRgba8;
    frame.image.rotation = Rotation::k0;
    frame.sequence = 1;
    frame.source_id = "demo-screen";
    frame.owner = std::make_shared<const std::vector<std::byte>>(pixels);

    // 1. What changed? First submission => everything is new.
    ChangeReport change = session.analyze_change(frame).value();

    // 2. Run OCR over the lower half; results are recovered into the space
    //    you submitted.
    OcrRequest request;
    request.roi = RectF{0.0F, 100.0F, 320.0F, 100.0F};
    std::vector<TextRegion> lines = session.run_ocr(frame, &ocr, request).value();

    // 3. Same frame, same request => served from the result cache,
    //    no second backend execution.
    std::vector<TextRegion> cached = session.run_ocr(frame, &ocr, request).value();

    std::printf("classification=%d lines=%zu cached=%zu\n",
                static_cast<int>(change.classification), lines.size(), cached.size());
}
```

What happens here: the first `analyze_change` reports that everything is new; the first `run_ocr` executes the backend once; the second `run_ocr` with the identical request is answered from the cache without calling the backend again.

## Architecture

The core loop, from raw frame to published result:

```text
Frame (ImageView)
   │  submit
   ▼
Change detection ──── nothing relevant changed ───▶ reuse previous results
   │ partially / fully changed
   ▼
Run backends where needed (OCR / detection / lines, injected via the SPI)
   │  results mapped back to your coordinates and cached
   ▼
Fusion
   ▼
SemanticSnapshot (immutable; each region has a stable ID + generation)
   ▼
Set-of-Mark output / your code
```

Six CMake libraries, each can be enabled or disabled on its own:

| Library | What it contains |
| --- | --- |
| `mirador::core` | Base types, `Status`/`Result`, `ImageView`/`Frame`, coordinate transforms, backend interfaces |
| `mirador::image` | Color conversion, resize/crop/letterbox, fingerprints, change detection, detection/OCR post-processing |
| `mirador::cache` | Bounded frame and result caches, three-tier visual index |
| `mirador::geometry` | Line-segment interface, first-party detector, segment filtering and collinear merge, region proposals |
| `mirador::fusion` | Evidence fusion, stable IDs, generations, `PerceptionSession`, `SemanticSnapshot` |
| `mirador::render` | Set-of-Mark rendering, grid partition and coordinate mapping |

```text
include/mirador/        public API (contracts are documented in the header comments)
src/core|image|cache|geometry|fusion|render/
adapters/               optional: opencv, capture-linux / -windows / -android
integrations/           reference backends on ncnn (not fetched by default builds)
examples/               runtime-free tours of the public API
benchmarks/             performance and size measurement entry points
tests/                  unit, property, architecture, concurrency, privacy, fuzz
docs/                   design, plans, decisions, benchmarks, API index, compatibility
```

Dependencies only point one way: the optional modules and adapters build on the public interfaces, never the other way around. OpenCV, ncnn and platform capture code appear only under `adapters/` and `integrations/`, behind CMake options that are off by default. Architecture tests in CI keep the core free of runtime, thread and network dependencies.

## Core Concepts

| Concept | Types | What it is |
| --- | --- | --- |
| Frame input | `ImageView`, `Frame` | A non-owning view of pixels you already hold: width, height, stride, pixel format, rotation. `Frame::owner` keeps the memory alive while the frame is in use. |
| Error handling | `Result<T>`, `Status`, `ErrorCode` | Operations return `Result<T>` values instead of throwing; every failure has an error code. |
| Backends | `OcrBackend`, `DetectorBackend`, `LineDetector`, `BackendInfo` | Synchronous interfaces you implement and inject. `BackendInfo` states the backend's identity, accepted pixel formats and whether it is thread-safe. |
| Cancellation | `ExecutionContext` | A cancel callback plus an optional deadline, passed into every long operation. Timeouts and cancels come back as a `Status`, not an exception or a hang. |
| Session | `PerceptionSession` | One per image source (window, display, camera). Holds the previous frame fingerprint, the caches and the ID tracker. |
| Snapshot | `SemanticSnapshot`, `VisualRegion` | The published result of one round: regions with bounds, source and stable ID. Snapshots are immutable once published. |
| Coordinates | `Transform2D` | Maps points, rects and segments between frame, rotated and display coordinates. Tested for all four rotations, odd sizes and non-contiguous strides. |
| Caches | `FrameCache`, `CapabilityResultCache`, `VisualIndex` | Every cache has a byte limit and a defined eviction rule. Cache keys include the model revision, parameters, ROI and preprocessing version. |

## Change detection and caching

`mirador::image` computes a dHash fingerprint per frame and compares it with the previous frame layer by layer. The result is a classification — `none`, `partial` or `global` — with a reason and the regions that changed. Areas you register as ignore regions (a clock, a blinking cursor, a video overlay) do not count as changes.

- Classification `none`: the session skips the backends and serves the previous results.
- `FrameCache` keeps frame fingerprints and change ROIs under a byte limit, evicting least-recently-used entries.
- `CapabilityResultCache` stores backend results under a 128-bit key computed from model revision, model parameters, request ROI and preprocessing version. A model update or a different ROI can therefore never return an old result.
- `CachePolicy::kRefresh` forces a fresh backend run for a single request, for the cases where you know the cached value is not good enough.

## Pluggable backends

Mirador ships no models. OCR, object detection and line detection go through small synchronous interfaces in `mirador::core`, and you implement them against whatever runtime you use. Reference implementations for PP-OCR and YOLO families on ncnn live under `integrations/`, disabled by default.

- A backend receives a prepared view (already cropped and resized) and returns boxes in that view's coordinates; the session maps them back into the coordinates you submitted. Letterboxing, cropping and the crop-refine flow are handled by the session, not by your backend.
- `BackendInfo` declares the backend's name, version, model and revision, the pixel formats it accepts, and whether it is safe to call from multiple threads.
- `ExecutionContext` carries cancellation and deadlines into long operations; a cancelled or timed-out request returns a `Status` instead of hanging or failing silently.

## Fusion, stable IDs and Set-of-Mark

`mirador::fusion` merges OCR text, detections, line segments and regions you supply yourself (for example from an accessibility service) into one `SemanticSnapshot`.

- Input regions may be given in frame, rotated or display coordinates; they are transformed before fusion.
- Fusion is deterministic, and every accept/reject/merge decision is recorded in a `FusionTrace` you can inspect afterwards.
- Each region carries a `stable_id`. A button that moves a few pixels keeps its ID; a region that changes beyond recognition gets a new ID, and the snapshot's `generation` increments. Code holding a reference from an older snapshot can check the generation and notice it is stale.
- `mirador::render::set_of_mark` draws a numbered box around each region and returns the number → stable-ID map, so a multimodal model can say "click item 3" and your code can resolve which region that means.

## Line segments and region proposals

`mirador::geometry` is a small, dependency-free layer that is useful outside screen automation as well:

- `SegmentGrowingLineDetector` — a deterministic line detector written from scratch, no third-party code involved.
- `filter_segments` and `merge_collinear` — keep segments inside angle/length windows and merge collinear pieces into longer lines (road edges, table separators).
- `propose_regions` — groups closed and near-closed shapes into region proposals scored by closure, rectangularity and edge support, each with a tight ROI and a second ROI including context. Useful for deciding where to run icon or widget search. Stable API since v0.3.0.

## Examples

Five examples compile with the default build and walk through the public API without needing any model runtime:

| Example | What it shows |
| --- | --- |
| `mirador_example_change_detection` | submit frames → change detection → ROIs & fingerprints → frame cache |
| `mirador_example_perception_session` | session loop: change analysis → stub OCR → cached reuse → refresh policy |
| `mirador_example_road_segments` | line detection → angle/length filtering → collinear merge (road-boundary semantics) |
| `mirador_example_icon_state_index` | store icon states in the visual index → query under perturbation → three-tier lookup |
| `mirador_example_hybrid_localization` | accessibility regions + OCR/detection fusion → stable IDs → Set-of-Mark → generation check |

```bash
cmake --build build/release --target mirador_example_hybrid_localization
./build/release/examples/mirador_example_hybrid_localization
```

Benchmarks (`mirador_bench_change_detection`, `mirador_bench_cache_backend`,
`mirador_bench_geometric_proposal`, `mirador_bench_proposal_reuse`, and
`benchmarks/measure_sizes.sh`) measure p50/p95 latency, cache overhead, peak RSS
and artifact sizes on your machine. Published numbers are in
[docs/benchmarks/](docs/benchmarks/); they are not comparable across machines.

## API

The public API is the header set under [`include/mirador/`](include/mirador/); each header documents its own contracts (semantics, error codes, budgets). [docs/api/README.md](docs/api/README.md) is the module-level index. The most used headers:

| Module | Headers |
| --- | --- |
| core | `status.hpp`/`result.hpp`, `image_view.hpp`/`frame.hpp`, `transform.hpp`, `ocr_backend.hpp`/`detector_backend.hpp`/`line_detector.hpp`, `backend_info.hpp`, `execution_context.hpp` |
| image | `change_detection.hpp`, `fingerprint.hpp`, `frame_cache.hpp`, `letterbox.hpp`, `crop_refine.hpp`, `detection_postprocess.hpp`, `text_postprocess.hpp`, `text_normalize.hpp` |
| cache | `cache_policy.hpp`, `capability_cache.hpp`, `visual_index.hpp` |
| geometry | `segment_growing_line_detector.hpp`, `geometric_proposal.hpp` |
| fusion | `evidence.hpp`, `fusion.hpp`, `semantic_snapshot.hpp`, `stable_id_tracker.hpp`, `perception_session.hpp` |
| render | `set_of_mark.hpp`, `grid_partition.hpp` |

Link against `mirador::core`, `mirador::image`, `mirador::cache`, `mirador::geometry`, `mirador::fusion` or `mirador::render` from CMake; everything lives in namespace `mirador`.

## Platforms

| Platform | Verification level |
| --- | --- |
| Linux x86-64 | Full CI matrix (GCC and Clang; debug, warnings, ASAN, UBSAN, TSAN) plus an Ubuntu 20.04 (focal) container job. The optional X11 capture adapter is smoke-tested under Xvfb/Xorg. |
| Windows x86-64 | MSVC build verified in CI (`windows-debug` preset); the optional GDI capture adapter is compiled by CI. |
| Android | NDK cross-build verified in CI; the platform-independent parts build and run tests on any host. The MediaProjection + Accessibility adapter is compiled by CI. |

Toolchain floors: GCC/Clang ≥ 10 (libstdc++ ≥ 10), CMake ≥ 3.16 (≥ 3.21 for the presets), C++20. The core itself contains no platform-specific code. Which adapter has been verified at which level is tracked in [docs/compatibility/compatibility.md](docs/compatibility/compatibility.md), and benchmark numbers are only valid on the machine that produced them.

## Dependencies

The core library depends only on the C++20 standard library. Everything else is opt-in and is not distributed with the core:

| Component | When | License | Notes |
| --- | --- | --- | --- |
| C++20 standard library | always | — | the only core dependency |
| GoogleTest 1.18.0 | tests only | BSD-3-Clause | pinned submodule; never linked into the libraries |
| OpenCV 4.6.0 (tested) | `MIRADOR_BUILD_ADAPTERS_OPENCV=ON` | Apache-2.0 | you provide it via `find_package`; only the `cv::Mat` surface is used |
| ncnn (pinned 20260526) | `MIRADOR_BUILD_INTEGRATIONS=ON` | BSD-3-Clause | fetched at configure time, only when enabled |
| X11 / GDI / MediaProjection | optional capture adapters | provided by the platform | never vendored |

Model weights are not stored in this repository; examples that need a real model take the path from a command-line argument or environment. Full audit records: [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), [dependencies.lock.json](dependencies.lock.json) and [docs/supply-chain/](docs/supply-chain/).

## Roadmap

All six planned milestones are done; the latest release is
[v0.3.0](https://github.com/Linductor-alkaid/mirador/releases/tag/v0.3.0) (2026-09-20).

| Milestone | Scope | Tag |
| --- | --- | --- |
| M0 | Boundaries, skeleton, architecture tests | `v0.1.0-alpha` |
| M1 | Image operations, change detection, frame cache | `v0.1.0-beta.1` |
| M2 | Backend SPI, result cache, `PerceptionSession` | `v0.1.0-beta.2` |
| M3 | Vision common components, visual index, line segments | `v0.1.0-beta.3` |
| M4 | Fusion, stable IDs, Set-of-Mark | `v0.1.0` |
| M5 | Platform adapters, production benchmarks | `v0.2.0` |
| M6 | Geometric region proposals (started as an experiment) | `v0.3.0` |

Parked ideas, each with a concrete trigger written down in the
[implementation plan](docs/plans/mirador-implementation-plan.md): zero-copy
GPU/native buffer paths, a non-breaking asynchronous extension surface,
optical-flow/feature-based change detection, and an embedder backend with an
embedding-based visual index. Each starts only if measurements show the current
approach is not enough.

## Contributing

- `master` is protected; changes land through merge requests. Commit messages follow `<type>(<scope>): <subject>` with scopes such as `core`, `image`, `cache`, `geometry`, `fusion`, `render`, `adapters`, `examples`, `benchmarks`, `tests`, `build`, `ci`.
- Merge-request descriptions must state what changed, why, which tests were actually run, and what is affected. Do not claim tests passed that were not run.
- Development loop: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`. Substantive changes should also pass the sanitizer presets (`asan`, `ubsan`, `tsan`, `warnings`); tests are organized by ctest labels (`unit`, `property`, `architecture`).
- Project documents: the [engineering standards](docs/project/project-standards.md) (planning, decisions, evidence), [AGENTS.md](AGENTS.md) (repository rules), and the [design document](docs/design/mirador-development-design.md) (how the system is supposed to work).
- Contributions are licensed under the MIT license.

## License

MIT — see [LICENSE](LICENSE). License obligations for the optional dependencies
and model adapters are tracked in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
