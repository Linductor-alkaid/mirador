#ifndef MIRADOR_ADAPTERS_CAPTURE_WINDOWS_GDI_CAPTURE_HPP
#define MIRADOR_ADAPTERS_CAPTURE_WINDOWS_GDI_CAPTURE_HPP

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/transform.hpp>

#include <cstdint>
#include <utility>

namespace mirador::adapters {

/// Win32 window handle. HWND never crosses this header (RULE-02); GDI
/// handles are pointer-sized opaque values on every supported ABI.
using GdiWindowId = const void*;

/// Screen-space client-area geometry of one window.
struct GdiClientArea {
    int32_t x = 0;  ///< screen-space left of the client area
    int32_t y = 0;  ///< screen-space top of the client area
    int32_t width = 0;
    int32_t height = 0;
};

struct GdiCaptureOptions {
    /// Placeholder for future session/display selection; GDI captures from
    /// the interactive window station of the calling process.
    int32_t reserved = 0;
};

/// Synchronous GDI screen/window capture adapter (M5-05, Windows only).
/// Independent adaptation layer (AGENTS.md): Mirador core never touches
/// Win32; the adapter converts BitBlt results into plain `Frame`s the caller
/// feeds into a `PerceptionSession`. Frames own their RGB8 buffers (view +
/// owner, design section 6); pixels are processed in memory only and never
/// written to disk (RULE-10).
///
/// The type is intentionally stateless (GDI needs no persistent connection);
/// create() exists so callers treat every capture adapter uniformly.
///
/// Not thread-safe: one instance per capture site; callers own scheduling,
/// threads and rate limits (AGENTS.md concurrency rules). GDI BitBlt of
/// layered/protected windows can yield black regions or stale content —
/// callers needing those surfaces should use a Windows.Graphics.Capture
/// adapter; this one demonstrates the DEC-012/DEC-016 adapter contract only.
class GdiCapture {
public:
    GdiCapture() noexcept = default;
    GdiCapture(GdiCapture&& other) noexcept = default;
    GdiCapture& operator=(GdiCapture&& other) noexcept = default;
    ~GdiCapture() noexcept = default;
    GdiCapture(const GdiCapture&) = delete;
    GdiCapture& operator=(const GdiCapture&) = delete;

    /// Validates options (nothing to fail on yet). Never throws.
    [[nodiscard]] static Result<GdiCapture> create(const GdiCaptureOptions& options);

    /// Id of the desktop window (full-screen capture target).
    [[nodiscard]] GdiWindowId desktop_window() const noexcept;

    /// Screen-space origin and size of `window`'s client area, relative to
    /// the primary display's top-left; kBackendFailure for invalid or gone
    /// windows.
    Result<GdiClientArea> window_client_area(GdiWindowId window) const;

    /// Captures `window`'s current pixels into an owned RGB8 frame. Polls
    /// `context` before and after the (atomic) BitBlt. kBackendFailure when
    /// the window is gone or the bitmap setup fails; kUnsupportedFormat when
    /// the screen DC reports an unexpected layout.
    Result<Frame> capture_window(GdiWindowId window, const ExecutionContext& context);

    /// Captures the whole desktop.
    Result<Frame> capture_desktop(const ExecutionContext& context);

    /// Display-space mapping of `window`'s captured view (DEC-016): the
    /// kOriented -> kDisplay transform placing the captured frame in the
    /// primary display's client coordinate system — a translation by the
    /// window's current screen-space client origin, because GDI capture is
    /// 1:1 pixels and never rotated. The desktop window maps to the identity.
    /// kBackendFailure for unknown windows.
    Result<Transform2D> window_display_transform(GdiWindowId window) const;
};

}  // namespace mirador::adapters

#endif  // MIRADOR_ADAPTERS_CAPTURE_WINDOWS_GDI_CAPTURE_HPP
