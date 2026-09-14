// M3-08: unit tests for the small-target crop-refine combinator (design
// section 14, DEC-014 reference adaptation scope).

#include <mirador/crop_refine.hpp>
#include <mirador/image_buffer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <utility>
#include <vector>

namespace {

using mirador::BackendInfo;
using mirador::CoordinateSpaceId;
using mirador::CropRefineParams;
using mirador::DetectionRegion;
using mirador::DetectionRequest;
using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::RectI;
using mirador::Status;

/// Records the prepared view of every call and returns fixed model-space
/// regions; honors min_confidence and the cancellation contract.
class RecordingDetector final : public mirador::DetectorBackend {
public:
    explicit RecordingDetector(std::vector<DetectionRegion> results) : results_(std::move(results)) {}

    [[nodiscard]] BackendInfo info() const override {
        BackendInfo info;
        info.name = "recording-detector";
        info.implementation_version = "1.0.0";
        info.accepted_formats = accepted_formats_;
        return info;
    }

    mirador::Result<std::vector<DetectionRegion>> detect(const ImageView& prepared_image,
                                                         const DetectionRequest& request,
                                                         const ExecutionContext& context) override {
        ++calls_;
        last_format_ = prepared_image.format;
        last_width_ = prepared_image.width;
        last_height_ = prepared_image.height;
        last_min_confidence_ = request.min_confidence;
        if (mirador::is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "cancelled");
        }
        std::vector<DetectionRegion> kept;
        for (const DetectionRegion& region : results_) {
            if (region.confidence >= request.min_confidence) {
                kept.push_back(region);
            }
        }
        return kept;
    }

    int calls() const noexcept { return calls_; }
    PixelFormat last_format() const noexcept { return last_format_; }
    int32_t last_width() const noexcept { return last_width_; }
    int32_t last_height() const noexcept { return last_height_; }
    void set_accepted_formats(std::vector<PixelFormat> formats) { accepted_formats_ = std::move(formats); }

private:
    std::vector<DetectionRegion> results_;
    std::vector<PixelFormat> accepted_formats_{PixelFormat::kGray8};
    int calls_ = 0;
    PixelFormat last_format_ = PixelFormat::kGray8;
    int32_t last_width_ = 0;
    int32_t last_height_ = 0;
    float last_min_confidence_ = -1.0F;
};

DetectionRegion make_region(float x, float y, float w, float h, float confidence, std::string label = "refined") {
    DetectionRegion region;
    region.bounds = RectF{x, y, w, h};
    region.confidence = confidence;
    region.class_id = 3;
    region.label = std::move(label);
    return region;
}

ImageView gray_view(std::vector<uint8_t>& pixels, int32_t width, int32_t height) {
    pixels.assign(static_cast<size_t>(width) * height, 128);
    ImageView view;
    view.data = reinterpret_cast<const std::byte*>(pixels.data());
    view.width = width;
    view.height = height;
    view.row_stride_bytes = width;
    view.format = PixelFormat::kGray8;
    return view;
}

TEST(CropRefine, RefinesLowConfidenceCandidateWithExactRecovery) {
    std::vector<uint8_t> pixels;
    const ImageView source = gray_view(pixels, 100, 100);
    const std::vector<DetectionRegion> initial{
        make_region(10.0F, 10.0F, 20.0F, 20.0F, 0.3F),  // below 0.5: refined
        make_region(60.0F, 60.0F, 30.0F, 30.0F, 0.9F),  // untouched
    };
    // The backend reports a box in prepared-crop pixel space.
    RecordingDetector detector({make_region(2.0F, 2.0F, 10.0F, 10.0F, 0.8F)});

    CropRefineParams params;
    params.expand_ratio = 0.25F;  // (10,10,20,20) -> (5,5,30,30)
    params.refine_target_side = 0;
    const auto refined = mirador::refine_small_detections(source, &detector, initial, params, {});
    ASSERT_TRUE(refined.ok()) << refined.status().message();
    ASSERT_EQ(detector.calls(), 1);
    ASSERT_EQ(refined.value().size(), 2U);

    // Crop origin (5,5), no resample: (2,2,10,10) maps to (7,7,10,10) in source space.
    EXPECT_EQ(refined.value()[0].bounds, (RectF{7.0F, 7.0F, 10.0F, 10.0F}));
    EXPECT_EQ(refined.value()[0].label, "refined");
    EXPECT_FLOAT_EQ(refined.value()[0].confidence, 0.8F);
    // The untouched proposal passes through byte-identical.
    EXPECT_EQ(refined.value()[1], initial[1]);
}

TEST(CropRefine, ResamplesCropAndRecoversCoordinates) {
    std::vector<uint8_t> pixels;
    const ImageView source = gray_view(pixels, 100, 100);
    const std::vector<DetectionRegion> initial{make_region(10.0F, 10.0F, 20.0F, 20.0F, 0.3F)};
    RecordingDetector detector({make_region(4.0F, 4.0F, 20.0F, 20.0F, 0.8F)});

    CropRefineParams params;
    params.expand_ratio = 0.25F;      // crop (5,5,30,30)
    params.refine_target_side = 60;   // resample x2 -> 60x60 model input
    const auto refined = mirador::refine_small_detections(source, &detector, initial, params, {});
    ASSERT_TRUE(refined.ok()) << refined.status().message();
    ASSERT_EQ(detector.calls(), 1);
    EXPECT_EQ(detector.last_width(), 60);
    EXPECT_EQ(detector.last_height(), 60);
    // Model-space (4,4,20,20) / 2 + crop origin (5,5) -> (7,7,10,10).
    EXPECT_EQ(refined.value()[0].bounds, (RectF{7.0F, 7.0F, 10.0F, 10.0F}));
}

