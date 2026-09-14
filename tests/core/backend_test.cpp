// Unit tests for the M2 Backend SPI public contracts (M2-01, DEC-012):
// ExecutionContext cancellation/deadline helpers, BackendInfo validation and
// the abstract OcrBackend/DetectorBackend call surface.

#include <mirador/backend_info.hpp>
#include <mirador/cache_policy.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <vector>

namespace {

using mirador::BackendInfo;
using mirador::CachePolicy;
using mirador::CoordinateSpaceId;
using mirador::deadline_reached;
using mirador::DetectionRegion;
using mirador::DetectionRequest;
using mirador::DetectorBackend;
using mirador::ExecutionContext;
using mirador::is_cancelled;
using mirador::OcrBackend;
using mirador::OcrRequest;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::TextRegion;

// --- ExecutionContext helpers -------------------------------------------------

TEST(ExecutionContextTest, NullCallbackIsNeverCancelled) {
    const ExecutionContext context;
    EXPECT_FALSE(is_cancelled(context));
    EXPECT_FALSE(deadline_reached(context));
}

TEST(ExecutionContextTest, CallbackResultIsReported) {
    ExecutionContext context;
    context.is_cancelled = [] { return false; };
    EXPECT_FALSE(is_cancelled(context));
    context.is_cancelled = [] { return true; };
    EXPECT_TRUE(is_cancelled(context));
}

TEST(ExecutionContextTest, ThrowingCallbackCountsAsCancelled) {
    ExecutionContext context;
    context.is_cancelled = []() -> bool { throw std::runtime_error("flag broken"); };
    EXPECT_TRUE(is_cancelled(context));
}

TEST(ExecutionContextTest, DeadlineIsHonored) {
    ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
    EXPECT_FALSE(deadline_reached(context));
    context.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    EXPECT_TRUE(deadline_reached(context));
}

// --- BackendInfo validation ---------------------------------------------------

BackendInfo usable_info() {
    BackendInfo info;
    info.name = "fake";
    info.implementation_version = "1.0.0";
    info.model_id = "fake-model";
    info.model_revision = "r0";
    info.accepted_formats = {PixelFormat::kGray8, PixelFormat::kRgba8};
    info.thread_safe = true;
    return info;
}

TEST(BackendInfoTest, ValidInfoPasses) {
    const BackendInfo info = usable_info();
    ASSERT_TRUE(mirador::validate(info).ok());
}

TEST(BackendInfoTest, DefaultConstructedInfoIsRejected) {
    const BackendInfo info;
    const mirador::Result<void> result = mirador::validate(info);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), mirador::ErrorCode::kBackendUnavailable);
}

TEST(BackendInfoTest, EmptyIdentityFieldsAreRejected) {
    BackendInfo info = usable_info();
    info.name.clear();
    EXPECT_EQ(mirador::validate(info).status().code(), mirador::ErrorCode::kBackendUnavailable);

    info = usable_info();
    info.implementation_version.clear();
    EXPECT_EQ(mirador::validate(info).status().code(), mirador::ErrorCode::kBackendUnavailable);
}

TEST(BackendInfoTest, EmptyOrUndefinedFormatsAreRejected) {
    BackendInfo info = usable_info();
    info.accepted_formats.clear();
    EXPECT_EQ(mirador::validate(info).status().code(), mirador::ErrorCode::kBackendUnavailable);

    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): undefined-value contract test
    info.accepted_formats = {static_cast<PixelFormat>(200)};
    EXPECT_EQ(mirador::validate(info).status().code(), mirador::ErrorCode::kBackendUnavailable);
}

TEST(BackendInfoTest, ModelFreeBackendMayOmitModelIdentity) {
    BackendInfo info = usable_info();
    info.model_id.clear();
    info.model_revision.clear();
    EXPECT_TRUE(mirador::validate(info).ok());
}

// --- Request defaults (DEC-012 contract) ---------------------------------------

TEST(BackendRequestTest, OcrRequestDefaults) {
    const OcrRequest request;
    EXPECT_FALSE(request.roi.has_value());
    EXPECT_EQ(request.roi_space, CoordinateSpaceId::kOriented);
    EXPECT_EQ(request.min_confidence, 0.0F);
    EXPECT_EQ(request.max_side, 0);
    EXPECT_EQ(request.output_space, CoordinateSpaceId::kOriented);
    EXPECT_EQ(request.cache_policy, CachePolicy::kReadWrite);
    EXPECT_TRUE(request.language_hint.empty());
    EXPECT_TRUE(request.backend_params.empty());
}

TEST(BackendRequestTest, DetectionRequestDefaults) {
    const DetectionRequest request;
    EXPECT_FALSE(request.roi.has_value());
    EXPECT_EQ(request.roi_space, CoordinateSpaceId::kOriented);
    EXPECT_EQ(request.min_confidence, 0.0F);
    EXPECT_EQ(request.max_side, 0);
    EXPECT_EQ(request.output_space, CoordinateSpaceId::kOriented);
    EXPECT_EQ(request.cache_policy, CachePolicy::kReadWrite);
    EXPECT_TRUE(request.backend_params.empty());
}

// --- SPI call surface through a minimal concrete implementation -----------------

class NullOcrBackend final : public OcrBackend {
public:
    [[nodiscard]] BackendInfo info() const override { return usable_info(); }
    mirador::Result<std::vector<TextRegion>> recognize(const mirador::ImageView& /*prepared_image*/,
                                                       const OcrRequest& /*request*/,
                                                       const ExecutionContext& /*context*/) override {
        return std::vector<TextRegion>{};
    }
};

class NullDetectorBackend final : public DetectorBackend {
public:
    [[nodiscard]] BackendInfo info() const override { return usable_info(); }
    mirador::Result<std::vector<DetectionRegion>> detect(const mirador::ImageView& /*prepared_image*/,
                                                         const DetectionRequest& /*request*/,
                                                         const ExecutionContext& /*context*/) override {
        return std::vector<DetectionRegion>{};
    }
};

TEST(BackendSpiTest, ConcreteImplementationsAreCallableThroughTheInterface) {
    NullOcrBackend ocr;
    NullDetectorBackend detector;
    const mirador::ImageView view;  // backends may inspect; the fake ignores it
    const ExecutionContext context;

    OcrRequest ocr_request;
    ocr_request.roi = RectF{1.0F, 2.0F, 3.0F, 4.0F};
    const mirador::Result<std::vector<TextRegion>> ocr_result = ocr.recognize(view, ocr_request, context);
    ASSERT_TRUE(ocr_result.ok());
    EXPECT_TRUE(ocr_result.value().empty());

    const mirador::Result<std::vector<DetectionRegion>> detection_result =
        detector.detect(view, DetectionRequest{}, context);
    ASSERT_TRUE(detection_result.ok());
    EXPECT_TRUE(detection_result.value().empty());

    EXPECT_TRUE(mirador::validate(ocr.info()).ok());
}

TEST(BackendSpiTest, PolymorphicDestructionThroughBasePointer) {
    OcrBackend* ocr = new NullOcrBackend();
    DetectorBackend* detector = new NullDetectorBackend();
    delete ocr;
    delete detector;
}

}  // namespace
