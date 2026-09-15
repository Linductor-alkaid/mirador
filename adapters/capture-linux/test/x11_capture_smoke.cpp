// M5-05 (design sections 21, 24): live-X smoke test for the X11 capture
// adapter. Creates its OWN 64x48 input-only window over the display given by
// the environment, fills it with a deterministic pattern (red field, blue
// bottom rows), then verifies the adapter's frame contract, geometry,
// root capture, cancellation and the documented failure paths. Only the
// window created here is ever read; no pixels are printed or persisted
// (privacy RULE-10). A missing X server fails loudly (CI runs under xvfb).
//
// Xlib.h pollutes the preprocessor ('#define Status int', Bool); its headers
// come first and the macros are scrubbed before any Mirador header. The test
// installs a non-fatal X error handler so BadWindow paths surface as
// kBackendFailure results instead of terminating the process.

#include <X11/X.h>
#include <X11/Xlib.h>

#undef Status
#undef Bool

#include "x11_capture.hpp"

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace {

int g_failed_checks = 0;
int g_x11_errors = 0;

void expect_true(bool condition, const std::string& what) {
    if (condition) {
        std::printf("ok: %s\n", what.c_str());
    } else {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failed_checks;
    }
}

void expect_code(const mirador::Status& status, mirador::ErrorCode expected, const std::string& what) {
    const bool matched = !status.ok() && status.code() == expected;
    if (matched) {
        std::printf("ok: %s\n", what.c_str());
    } else {
        std::printf("FAIL: %s (expected %s, got %s)\n", what.c_str(), mirador::error_code_name(expected),
                    status.ok() ? "Ok" : mirador::error_code_name(status.code()));
        ++g_failed_checks;
    }
}

/// Non-fatal X error handler: counts protocol errors and lets Xlib continue
/// so the adapter can translate them into failure codes (default handler
/// would exit the process).
int count_x11_errors(Display* /*display*/, XErrorEvent* /*event*/) {
    ++g_x11_errors;
    return 1;  // nonzero: error processed, keep going
}

constexpr int32_t k_window_width = 64;
constexpr int32_t k_window_height = 48;
constexpr int32_t k_blue_rows = 8;
constexpr int k_channel_tolerance = 8;  // visual quantization headroom

/// Waits (bounded, ~2 s) until the window manager reports the window mapped.
bool wait_for_map_notify(Display* display, Window window) {
    XSelectInput(display, window, StructureNotifyMask);
    XMapRaised(display, window);
    XFlush(display);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == MapNotify && event.xmap.window == window) {
                return true;
            }
        }
        ::usleep(20000);  // 20 ms poll, no threads
    }
    return false;
}

/// Deterministic pattern: red field, blue bottom rows (direct 0xRRGGBB pixel
/// values; the live display and xvfb both expose 24-bit TrueColor roots).
void draw_pattern(Display* display, Window window) {
    XGCValues gc_values;
    gc_values.foreground = 0xFF0000UL;
    GC gc = XCreateGC(display, window, GCForeground, &gc_values);
    XFillRectangle(display, window, gc, 0, 0, static_cast<unsigned int>(k_window_width),
                   static_cast<unsigned int>(k_window_height));
    XSetForeground(display, gc, 0x0000FFUL);
    XFillRectangle(display, window, gc, 0, static_cast<int>(k_window_height - k_blue_rows),
                   static_cast<unsigned int>(k_window_width), static_cast<unsigned int>(k_blue_rows));
    XFlush(display);
    XFreeGC(display, gc);
}

bool channel_close(int captured, int expected) {
    return std::fabs(static_cast<double>(captured - expected)) <= k_channel_tolerance;
}

bool pixel_is(const mirador::Frame& frame, int32_t x, int32_t y, int red, int green, int blue) {
    if (frame.image.data == nullptr) {
        return false;
    }
    const auto* row = frame.image.data + static_cast<int64_t>(y) * frame.image.row_stride_bytes;
    const auto* pixel = row + static_cast<int64_t>(x) * 3;
    return channel_close(static_cast<int>(std::to_integer<uint8_t>(pixel[0])), red) &&
           channel_close(static_cast<int>(std::to_integer<uint8_t>(pixel[1])), green) &&
           channel_close(static_cast<int>(std::to_integer<uint8_t>(pixel[2])), blue);
}

