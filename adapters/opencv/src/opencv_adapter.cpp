// NOLINTBEGIN(misc-include-cleaner): OpenCV headers arrive as SYSTEM includes
// and the cleaner cannot agree on their physical providers (a direct include
// reads as unused, an indirect one as missing), so the check is silenced for
// this file as a whole.
#include <mirador/opencv_adapter.hpp>

#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <opencv2/core.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace mirador {
namespace {

/// True when the Mat is a CV_8U Mat with `channels` channels.
bool is_8uc(const cv::Mat& mat, const int channels) noexcept {
    return mat.type() == CV_8UC(channels);
}

PixelFormat format_for_mat_type(const cv::Mat& mat) noexcept {
    if (is_8uc(mat, 1)) {
        return PixelFormat::kGray8;
    }
    if (is_8uc(mat, 3)) {
        return PixelFormat::kBgr8;  // OpenCV channel order: blue, green, red.
    }
    if (is_8uc(mat, 4)) {
        return PixelFormat::kBgra8;  // OpenCV channel order: blue, green, red, alpha.
    }
    return PixelFormat::kGray8;  // Undefined mapping; rejected by the caller.
}

/// Fills the single-plane view fields shared by wrap and export paths.
ImageView single_plane_view(const cv::Mat& mat, const PixelFormat format) noexcept {
    ImageView view;
    view.data = reinterpret_cast<const std::byte*>(mat.data);
    view.width = mat.cols;
    view.height = mat.rows;
    view.row_stride_bytes = static_cast<int64_t>(mat.step);
    view.format = format;
    view.rotation = Rotation::k0;
    return view;
}

Status wrap_error(const char* reason) {
    return {ErrorCode::kInvalidArgument, reason};
}

Status unsupported_type_error() {
    return {ErrorCode::kUnsupportedFormat,
            "opencv adapter: only CV_8U Mats with 1, 3 or 4 channels map to a Mirador format"};
}

}  // namespace

Result<ImageView> wrap_mat(const cv::Mat& mat) noexcept {
    if (mat.data == nullptr) {
        return wrap_error("opencv adapter: cannot wrap an empty Mat");
    }
    if (!is_8uc(mat, 1) && !is_8uc(mat, 3) && !is_8uc(mat, 4)) {
        return unsupported_type_error();
    }
    const ImageView view = single_plane_view(mat, format_for_mat_type(mat));
    if (const Result<void> valid = validate(view); !valid.ok()) {
        return valid.status();
    }
    return view;
}

Result<ImageView> wrap_nv12_mat(const cv::Mat& mat, const int32_t width, const int32_t height) noexcept {
    if (mat.data == nullptr) {
        return wrap_error("opencv adapter: cannot wrap an empty Mat");
    }
    if (!is_8uc(mat, 1)) {
        return Status{ErrorCode::kUnsupportedFormat, "opencv adapter: NV12 Mats must be CV_8UC1"};
    }
    if (width < 1 || height < 1 || width > kMaxImageDimension || height > kMaxImageDimension) {
        return wrap_error("opencv adapter: NV12 dimensions must be within [1, kMaxImageDimension]");
    }
    const auto step = static_cast<int64_t>(mat.step);
    const int64_t chroma_stride = static_cast<int64_t>(width) + (width % 2);
    if (mat.rows < height + (height + 1) / 2) {
        return wrap_error("opencv adapter: NV12 Mat needs height + ceil(height / 2) rows");
    }
    if (step < chroma_stride) {
        return wrap_error("opencv adapter: NV12 Mat step must hold the chroma rows (width + width % 2)");
    }
    ImageView view;
    view.data = reinterpret_cast<const std::byte*>(mat.data);
    view.width = width;
    view.height = height;
    view.row_stride_bytes = step;
    view.format = PixelFormat::kNv12;
    view.rotation = Rotation::k0;
    view.secondary_plane = ImagePlane{reinterpret_cast<const std::byte*>(mat.data) + step * height, step};
    if (const Result<void> valid = validate(view); !valid.ok()) {
        return valid.status();
    }
    return view;
}

Result<ImageBuffer> export_mat(const cv::Mat& mat, const int64_t max_bytes) noexcept {
    const Result<ImageView> view = wrap_mat(mat);
    if (!view.ok()) {
        return view.status();
    }
    const ImageView source = view.value();
    Result<ImageBuffer> buffer = ImageBuffer::create(source.format, source.width, source.height, max_bytes);
    if (!buffer.ok()) {
        return buffer.status();
    }
    ImageBuffer out = buffer.take_value();
    const int64_t row_bytes = static_cast<int64_t>(source.width) * bytes_per_pixel(source.format);
    for (int32_t y = 0; y < source.height; ++y) {
        std::memcpy(out.data() + static_cast<int64_t>(y) * row_bytes,
                    source.data + static_cast<int64_t>(y) * source.row_stride_bytes, static_cast<size_t>(row_bytes));
    }
    return {std::move(out)};
}

Result<ImageBuffer> export_nv12_mat(const cv::Mat& mat, const int32_t width, const int32_t height,
                                    const int64_t max_bytes) noexcept {
    const Result<ImageView> view = wrap_nv12_mat(mat, width, height);
    if (!view.ok()) {
        return view.status();
    }
    const ImageView source = view.value();
    Result<ImageBuffer> buffer = ImageBuffer::create(PixelFormat::kNv12, width, height, max_bytes);
    if (!buffer.ok()) {
        return buffer.status();
    }
    ImageBuffer out = buffer.take_value();
    const auto luma_rows = static_cast<int64_t>(width);
    const int64_t chroma_rows = static_cast<int64_t>(width) + (width % 2);
    for (int32_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<int64_t>(y) * luma_rows,
                    source.data + static_cast<int64_t>(y) * source.row_stride_bytes, static_cast<size_t>(luma_rows));
    }
    const std::byte* source_chroma = source.secondary_plane.data;
    auto* out_chroma = out.data() + static_cast<int64_t>(width) * height;
    const int32_t chroma_row_count = (height + 1) / 2;
    for (int32_t y = 0; y < chroma_row_count; ++y) {
        std::memcpy(out_chroma + static_cast<int64_t>(y) * chroma_rows,
                    source_chroma + static_cast<int64_t>(y) * source.secondary_plane.row_stride_bytes,
                    static_cast<size_t>(chroma_rows));
    }
    return {std::move(out)};
}

// NOLINTEND(misc-include-cleaner)

}  // namespace mirador
