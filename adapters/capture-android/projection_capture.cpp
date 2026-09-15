#include "projection_capture.hpp"

#include "display_transform.hpp"

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <media/NdkImageReader.h>

#include <android/native_window.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mirador::adapters {
namespace {

/// BT.601 studio-swing conversion (Y in [16, 235], chroma centered at 128),
/// the standard path for MediaProjection/ImageReader output. Clamped to [0,
/// 255]; deterministic per-pixel math, no SIMD shortcuts.
std::byte clamp_to_byte(double value) noexcept {
    if (value <= 0.0) {
        return static_cast<std::byte>(0);
    }
    if (value >= 255.0) {
        return static_cast<std::byte>(255);
    }
    return static_cast<std::byte>(static_cast<int>(value + 0.5));
}

/// Converts a YUV_420_888 image (any row/pixel stride combination: NV12-style
/// interleaved chroma with pixel_stride 2, or planar I420/YV12 with
/// pixel_stride 1) into an owned packed RGB8 buffer, polling `context` on the
/// packing loop.
Result<std::vector<std::byte>> convert_yuv420_to_rgb8(const AImage* image, int32_t width, int32_t height,
                                                      const ExecutionContext& context) {
    // MediaErrorCode-free plane access: all four calls either report the
    // plane or the image was malformed at acquire time.
    // AImage_getPlaneData hands out mutable row pointers (the buffers are
    // owned by the AImage and stay valid until AImage_delete); the adapter
    // never writes through them.
    uint8_t* y_data = nullptr;
    uint8_t* u_data = nullptr;
    uint8_t* v_data = nullptr;
    int y_length = 0;
    int u_length = 0;
    int v_length = 0;
    int y_row_stride = 0;
    int u_row_stride = 0;
    int v_row_stride = 0;
    int u_pixel_stride = 0;
    int v_pixel_stride = 0;
    if (AImage_getPlaneData(image, 0, &y_data, &y_length) != AMEDIA_OK ||
        AImage_getPlaneData(image, 1, &u_data, &u_length) != AMEDIA_OK ||
        AImage_getPlaneData(image, 2, &v_data, &v_length) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 0, &y_row_stride) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 1, &u_row_stride) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 2, &v_row_stride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 1, &u_pixel_stride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 2, &v_pixel_stride) != AMEDIA_OK) {
        return Status{ErrorCode::kBackendFailure, "AImage plane query failed"};
    }

    std::vector<std::byte> rgb(static_cast<size_t>(width) * height * 3);
    for (int32_t y = 0; y < height; ++y) {
        if (y % 64 == 0 && is_cancelled(context)) {
            return Status{ErrorCode::kCancelled, "cancelled while converting captured pixels"};
        }
        const uint8_t* row_y = y_data + static_cast<int64_t>(y) * y_row_stride;
        const uint8_t* row_u = u_data + static_cast<int64_t>(y / 2) * u_row_stride;
        const uint8_t* row_v = v_data + static_cast<int64_t>(y / 2) * v_row_stride;
        std::byte* dst = rgb.data() + static_cast<size_t>(y) * width * 3;
        for (int32_t x = 0; x < width; ++x) {
            const double luma = static_cast<double>(row_y[x]) - 16.0;
            const double chroma_u = static_cast<double>(row_u[static_cast<size_t>(x) * u_pixel_stride]) - 128.0;
            const double chroma_v = static_cast<double>(row_v[static_cast<size_t>(x) * v_pixel_stride]) - 128.0;
            dst[static_cast<size_t>(x) * 3 + 0] = clamp_to_byte(1.164 * luma + 1.596 * chroma_v);
            dst[static_cast<size_t>(x) * 3 + 1] = clamp_to_byte(1.164 * luma - 0.391 * chroma_u - 0.813 * chroma_v);
            dst[static_cast<size_t>(x) * 3 + 2] = clamp_to_byte(1.164 * luma + 2.018 * chroma_u);
        }
    }
    return rgb;
}

}  // namespace

