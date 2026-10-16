#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <mirador/letterbox.hpp>
#include <mirador/status.hpp>
#include <vector>
#include "yolo_preprocess.hpp"
int main() {
    using namespace mirador;
    using namespace mirador::integrations;
    int failed = 0;
    auto check = [&](bool ok) {
        if (!ok) ++failed;
    };
    constexpr int w = 7, h = 3, stride = w * 3 + 5, side = 64;
    std::vector<std::byte> bytes(stride * h, std::byte{255});
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) bytes[y * stride + x * 3 + c] = std::byte(10 + x * 20 + y * 15 + c * 3);
    ImageView image;
    image.data = bytes.data();
    image.width = w;
    image.height = h;
    image.row_stride_bytes = stride;
    image.format = PixelFormat::kRgb8;
    auto old = prepare_yolo_input(image, side, YoloInterpolation::kArea, 0, 1 << 20, {});
    LetterboxRequest request;
    request.dst_width = request.dst_height = side;
    auto box = letterbox(image, request, 1 << 20);
    check(old.ok() && box.ok());
    if (!old || !box) return 1;
    auto packed = pack_image(box.value().buffer.view(), {});
    if (!packed) return 1;
    auto packed_tensor = packed.take_value();
    for (auto& v : packed_tensor.data) v /= 255.F;
    check(old.value().tensor.data == packed_tensor.data);
    auto prepared = prepare_yolo_input(image, side, YoloInterpolation::kBilinear, 114, 1 << 20, {});
    if (!prepared) return 1;
    const auto& p = prepared.value();
    check(p.resized_width == 64 && p.resized_height == 27);
    const int py = (side - p.resized_height) / 2;
    double max_error = 0;
    for (int c = 0; c < 3; ++c)
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) {
                double expected = 114;
                if (y >= py && y < py + p.resized_height) {
                    double sx = std::clamp((x + .5) * w / p.resized_width - .5, 0., double(w - 1));
                    double sy = std::clamp((y - py + .5) * h / p.resized_height - .5, 0., double(h - 1));
                    // The source is an affine plane, so independent half-pixel bilinear
                    // interpolation has this closed form (no shared implementation).
                    expected = 10 + sx * 20 + sy * 15 + c * 3;
                }
                max_error =
                    std::max(max_error, std::abs(p.tensor.data[c * side * side + y * side + x] * 255. - expected));
            }
    check(max_error <= 1.1);  // ncnn's byte interpolation rounds with fixed-point weights.
    const auto end = transform_point(p.transform, PointF{float(w), float(h)});
    check(std::abs(end.x - 64) < 1e-5 && std::abs(end.y - (py + 27)) < 1e-5);
    check(!prepare_yolo_input(image, side, YoloInterpolation::kBilinear, 114, 1, {}).ok());
    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    check(prepare_yolo_input(image, side, YoloInterpolation::kBilinear, 114, 1 << 20, cancelled).status().code() ==
          ErrorCode::kCancelled);
    check(!prepare_yolo_input(image, side, static_cast<YoloInterpolation>(99), 0, 1 << 20, {}).ok());
    std::cout << "preprocess errors=" << failed << " max_byte_error=" << max_error << '\n';
    return failed ? 1 : 0;
}
