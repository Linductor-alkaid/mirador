#include "yolo_preprocess.hpp"
#include <mat.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mirador/letterbox.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>
#include <utility>
namespace mirador::integrations {
Result<YoloPreparedInput> prepare_yolo_input(const ImageView& image, int32_t side, YoloInterpolation interpolation,
                                             uint8_t pad, int64_t max_bytes, const ExecutionContext& context) {
    if (!validate(image).ok() || image.format != PixelFormat::kRgb8 || side < 64 || side > 1024 || (side % 32) != 0 ||
        image.row_stride_bytes > std::numeric_limits<int>::max() ||
        (interpolation != YoloInterpolation::kArea && interpolation != YoloInterpolation::kBilinear))
        return Status{ErrorCode::kInvalidArgument, "invalid YOLO preprocessing configuration/view"};
    if (is_cancelled(context)) return Status{ErrorCode::kCancelled, "YOLO preprocessing cancelled"};
    if (deadline_reached(context)) return Status{ErrorCode::kTimeout, "YOLO preprocessing expired"};
    // Both live float tensors fit, plus the area's byte intermediates.
    const int64_t tensor_bytes = int64_t{side} * side * 3 * sizeof(float);
    if (max_bytes < tensor_bytes * 2)
        return Status{ErrorCode::kBudgetExceeded, "YOLO preprocessing byte budget exceeded"};
    const double scale = std::min(double(side) / image.width, double(side) / image.height);
    const auto rw = std::clamp(static_cast<int32_t>(std::floor(image.width * scale + .5)), 1, side);
    const auto rh = std::clamp(static_cast<int32_t>(std::floor(image.height * scale + .5)), 1, side);
    const int32_t px = (side - rw) / 2, py = (side - rh) / 2;
    YoloPreparedInput result;
    result.resized_width = rw;
    result.resized_height = rh;
    if (interpolation == YoloInterpolation::kArea) {
        LetterboxRequest request;
        request.dst_width = request.dst_height = side;
        request.pad_value = pad;
        auto box = letterbox(image, request, max_bytes);
        if (!box) return box.status();
        auto packed = pack_image(box.value().buffer.view(), context);
        if (!packed) return packed.status();
        result.tensor = packed.take_value();
        result.transform = box.value().transform;
    } else {
        const ncnn::Mat resized =
            ncnn::Mat::from_pixels_resize(reinterpret_cast<const unsigned char*>(image.data), ncnn::Mat::PIXEL_RGB,
                                          image.width, image.height, static_cast<int>(image.row_stride_bytes), rw, rh);
        if (resized.empty()) return Status{ErrorCode::kBudgetExceeded, "YOLO resize allocation failed"};
        result.tensor.width = result.tensor.height = side;
        result.tensor.channels = 3;
        result.tensor.data.assign(static_cast<size_t>(side) * side * 3, static_cast<float>(pad));
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < rh; ++y) {
                if (is_cancelled(context)) return Status{ErrorCode::kCancelled, "YOLO resize cancelled"};
                const float* row = resized.channel(c).row(y);
                auto* dst = result.tensor.data.data() + static_cast<size_t>(c) * side * side +
                            static_cast<size_t>(y + py) * side + px;
                std::copy_n(row, rw, dst);
            }
        }
        auto transform =
            compose(make_scale(double(rw) / image.width, double(rh) / image.height, CoordinateSpaceId::kOriented,
                               CoordinateSpaceId::kModelInput),
                    make_translation(px, py, CoordinateSpaceId::kModelInput, CoordinateSpaceId::kModelInput));
        if (!transform) return transform.status();
        result.transform = transform.take_value();
    }
    for (auto& value : result.tensor.data) value /= 255.0F;
    if (is_cancelled(context)) return Status{ErrorCode::kCancelled, "YOLO preprocessing cancelled"};
    if (deadline_reached(context)) return Status{ErrorCode::kTimeout, "YOLO preprocessing expired"};
    return result;
}
}  // namespace mirador::integrations
