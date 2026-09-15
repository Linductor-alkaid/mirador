#ifndef MIRADOR_ADAPTERS_CAPTURE_LINUX_X11_CAPTURE_HPP
#define MIRADOR_ADAPTERS_CAPTURE_LINUX_X11_CAPTURE_HPP

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace mirador::adapters {

/// X11 window id (XID). X11 types never cross this header (RULE-02).
using X11WindowId = std::uint32_t;

struct X11CaptureOptions {
    /// X display string; empty uses the DISPLAY environment (XOpenDisplay
    /// semantics). The connection is opened once and owned by the capture
    /// object.
    std::string display;
};

/// Synchronous X11 screen/window capture adapter (M5-05). This is an
/// independent adaptation layer (AGENTS.md): Mirador core never touches
/// X11; the adapter converts captured pixmaps into plain `Frame`s the
/// caller feeds into a `PerceptionSession`. Frames own their RGB8 buffers
/// (view + owner, design section 6); pixels are processed in memory only
/// and never written to disk (RULE-10).
///
/// Not thread-safe: one instance per capture site; callers own scheduling,
/// threads and rate limits (AGENTS.md concurrency rules). X11 is inherently
/// best-effort: a window can disappear between query and capture, which
/// surfaces as kBackendFailure.
class X11Capture {
public:
    /// Opens the display connection; kBackendUnavailable when XOpenDisplay
    /// fails (missing server, bad DISPLAY, no X11 at runtime).
    static Result<X11Capture> create(const X11CaptureOptions& options);

    X11Capture(X11Capture&& other) noexcept;
    X11Capture& operator=(X11Capture&& other) noexcept;
    ~X11Capture();

    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    /// Id of the root window (full-screen capture target).
    [[nodiscard]] X11WindowId root_window() const noexcept;

    /// Current geometry of `window`; kBackendFailure for unknown windows.
    Result<std::pair<int32_t, int32_t>> window_geometry(X11WindowId window) const;

    /// Captures `window`'s current pixels into an owned RGB8 frame. Polls
    /// `context` before and after the (atomic) X roundtrip. Callers that
    /// pass arbitrary window ids must install a non-default X error handler
    /// in their process: Xlib's default handler exits on protocol errors
    /// (e.g. BadWindow for a destroyed window), which would make this
    /// method's kBackendFailure path unreachable. Capturing the root window
    /// fails on XWayland hosts (kBackendFailure): the XWayland server keeps
    /// no pixel backing for the root window — use a Wayland screen portal
    /// adapter there.
    Result<Frame> capture_window(X11WindowId window, const ExecutionContext& context);

    /// Captures the root window (whole screen).
    Result<Frame> capture_root(const ExecutionContext& context);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit X11Capture(std::unique_ptr<Impl> impl) noexcept;
};

}  // namespace mirador::adapters

#endif  // MIRADOR_ADAPTERS_CAPTURE_LINUX_X11_CAPTURE_HPP