TEST(CropRefine, AreaRuleAndCandidateCapSelectDeterministically) {
    std::vector<uint8_t> pixels;
    const ImageView source = gray_view(pixels, 100, 100);
    const std::vector<DetectionRegion> initial{
        make_region(0.0F, 0.0F, 3.0F, 3.0F, 0.95F),   // small area, high confidence
        make_region(20.0F, 0.0F, 3.0F, 3.0F, 0.95F),  // small area, high confidence
        make_region(50.0F, 50.0F, 40.0F, 40.0F, 0.9F),
    };
    RecordingDetector detector({});

    CropRefineParams params;
    params.small_box_area_ratio = 0.01;  // 9 px of 10000 < 0.01
    params.refine_confidence_below = 0.0;
    params.max_refine_candidates = 1;    // tie on confidence: index 0 wins
    const auto refined = mirador::refine_small_detections(source, &detector, initial, params, {});
    ASSERT_TRUE(refined.ok());
    ASSERT_EQ(detector.calls(), 1);
    // Untouched proposals keep their slots; refined index 0 expands from the backend (empty) result.
    ASSERT_EQ(refined.value().size(), 2U);
    EXPECT_EQ(refined.value()[0], initial[1]);
    EXPECT_EQ(refined.value()[1], initial[2]);
}

TEST(CropRefine, ConvertsFormatForGrayOnlyBackend) {
    std::vector<uint8_t> rgb_pixels(64 * 64 * 3, 90);
    ImageView source;
    source.data = reinterpret_cast<const std::byte*>(rgb_pixels.data());
    source.width = 64;
    source.height = 64;
    source.row_stride_bytes = 192;
    source.format = PixelFormat::kRgb8;

    RecordingDetector detector({make_region(0.0F, 0.0F, 4.0F, 4.0F, 0.8F)});
    std::vector<uint8_t> placeholder(8, 0);
    ImageView valid;  // unused; a region for refinement needs a box below the bar
    valid.data = reinterpret_cast<const std::byte*>(placeholder.data());
    const std::vector<DetectionRegion> initial{make_region(10.0F, 10.0F, 20.0F, 20.0F, 0.3F)};

    CropRefineParams params;
    const auto refined = mirador::refine_small_detections(source, &detector, initial, params, {});
    ASSERT_TRUE(refined.ok()) << refined.status().message();
    EXPECT_EQ(detector.last_format(), PixelFormat::kGray8);  // converted before the call
}

TEST(CropRefine, ExplicitErrors) {
    std::vector<uint8_t> pixels;
    const ImageView source = gray_view(pixels, 64, 64);
    const std::vector<DetectionRegion> initial{make_region(8.0F, 8.0F, 16.0F, 16.0F, 0.3F)};
    RecordingDetector detector({make_region(1.0F, 1.0F, 4.0F, 4.0F, 0.8F)});

    CropRefineParams params;
    ASSERT_EQ(mirador::refine_small_detections(source, nullptr, initial, params, {}).status().code(),
              ErrorCode::kBackendUnavailable);

    // Invalid backend capability report.
    RecordingDetector unnamed({});
    // (name/implementation fields left empty by default only in a fresh info();
    // RecordingDetector sets them, so wrap with an anonymous backend instead.)
    struct NullInfoDetector final : public mirador::DetectorBackend {
        [[nodiscard]] BackendInfo info() const override { return {}; }
        mirador::Result<std::vector<DetectionRegion>> detect(const ImageView&, const DetectionRequest&,
                                                             const ExecutionContext&) override {
            return Status(ErrorCode::kBackendFailure, "unused");
        }
    } bad_info;
    ASSERT_EQ(mirador::refine_small_detections(source, &bad_info, initial, params, {}).status().code(),
              ErrorCode::kBackendUnavailable);

    CropRefineParams bad_params;
    bad_params.expand_ratio = -1.0F;
    ASSERT_EQ(mirador::refine_small_detections(source, &detector, initial, bad_params, {}).status().code(),
              ErrorCode::kInvalidArgument);

    // A valid-but-tight per-candidate budget: the expanded crop cannot fit.
    std::vector<uint8_t> big_pixels;
    const ImageView big_source = gray_view(big_pixels, 128, 128);
    const std::vector<DetectionRegion> big_box{make_region(8.0F, 8.0F, 100.0F, 100.0F, 0.3F)};
    CropRefineParams tight_budget;
    tight_budget.per_crop_budget_bytes = 1024;  // the ~113x113 crop needs far more
    ASSERT_EQ(mirador::refine_small_detections(big_source, &detector, big_box, tight_budget, {}).status().code(),
              ErrorCode::kBudgetExceeded);

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    ASSERT_EQ(mirador::refine_small_detections(source, &detector, initial, params, cancelled).status().code(),
              ErrorCode::kCancelled);
    EXPECT_EQ(detector.calls(), 0);  // zero backend calls on early cancellation

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    ASSERT_EQ(mirador::refine_small_detections(source, &detector, initial, params, expired).status().code(),
              ErrorCode::kTimeout);

    // NV12 sources are rejected pending the chroma decision.
    auto nv12 = mirador::ImageBuffer::create(PixelFormat::kNv12, 32, 32, 8192);
    ASSERT_TRUE(nv12.ok());
    ASSERT_EQ(mirador::refine_small_detections(nv12.value().view(), &detector, initial, params, {}).status().code(),
              ErrorCode::kUnsupportedFormat);
}

}  // namespace
