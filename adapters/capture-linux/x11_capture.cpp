#include "x11_capture.hpp"

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <X11/X.h>
#include <X11/Xlib.h>

// Xlib.h pollutes the preprocessor with '#define Status int' (and Bool/True/
// False); it must not leak into the Mirador error model below.
#undef Status
#undef Bool

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mirador::adapters {
namespace {

/// Converts an XImage (ZPixmap, 24-bit depth = 4 bytes per pixel, BGRX byte
/// order on little-endian hosts) into an owned packed RGB8 buffer, polling
/// `context` on the packing loop. Rows may be padded (XImage bytes_per_line),
/// honored like any non-contiguous stride.
Result<std::vector<std::byte>> convert_bgrx_to_rgb8(const XImage& image, const ExecutionContext& context) {
    std::vector<std::byte> rgb(static_cast<size_t>(image.width) * image.height * 3);
    const int64_t src_stride = image.bytes_per_line;
    // XImage::data is char*; the buffer layout is ours to interpret.
    const auto* src = reinterpret_cast<const uint8_t*>(image.data);
    for (int32_t y = 0; y < image.height; ++y) {
        if (y % 64 == 0 && is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled while converting captured pixels"};
        }
        const uint8_t* row = src + static_cast<int64_t>(y) * src_stride;
        std::byte* dst = rgb.data() + static_cast<size_t>(y) * image.width * 3;
        for (int32_t x = 0; x < image.width; ++x) {
            const uint8_t* px = row + static_cast<int64_t>(x) * 4;
            // ZPixmap 24-bit: byte order B, G, R, X on little-endian.
            dst[static_cast<size_t>(x) * 3 + 0] = static_cast<std::byte>(px[2]);
            dst[static_cast<size_t>(x) * 3 + 1] = static_cast<std::byte>(px[1]);
            dst[static_cast<size_t>(x) * 3 + 2] = static_cast<std::byte>(px[0]);
        }
    }
    return rgb;
}

Result<Frame> capture_from_display(Display* display, Window window, const ExecutionContext& context) {
    XWindowAttributes attributes;
    if (XGetWindowAttributes(display, window, &attributes) == 0) {
        return Status{ErrorCode::kBackendFailure, "XGetWindowAttributes failed (window gone?)"};
    }
    if (attributes.width <= 0 || attributes.height <= 0) {
        return Status{ErrorCode::kBackendFailure, "window has no visible pixels"};
    }
    // Single atomic X roundtrip: cancellation is polled around it, not
    // during it (the X server serves the whole GetImage at once).
    XImage* raw = XGetImage(display, window, 0, 0, static_cast<unsigned int>(attributes.width),
                            static_cast<unsigned int>(attributes.height), AllPlanes, ZPixmap);
    if (raw == nullptr) {
        return Status{ErrorCode::kBackendFailure, "XGetImage failed (unmapped or prohibited)"};
    }
    // RAII: XImage owns its buffer; the struct's destroy_image function
    // pointer frees it on every path (XDestroyImage is a macro in Xutil.h,
    // so its address cannot be taken).
    std::unique_ptr<XImage, void (*)(XImage*)> image(raw,
                                                     [](XImage* owned) noexcept { owned->f.destroy_image(owned); });
    if (image->depth != 24 || image->bits_per_pixel != 32) {
        return Status{ErrorCode::kUnsupportedFormat, "only 24-bit depth / 32 bpp ZPixmap visuals are supported"};
    }

    auto pixels = convert_bgrx_to_rgb8(*image, context);
    if (!pixels.ok()) {
        return pixels.status();
    }

    Frame frame;
    frame.image.data = pixels.value().data();
    frame.image.width = image->width;
    frame.image.height = image->height;
    frame.image.row_stride_bytes = static_cast<int64_t>(image->width) * 3;
    frame.image.format = PixelFormat::kRgb8;
    frame.sequence = 0;
    frame.timestamp = std::chrono::steady_clock::now();
    frame.owner = std::make_shared<const std::vector<std::byte>>(std::move(pixels).take_value());
    return frame;
}

}  // namespace

struct X11Capture::Impl {
    Display* display = nullptr;
    Window root = 0;
    std::string display_name;
};

X11Capture::X11Capture(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

X11Capture::X11Capture(X11Capture&& other) noexcept = default;
X11Capture& X11Capture::operator=(X11Capture&& other) noexcept = default;

X11Capture::~X11Capture() {
    if (impl_ != nullptr && impl_->display != nullptr) {
        XCloseDisplay(impl_->display);
    }
}

Result<X11Capture> X11Capture::create(const X11CaptureOptions& options) {
    auto impl = std::make_unique<Impl>();
    impl->display_name = options.display;
    impl->display = XOpenDisplay(options.display.empty() ? nullptr : options.display.c_str());
    if (impl->display == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "XOpenDisplay failed"};
    }
    impl->root = XDefaultRootWindow(impl->display);
    return X11Capture{std::move(impl)};
}

X11WindowId X11Capture::root_window() const noexcept {
    return impl_ != nullptr ? static_cast<X11WindowId>(impl_->root) : 0;
}

Result<std::pair<int32_t, int32_t>> X11Capture::window_geometry(X11WindowId window) const {
    if (impl_ == nullptr || impl_->display == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "capture moved-from"};
    }
    XWindowAttributes attributes;
    if (XGetWindowAttributes(impl_->display, static_cast<Window>(window), &attributes) == 0) {
        return Status{ErrorCode::kBackendFailure, "XGetWindowAttributes failed"};
    }
    return std::make_pair(static_cast<int32_t>(attributes.width), static_cast<int32_t>(attributes.height));
}

Result<Transform2D> X11Capture::window_display_transform(X11WindowId window) const {
    if (impl_ == nullptr || impl_->display == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "capture moved-from"};
    }
    int root_x = 0;
    int root_y = 0;
    Window child = 0;
    if (XTranslateCoordinates(impl_->display, static_cast<Window>(window), impl_->root, 0, 0, &root_x, &root_y,
                              &child) == 0) {
        return Status{ErrorCode::kBackendFailure, "XTranslateCoordinates failed (window gone?)"};
    }
    return make_translation(static_cast<double>(root_x), static_cast<double>(root_y), CoordinateSpaceId::kOriented,
                            CoordinateSpaceId::kDisplay);
}

Result<Frame> X11Capture::capture_window(X11WindowId window, const ExecutionContext& context) {
    if (impl_ == nullptr || impl_->display == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "capture moved-from"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled before capture"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached before capture"};
    }
    auto frame = capture_from_display(impl_->display, static_cast<Window>(window), context);
    if (!frame.ok()) {
        return frame;
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled after capture"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached after capture"};
    }
    return frame;
}

Result<Frame> X11Capture::capture_root(const ExecutionContext& context) {
    return capture_window(root_window(), context);
}

}  // namespace mirador::adapters
