#ifndef MIRADOR_TESTS_FUSION_FAKE_BACKENDS_HPP
#define MIRADOR_TESTS_FUSION_FAKE_BACKENDS_HPP

// Test doubles for the Backend SPI (design section 23: backends are faked, so
// core tests never download models or install runtimes). The fakes observe the
// prepared view and echo deterministic regions in prepared-image pixel space,
// honoring the DEC-012 contract: they read min_confidence and the context, and
// leave pipeline fields alone.

#include <mirador/backend_info.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <string>
#include <utility>
#include <vector>

namespace mirador::test {

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): test doubles
// expose their knobs and observations by design.
struct FakeOcrBackend final : OcrBackend {
    [[nodiscard]] BackendInfo info() const override { return info_value; }

    BackendInfo info_value;
    int call_count = 0;
    /// When non-empty, returned verbatim instead of the synthesized region.
    std::vector<TextRegion> next_regions;
    /// When not ok, returned as the call result.
    Status next_status;

    // Observations of the last call.
    int last_prepared_width = 0;
    int last_prepared_height = 0;
    PixelFormat last_format = PixelFormat::kRgba8;
    float last_min_confidence = -1.0F;
    std::string last_language_hint;

    FakeOcrBackend() {
        info_value.name = "fake-ocr";
        info_value.implementation_version = "1.0.0";
        info_value.model_id = "fake-model";
        info_value.model_revision = "r1";
        info_value.accepted_formats = {PixelFormat::kRgba8, PixelFormat::kGray8};
        info_value.thread_safe = true;
    }

    Result<std::vector<TextRegion>> recognize(const ImageView& prepared_image, const OcrRequest& request,
                                              const ExecutionContext& context) override {
        ++call_count;
        last_prepared_width = prepared_image.width;
        last_prepared_height = prepared_image.height;
        last_format = prepared_image.format;
        last_min_confidence = request.min_confidence;
        last_language_hint = request.language_hint;
        if (is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "fake-ocr observed cancellation");
        }
        if (deadline_reached(context)) {
            return Status(ErrorCode::kTimeout, "fake-ocr observed deadline");
        }
        if (!next_status.ok()) {
            return next_status;
        }
        if (!next_regions.empty()) {
            return next_regions;
        }
        TextRegion region;
        region.bounds =
            RectF{static_cast<float>(prepared_image.width) * 0.25F, static_cast<float>(prepared_image.height) * 0.25F,
                  static_cast<float>(prepared_image.width) * 0.50F, static_cast<float>(prepared_image.height) * 0.50F};
        region.utf8_text = "fake";
        region.confidence = 0.9F;
        region.polygon = {{region.bounds.x, region.bounds.y},
                          {region.bounds.x + region.bounds.width, region.bounds.y},
                          {region.bounds.x + region.bounds.width, region.bounds.y + region.bounds.height},
                          {region.bounds.x, region.bounds.y + region.bounds.height}};
        if (region.confidence < request.min_confidence) {
            return std::vector<TextRegion>{};  // DEC-012: the backend owns min_confidence filtering
        }
        return std::vector<TextRegion>{std::move(region)};
    }
};

struct FakeDetectorBackend final : DetectorBackend {
    [[nodiscard]] BackendInfo info() const override { return info_value; }

    BackendInfo info_value;
    int call_count = 0;

    FakeDetectorBackend() {
        info_value.name = "fake-detector";
        info_value.implementation_version = "2.0.0";
        info_value.model_id = "fake-det-model";
        info_value.model_revision = "r9";
        info_value.accepted_formats = {PixelFormat::kGray8};
        info_value.thread_safe = false;
    }

    Result<std::vector<DetectionRegion>> detect(const ImageView& prepared_image, const DetectionRequest& request,
                                                const ExecutionContext& context) override {
        ++call_count;
        if (is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "fake-detector observed cancellation");
        }
        if (deadline_reached(context)) {
            return Status(ErrorCode::kTimeout, "fake-detector observed deadline");
        }
        DetectionRegion region;
        region.bounds = RectF{0.0F, 0.0F, static_cast<float>(prepared_image.width) * 0.5F,
                              static_cast<float>(prepared_image.height) * 0.5F};
        region.class_id = 3;
        region.label = "icon";
        region.confidence = 0.8F;
        if (region.confidence < request.min_confidence) {
            return std::vector<DetectionRegion>{};
        }
        return std::vector<DetectionRegion>{std::move(region)};
    }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

}  // namespace mirador::test

#endif  // MIRADOR_TESTS_FUSION_FAKE_BACKENDS_HPP
