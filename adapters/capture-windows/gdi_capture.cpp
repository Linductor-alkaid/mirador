#include "gdi_capture.hpp"

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

// Win32 must come first; windows.h defines min/max macros that the std
// algorithms below would misinterpret.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mirador::adapters {
namespace {

/// BGRA rows from GetDIBits (top-down 32bpp) converted into an owned packed
/// RGB8 buffer, polling `context` on the packing loop. `stride` is the
/// DWORD-aligned row width GetDIBits reports.
Result<std::vector<std::byte>> convert_bgra_to_rgb8(const uint8_t* src, int32_t width, int32_t height, int64_t stride,
                                                    const ExecutionContext& context) {
    std::vector<std::byte> rgb(static_cast<size_t>(width) * height * 3);
    for (int32_t y = 0; y < height; ++y) {
        if (y % 64 == 0 && is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled while converting captured pixels"};
        }
        const uint8_t* row = src + static_cast<int64_t>(y) * stride;
        std::byte* dst = rgb.data() + static_cast<size_t>(y) * width * 3;
        for (int32_t x = 0; x < width; ++x) {
            const uint8_t* px = row + static_cast<int64_t>(x) * 4;
            dst[static_cast<size_t>(x) * 3 + 0] = static_cast<std::byte>(px[2]);
            dst[static_cast<size_t>(x) * 3 + 1] = static_cast<std::byte>(px[1]);
            dst[static_cast<size_t>(x) * 3 + 2] = static_cast<std::byte>(px[0]);
        }
    }
    return rgb;
}

/// Screen-space client rectangle of `hwnd`; false when the window is gone.
bool client_area_of(HWND hwnd, GdiClientArea& area) noexcept {
    RECT client = {0, 0, 0, 0};
    if (GetClientRect(hwnd, &client) == 0) {
        return false;
    }
    POINT origin = {client.left, client.top};
    if (ClientToScreen(hwnd, &origin) == 0) {
        return false;
    }
    area.x = origin.x;
    area.y = origin.y;
    area.width = client.right - client.left;
    area.height = client.bottom - client.top;
    return true;
}

}  // namespace

Result<GdiCapture> GdiCapture::create(const GdiCaptureOptions& options) {
    if (options.reserved != 0) {
        return Status{ErrorCode::kInvalidArgument, "GdiCaptureOptions.reserved must be zero"};
    }
    return GdiCapture{};
}

GdiWindowId GdiCapture::desktop_window() const noexcept {
    // GetDesktopWindow returns the root window covering all monitors.
    return reinterpret_cast<GdiWindowId>(GetDesktopWindow());
}

Result<GdiClientArea> GdiCapture::window_client_area(GdiWindowId window) const {
    GdiClientArea area;
    if (!client_area_of(reinterpret_cast<HWND>(const_cast<void*>(window)), area)) {
        return Status{ErrorCode::kBackendFailure, "GetClientRect/ClientToScreen failed (window gone?)"};
    }
    return area;
}