struct ProjectionCapture::Impl {
    AImageReader* reader = nullptr;
    ANativeWindow* window = nullptr;
    int32_t width = 0;
    int32_t height = 0;
};

ProjectionCapture::~ProjectionCapture() {
    if (impl_ != nullptr && impl_->reader != nullptr) {
        AImageReader_delete(impl_->reader);  // NOLINT(cppcoreguidelines-owning-memory)
    }
}

Result<ProjectionCapture> ProjectionCapture::create(const ProjectionCaptureOptions& options) {
    if (options.buffer_width <= 0 || options.buffer_height <= 0) {
        return Status{ErrorCode::kInvalidArgument, "buffer dimensions must be positive"};
    }
    if (options.max_images <= 0) {
        return Status{ErrorCode::kInvalidArgument, "max_images must be positive"};
    }
    auto impl = std::make_unique<Impl>();
    impl->width = options.buffer_width;
    impl->height = options.buffer_height;
    if (AImageReader_new(options.buffer_width, options.buffer_height, AIMAGE_FORMAT_YUV_420_888, options.max_images,
                         &impl->reader) != AMEDIA_OK ||
        impl->reader == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "AImageReader_new failed"};
    }
    if (AImageReader_getWindow(impl->reader, &impl->window) != AMEDIA_OK || impl->window == nullptr) {
        AImageReader_delete(impl->reader);
        return Status{ErrorCode::kBackendUnavailable, "AImageReader_getWindow failed"};
    }
    ProjectionCapture capture;
    capture.impl_ = std::move(impl);
    return capture;
}

ANativeWindow* ProjectionCapture::window() const noexcept {
    return impl_ != nullptr ? impl_->window : nullptr;
}

Result<Frame> ProjectionCapture::capture(const ExecutionContext& context) const {
    if (impl_ == nullptr || impl_->reader == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "capture moved-from"};
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled before capture"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached before capture"};
    }
    AImage* raw = nullptr;
    const media_status_t acquired = AImageReader_acquireLatestImage(impl_->reader, &raw);
    if (acquired != AMEDIA_OK || raw == nullptr) {
        return Status{ErrorCode::kBackendFailure, "AImageReader_acquireLatestImage failed (no buffer yet?)"};
    }
    // RAII: the AImage owns its plane buffers on every path below.
    std::unique_ptr<AImage, void (*)(AImage*)> image(raw, [](AImage* owned) noexcept {
        AImage_delete(owned);  // NOLINT(cppcoreguidelines-owning-memory)
    });

    int32_t width = 0;
    int32_t height = 0;
    if (AImage_getWidth(raw, &width) != AMEDIA_OK || AImage_getHeight(raw, &height) != AMEDIA_OK || width <= 0 ||
        height <= 0) {
        return Status{ErrorCode::kBackendFailure, "AImage geometry query failed"};
    }

    auto pixels = convert_yuv420_to_rgb8(raw, width, height, context);
    if (!pixels.ok()) {
        return pixels.status();
    }
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "cancelled after capture"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "deadline reached after capture"};
    }

    Frame frame;
    frame.image.data = pixels.value().data();
    frame.image.width = width;
    frame.image.height = height;
    frame.image.row_stride_bytes = static_cast<int64_t>(width) * 3;
    frame.image.format = PixelFormat::kRgb8;
    frame.sequence = 0;
    frame.timestamp = std::chrono::steady_clock::now();
    frame.owner = std::make_shared<const std::vector<std::byte>>(std::move(pixels).take_value());
    return frame;
}

Result<Transform2D> ProjectionCapture::display_transform(int32_t display_width, int32_t display_height) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::kBackendUnavailable, "capture moved-from"};
    }
    return projection_display_transform(impl_->width, impl_->height, display_width, display_height);
}

}  // namespace mirador::adapters
