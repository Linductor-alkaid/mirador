# Quick Start

## Minimal Runnable Integration

Mirador exports no installed CMake package; consume the repository with `FetchContent` or `add_subdirectory` of a vendored checkout. `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_app LANGUAGES CXX)

include(FetchContent)
set(MIRADOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MIRADOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(MIRADOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mirador
    GIT_REPOSITORY https://github.com/Linductor-alkaid/mirador.git
    GIT_TAG        v0.3.0)  # pin a release tag; refresh deliberately
FetchContent_MakeAvailable(mirador)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE mirador::fusion)
```

Link only the highest-level module you use; targets carry their dependencies. `mirador::fusion` provides the session pipeline, `mirador::render` adds Set-of-Mark, `mirador::geometry` line segments and proposals, `mirador::cache` + `mirador::image` standalone index and fingerprint use. The default build fetches nothing beyond the source; integrations under `integrations/` stay off unless you opt in.

`main.cpp` — one frame, one stub OCR, one proven cache hit (the runnable, commented version is `examples/perception_session_tour.cpp`):

```cpp
#include <mirador/backend_info.hpp>
#include <mirador/frame.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

using namespace mirador;

class StubOcr final : public OcrBackend {
public:
    BackendInfo info() const override {
        return {"stub-ocr", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
    }

    Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest&,
                                              const ExecutionContext&) override {
        ++calls_;  // a cache hit never reaches this line
        TextRegion line;
        line.bounds = RectF{0.0F, 0.0F, static_cast<float>(prepared.width),
                            static_cast<float>(prepared.height)};
        line.utf8_text = "demo";
        line.confidence = 0.9F;
        return std::vector<TextRegion>{std::move(line)};
    }

    int calls_ = 0;
};

int main() {
    PerceptionSessionOptions options;
    options.source_id = "screen-1";
    PerceptionSession session = PerceptionSession::create(options).take_value();
    StubOcr ocr;

    std::vector<std::byte> pixels(320 * 200 * 4, std::byte{0});
    Frame frame;
    frame.image.data = pixels.data();
    frame.image.width = 320;
    frame.image.height = 200;
    frame.image.row_stride_bytes = 320 * 4;
    frame.image.format = PixelFormat::kRgba8;
    frame.image.rotation = Rotation::k0;
    frame.sequence = 1;
    frame.source_id = "screen-1";
    frame.owner = std::make_shared<const std::vector<std::byte>>(pixels);

    static_cast<void>(session.analyze_change(frame));  // first submission: kGlobal, kFirstFrame

    OcrRequest request;
    request.roi = RectF{0.0F, 100.0F, 320.0F, 100.0F};
    const auto first = session.run_ocr(frame, &ocr, request).value();
    const auto second = session.run_ocr(frame, &ocr, request).value();

    return (first.size() == 1 && second.size() == 1 && ocr.calls_ == 1) ? 0 : 1;
}
```

Configure, build, and run; the program must exit 0 with `calls_ == 1` before adding another capability.

## First Boundary

The public API never throws: every call returns `Result<T>` and reports failure through `Status`/`ErrorCode` (`kInvalidArgument`, `kBackendUnavailable`, `kUnsupportedFormat`, `kBackendFailure`, `kBudgetExceeded`, `kCancelled`, `kTimeout`, ...). Check `ok()` before `value()`. The core creates no threads and holds no timers — your code drives the loop and the scheduling.

## Next

For the backend contract read [backends](backends.md); for the change/cache loop read [session and caching](session-and-caching.md). Otherwise return to the entry router and load one matching card.