Result<Frame> GdiCapture::capture_window(GdiWindowId window, const ExecutionContext& context) {
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled before capture"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached before capture"};
    }
    const HWND hwnd = reinterpret_cast<HWND>(const_cast<void*>(window));
    GdiClientArea area;
    if (!client_area_of(hwnd, area)) {
        return Status{ErrorCode::kBackendFailure, "window client area unavailable"};
    }
    if (area.width <= 0 || area.height <= 0) {
        return Status{ErrorCode::kBackendFailure, "window has no visible pixels"};
    }

    const HDC screen_dc = GetDC(hwnd);
    if (screen_dc == nullptr) {
        return Status{ErrorCode::kBackendFailure, "GetDC failed"};
    }
    // RAII: every path releases the screen DC (paired with its hwnd) and
    // deletes the memory DC and bitmap; the pixel bytes move into the Frame
    // owner.
    struct DcGuard {
        HWND hwnd;
        HDC dc;
        ~DcGuard() {
            ReleaseDC(hwnd, dc);  // NOLINT(cppcoreguidelines-owning-memory)
        }
    } dc_guard{hwnd, screen_dc};

    const HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (memory_dc == nullptr) {
        return Status{ErrorCode::kBackendFailure, "CreateCompatibleDC failed"};
    }
    // DeleteDC returns BOOL; the custom deleter ignores it, nothing can be
    // reported on a cleanup path.
    std::unique_ptr<HDC__, int (*)(HDC)> memory_guard(memory_dc, &DeleteDC);

    // Negative height: top-down DIB, rows in natural scanline order.
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = area.width;
    info.bmiHeader.biHeight = -area.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(memory_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        return Status{ErrorCode::kBackendFailure, "CreateDIBSection failed"};
    }
    // DeleteObject returns BOOL; ignored for the same reason as DeleteDC.
    std::unique_ptr<HBITMAP__, BOOL (*)(HGDIOBJ)> bitmap_guard(bitmap, &DeleteObject);

    const HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    if (previous == HGDI_ERROR || previous == nullptr) {
        return Status{ErrorCode::kBackendFailure, "SelectObject failed"};
    }
    // Single atomic BitBlt: cancellation is polled around it, not during it.
    if (BitBlt(memory_dc, 0, 0, area.width, area.height, screen_dc, 0, 0, SRCCOPY | CAPTUREBLT) == 0) {
        return Status{ErrorCode::kBackendFailure, "BitBlt failed (prohibited or gone surface)"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled after capture"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached after capture"};
    }

    // GetDIBits with an explicit top-down header re-reports the aligned row
    // stride; the DIB section layout is ours to interpret either way.
    BITMAPINFO read_info = {};
    read_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    read_info.bmiHeader.biWidth = area.width;
    read_info.bmiHeader.biHeight = -area.height;
    read_info.bmiHeader.biPlanes = 1;
    read_info.bmiHeader.biBitCount = 32;
    read_info.bmiHeader.biCompression = BI_RGB;
    const int rows = GetDIBits(memory_dc, bitmap, 0, static_cast<UINT>(area.height), bits, &read_info, DIB_RGB_COLORS);
    if (rows != area.height) {
        return Status{ErrorCode::kUnsupportedFormat, "GetDIBits returned an unexpected row count"};
    }
    const int64_t stride = (read_info.bmiHeader.biSizeImage != 0 && area.height > 0)
                               ? static_cast<int64_t>(read_info.bmiHeader.biSizeImage) / area.height
                               : (static_cast<int64_t>(area.width) * 4 + 3) / 4 * 4;

    auto pixels = convert_bgra_to_rgb8(static_cast<const uint8_t*>(bits), area.width, area.height, stride, context);
    if (!pixels.ok()) {
        return pixels.status();
    }

    Frame frame;
    frame.image.data = pixels.value().data();
    frame.image.width = area.width;
    frame.image.height = area.height;
    frame.image.row_stride_bytes = static_cast<int64_t>(area.width) * 3;
    frame.image.format = PixelFormat::kRgb8;
    frame.sequence = 0;
    frame.timestamp = std::chrono::steady_clock::now();
    frame.owner = std::make_shared<const std::vector<std::byte>>(std::move(pixels).take_value());
    return frame;
}

Result<Frame> GdiCapture::capture_desktop(const ExecutionContext& context) {
    return capture_window(desktop_window(), context);
}

Result<Transform2D> GdiCapture::window_display_transform(GdiWindowId window) const {
    GdiClientArea area;
    if (!client_area_of(reinterpret_cast<HWND>(const_cast<void*>(window)), area)) {
        return Status{ErrorCode::kBackendFailure, "GetClientRect/ClientToScreen failed (window gone?)"};
    }
    return make_translation(static_cast<double>(area.x), static_cast<double>(area.y), CoordinateSpaceId::kOriented,
                            CoordinateSpaceId::kDisplay);
}

}  // namespace mirador::adapters