bool run_all_checks() {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::printf("FAIL: XOpenDisplay(nullptr) failed; a live X display is required for this test\n");
        ++g_failed_checks;
        return false;
    }
    XSetErrorHandler(count_x11_errors);

    const Window window = XCreateSimpleWindow(
        display, XDefaultRootWindow(display), 10, 10, static_cast<unsigned int>(k_window_width),
        static_cast<unsigned int>(k_window_height), 0, 0, WhitePixel(display, DefaultScreen(display)));
    if (window == 0) {
        std::printf("FAIL: XCreateSimpleWindow failed\n");
        ++g_failed_checks;
        XCloseDisplay(display);
        return false;
    }
    if (!wait_for_map_notify(display, window)) {
        std::printf("FAIL: window was not mapped within 2 s\n");
        ++g_failed_checks;
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return false;
    }
    draw_pattern(display, window);
    std::printf("ok: own 64x48 window mapped and pattern drawn (display %s)\n", DisplayString(display));

    // Check 2: adapter creation over the ambient DISPLAY, root window id.
    auto capture_result = mirador::adapters::X11Capture::create(mirador::adapters::X11CaptureOptions{});
    expect_true(capture_result.ok(), "X11Capture::create({}) succeeds on the ambient DISPLAY");
    if (!capture_result.ok()) {
        std::printf("  create status: %s\n", capture_result.status().message().c_str());
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return false;
    }
    auto capture = std::move(capture_result).take_value();
    expect_true(capture.root_window() != 0, "root_window() is non-zero");

    // Check 3: capture of the own window — frame contract and pixels.
    const mirador::ExecutionContext never_cancelled;
    const auto frame_result =
        capture.capture_window(static_cast<mirador::adapters::X11WindowId>(window), never_cancelled);
    expect_true(frame_result.ok(), "capture_window captures the own window");
    if (frame_result.ok()) {
        const mirador::Frame& frame = frame_result.value();
        expect_true(frame.image.width == k_window_width && frame.image.height == k_window_height,
                    "captured frame is 64x48");
        expect_true(frame.image.format == mirador::PixelFormat::kRgb8, "captured frame format is kRgb8");
        expect_true(frame.image.row_stride_bytes == static_cast<int64_t>(k_window_width) * 3,
                    "captured frame row stride is 64*3");
        expect_true(frame.owner != nullptr && frame.image.data != nullptr, "captured frame owns its pixels");
        expect_true(pixel_is(frame, 5, 5, 255, 0, 0), "pixel (5,5) is red within tolerance");
        expect_true(pixel_is(frame, 5, 44, 0, 0, 255), "pixel (5,44) is blue within tolerance");
    }

    // Check 4: geometry of the own window.
    const auto geometry = capture.window_geometry(static_cast<mirador::adapters::X11WindowId>(window));
    expect_true(geometry.ok() && geometry.value().first == k_window_width && geometry.value().second == k_window_height,
                "window_geometry reports 64x48 for the own window");

    // Check 5: root capture. A real Xorg/Xvfb server backs the root window
    // with pixels, so the frame invariants hold. Under XWayland (a Wayland
    // session exports WAYLAND_DISPLAY next to :0) the root has no server-side
    // backing store at all and GetImage fails with BadMatch; there the
    // documented kBackendFailure outcome is asserted instead — no branch is
    // ever silently skipped.
    const bool under_xwayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
    const auto root_frame = capture.capture_root(never_cancelled);
    if (under_xwayland) {
        expect_code(root_frame.status(), mirador::ErrorCode::kBackendFailure,
                    "root capture under XWayland -> kBackendFailure (root has no backing)");
    } else {
        expect_true(root_frame.ok(), "capture_root succeeds");
        if (!root_frame.ok()) {
            std::printf("  capture_root status: %s\n", root_frame.status().message().c_str());
        }
        if (root_frame.ok()) {
            const auto root_geometry = capture.window_geometry(capture.root_window());
            expect_true(root_geometry.ok() && root_frame.value().image.width == root_geometry.value().first &&
                            root_frame.value().image.height == root_geometry.value().second &&
                            root_frame.value().image.width > 0 && root_frame.value().image.height > 0,
                        "root frame size matches window_geometry(root) and is non-empty");
            expect_true(root_frame.value().image.format == mirador::PixelFormat::kRgb8, "root frame format is kRgb8");
        }
    }

    // Check 6: cancellation before the X roundtrip.
    mirador::ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const auto cancelled_capture =
        capture.capture_window(static_cast<mirador::adapters::X11WindowId>(window), cancelled);
    expect_code(cancelled_capture.status(), mirador::ErrorCode::kCancelled,
                "capture_window with a cancelled context -> kCancelled");

    // Check 7: documented failure paths.
    const auto bad_display =
        mirador::adapters::X11Capture::create(mirador::adapters::X11CaptureOptions{"no-such-display:99"});
    expect_code(bad_display.status(), mirador::ErrorCode::kBackendUnavailable,
                "create with a non-existent display -> kBackendUnavailable");

    const auto unknown_window =
        capture.capture_window(static_cast<mirador::adapters::X11WindowId>(0x7FFFFFFFU), never_cancelled);
    expect_code(unknown_window.status(), mirador::ErrorCode::kBackendFailure,
                "capture_window on a non-existent window id -> kBackendFailure");

    // Check 8: own-resource cleanup (the adapter cleans its display in its
    // destructor at scope exit).
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    expect_true(g_x11_errors > 0, "the negative window path produced a counted X protocol error");
    return g_failed_checks == 0;
}

}  // namespace

int main() {
    const bool all_passed = run_all_checks();
    if (!all_passed) {
        std::printf("x11 capture smoke: FAIL (%d failed check(s))\n", g_failed_checks);
        return 1;
    }
    std::printf("x11 capture smoke: PASS (own-window capture, geometry, root, cancellation, failure paths)\n");
    return 0;
}
