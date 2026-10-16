#include "yolo_preprocess.hpp"
#include <mat.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/letterbox.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>
#include "ncnn_runtime.hpp"
namespace mirador::integrations {
namespace {
Status check_context(const ExecutionContext& context) {
    if (is_cancelled(context)) {
        return Status{ErrorCode::kCancelled, "YOLO preprocessing cancelled"};
    }
    if (deadline_reached(context)) {
        return Status{ErrorCode::kTimeout, "YOLO preprocessing expired"};
    }
    return Status::success();
}
Result<YoloPreparedInput> bilinear_input(const ImageView& image, int32_t side, uint8_t pad,
                                         const ExecutionContext& context) {
    const double scale = std::min(double(side) / image.width, double(side) / image.height);
    const auto rw = std::clamp(static_cast<int32_t>(std::floor(image.width * scale + .5)), 1, side);
    const auto rh = std::clamp(static_cast<int32_t>(std::floor(image.height * scale + .5)), 1, side);
    const int32_t px = (side - rw) / 2;
    const int32_t py = (side - rh) / 2;
    const ncnn::Mat resized =
        ncnn::Mat::from_pixels_resize(reinterpret_cast<const unsigned char*>(image.data), ncnn::Mat::PIXEL_RGB,
                                      image.width, image.height, static_cast<int>(image.row_stride_bytes), rw, rh);
    if (resized.empty()) {
        return Status{ErrorCode::kBudgetExceeded, "YOLO resize allocation failed"};
    }
    YoloPreparedInput result;
    result.resized_width = rw;
    result.resized_height = rh;
    result.tensor.width = result.tensor.height = side;
    result.tensor.channels = 3;
    result.tensor.data.assign(static_cast<size_t>(side) * side * 3, static_cast<float>(pad));
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < rh; ++y) {
            if (const auto status = check_context(context); !status.ok()) {
                return status;
            }
            const float* row = resized.channel(c).row(y);
            auto* dst = result.tensor.data.data() + static_cast<size_t>(c) * side * side +
                        static_cast<size_t>(y + py) * side + px;
            std::copy_n(row, rw, dst);
        }
    }
    auto transform = compose(make_scale(double(rw) / image.width, double(rh) / image.height,
                                        CoordinateSpaceId::kOriented, CoordinateSpaceId::kModelInput),
                             make_translation(px, py, CoordinateSpaceId::kModelInput, CoordinateSpaceId::kModelInput));
    if (!transform) {
        return transform.status();
    }
    result.transform = transform.take_value();
    return result;
}
Result<YoloPreparedInput> area_input(const ImageView& image, int32_t side, uint8_t pad, int64_t max_bytes,
                                     const ExecutionContext& context) {
    LetterboxRequest request;
    request.dst_width = request.dst_height = side;
    request.pad_value = pad;
    auto box = letterbox(image, request, max_bytes);
    if (!box) {
        return box.status();
    }
    auto packed = pack_image(box.value().buffer.view(), context);
    if (!packed) {
        return packed.status();
    }
    YoloPreparedInput result;
    result.resized_width = box.value().resized_width;
    result.resized_height = box.value().resized_height;
    result.transform = box.value().transform;
    result.tensor = packed.take_value();
    return result;
}
}  // namespace
Result<YoloPreparedInput> prepare_yolo_input(const ImageView& image, int32_t side, YoloInterpolation interpolation,
                                             uint8_t pad, int64_t max_bytes, const ExecutionContext& context) {
    if (!validate(image).ok() || image.format != PixelFormat::kRgb8 || side < 64 || side > 1024 || (side % 32) != 0 ||
        image.row_stride_bytes > std::numeric_limits<int>::max() ||
        (interpolation != YoloInterpolation::kArea && interpolation != YoloInterpolation::kBilinear)) {
        return Status{ErrorCode::kInvalidArgument, "invalid YOLO preprocessing configuration/view"};
    }
    if (const auto status = check_context(context); !status.ok()) {
        return status;
    }
    // Both live float tensors fit, plus the area's byte intermediates.
    const int64_t tensor_bytes = int64_t{side} * side * 3 * static_cast<int64_t>(sizeof(float));
    if (max_bytes < tensor_bytes * 2) {
        return Status{ErrorCode::kBudgetExceeded, "YOLO preprocessing byte budget exceeded"};
    }
    auto prepared = interpolation == YoloInterpolation::kArea ? area_input(image, side, pad, max_bytes, context)
                                                              : bilinear_input(image, side, pad, context);
    if (!prepared) {
        return prepared.status();
    }
    auto result = prepared.take_value();
    for (auto& value : result.tensor.data) {
        value /= 255.0F;
    }
    if (const auto status = check_context(context); !status.ok()) {
        return status;
    }
    return result;
}
}  // namespace mirador::integrations
