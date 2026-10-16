#ifndef MIRADOR_INTEGRATIONS_YOLO_PREPROCESS_HPP
#define MIRADOR_INTEGRATIONS_YOLO_PREPROCESS_HPP
#include <cstdint>
#include <mirador/transform.hpp>
#include "ncnn_runtime.hpp"
namespace mirador::integrations {
/// The legacy profile stays area/pad0. Bilinear uses ncnn's byte pixel kernel.
enum class YoloInterpolation { kArea, kBilinear };
struct YoloPreparedInput {
    NcnnTensor tensor;
    Transform2D transform;
    int32_t resized_width = 0;
    int32_t resized_height = 0;
};
/// Explicit model preprocessing; RGB only, x/255, square centered letterbox.
/// The returned transform follows executed integer sizes, including odd padding.
/// Budget covers both the resized pixels/tensor and final float tensor.
Result<YoloPreparedInput> prepare_yolo_input(const ImageView& image, int32_t side, YoloInterpolation interpolation,
                                             uint8_t pad, int64_t max_bytes, const ExecutionContext& context);
}  // namespace mirador::integrations
#endif
