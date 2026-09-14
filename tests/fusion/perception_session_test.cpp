// End-to-end M2 tests (M2-04/M2-05, DEC-008/DEC-012): the perception session
// loop over Fake Backends — cache round trips and explicit refresh/invalidation,
// coordinate recovery across rotations, strides, odd sizes, ROI and max_side,
// cancellation/deadline paths, capability gating and budget errors.

#include <mirador/perception_session.hpp>

#include "fake_backends.hpp"

#include <mirador/cache_policy.hpp>
#include <mirador/capability_cache.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using mirador::test::FakeDetectorBackend;
using mirador::test::FakeOcrBackend;

using mirador::CachePolicy;
using mirador::ChangeClassification;
using mirador::ChangeReason;
using mirador::CoordinateSpaceId;
using mirador::DetectionRequest;
using mirador::ErrorCode;
using mirador::Frame;
using mirador::OcrRequest;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::Rotation;
using mirador::Status;

constexpr const char* kSourceId = "screen-main";

/// Test-owned frame: padded-stride buffer plus a valid view and owner.
struct TestFrame {
    std::vector<std::byte> bytes;
    Frame frame;
};

TestFrame make_frame(int32_t width, int32_t height, int32_t stride, PixelFormat format, Rotation rotation,
                     const char* source_id = kSourceId, uint8_t tint = 0) {
    const auto stride64 = static_cast<int64_t>(stride);
    const int bpp = mirador::bytes_per_pixel(format);
    TestFrame image;
    image.bytes.assign(static_cast<size_t>(stride64) * static_cast<size_t>(height), std::byte{7});
    for (int32_t y = 0; y < height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * stride64;
        for (int32_t x = 0; x < width; ++x) {
            for (int c = 0; c < bpp; ++c) {
                row[static_cast<int64_t>(x) * bpp + c] = static_cast<std::byte>((x * 31 + y * 17 + c * 5 + tint) % 251);
            }
        }
    }
    image.frame.image.data = image.bytes.data();
    image.frame.image.width = width;
    image.frame.image.height = height;
    image.frame.image.row_stride_bytes = stride;
    image.frame.image.format = format;
    image.frame.image.rotation = rotation;
    image.frame.sequence = 1;
    image.frame.source_id = source_id;
    image.frame.owner = std::make_shared<const std::vector<std::byte>>(image.bytes);
    return image;
}

PerceptionSession make_session(int64_t result_cache_bytes = int64_t{16} * 1024 * 1024) {
    PerceptionSessionOptions options;
    options.source_id = kSourceId;
    options.result_cache_bytes = result_cache_bytes;
    return PerceptionSession::create(std::move(options)).take_value();
}

bool near(float actual, float expected, float tolerance = 0.01F) {
    return std::abs(actual - expected) <= tolerance;
}

// --- Session configuration (DEC-008) -------------------------------------------

TEST(PerceptionSessionConfig, DefaultBudgetsMatchDec008) {
    const PerceptionSessionOptions options;
    EXPECT_EQ(options.frame_cache_bytes, 4 * 1024 * 1024);
    EXPECT_EQ(options.result_cache_bytes, 16 * 1024 * 1024);
}

TEST(PerceptionSessionConfig, RejectsNonPositiveBudgets) {
    PerceptionSessionOptions zero;
    zero.frame_cache_bytes = 0;
    EXPECT_EQ(PerceptionSession::create(zero).status().code(), ErrorCode::kInvalidArgument);
    PerceptionSessionOptions negative;
    negative.result_cache_bytes = -5;
    EXPECT_EQ(PerceptionSession::create(negative).status().code(), ErrorCode::kInvalidArgument);
}

// --- OCR loop: caching, refresh and invalidation ---------------------------------

TEST(OcrSessionLoop, FullFrameRoundTripHitsCacheOnSecondCall) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest request;
    const auto first = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().size(), 1U);
    // Identity chain (no rotation, full frame, no resize): prepared bounds
    // survive recovery unchanged.
    EXPECT_NEAR(first.value()[0].bounds.x, 25.0F, 0.01F);
    EXPECT_NEAR(first.value()[0].bounds.y, 20.0F, 0.01F);
    EXPECT_NEAR(first.value()[0].bounds.width, 50.0F, 0.01F);
    EXPECT_NEAR(first.value()[0].bounds.height, 40.0F, 0.01F);
    EXPECT_EQ(backend.call_count, 1);

    const auto second = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(backend.call_count, 1);  // served from the capability cache
    ASSERT_EQ(second.value().size(), 1U);
    EXPECT_EQ(second.value()[0].bounds.x, first.value()[0].bounds.x);
    EXPECT_EQ(second.value()[0].utf8_text, "fake");
}

TEST(OcrSessionLoop, RefreshBypassesCacheAndOverwrites) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);

    OcrRequest request;
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    request.cache_policy = CachePolicy::kRefresh;
    const auto refreshed = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(refreshed.ok());
    EXPECT_EQ(backend.call_count, 2);
    EXPECT_EQ(session.result_cache().entry_count(), 1);  // same key, overwritten

    // The refreshed entry is visible to a subsequent plain read.
    const OcrRequest read{};
    const auto hit = session.run_ocr(image.frame, &backend, read);
    ASSERT_TRUE(hit.ok());
    EXPECT_EQ(backend.call_count, 2);
}

TEST(OcrSessionLoop, ReadOnlyNeverStores) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);

    OcrRequest request;
    request.cache_policy = CachePolicy::kReadOnly;
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    EXPECT_EQ(backend.call_count, 2);
    EXPECT_EQ(session.result_cache().entry_count(), 0);
}

TEST(OcrSessionLoop, ModelRevisionChangeInvalidates) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest request;
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    backend.info_value.model_revision = "r2";
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    EXPECT_EQ(backend.call_count, 2);
}

TEST(OcrSessionLoop, RequestParameterChangesInvalidate) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest request;
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());

    OcrRequest language = request;
    language.language_hint = "en";
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, language).ok());

    OcrRequest confidence = request;
    confidence.min_confidence = 0.5F;
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, confidence).ok());

    OcrRequest side = request;
    side.max_side = 64;
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, side).ok());

    OcrRequest params = request;
    params.backend_params = "{\"beam\":4}";
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, params).ok());

    EXPECT_EQ(backend.call_count, 5);  // every parameter change misses
}

TEST(OcrSessionLoop, RoiChangeInvalidatesAndCropsCoordinates) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);

    OcrRequest request;
    request.roi = RectF{10.0F, 20.0F, 30.0F, 40.0F};
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(backend.last_prepared_width, 30);
    EXPECT_EQ(backend.last_prepared_height, 40);
    // Fake returns the central half of the 30x40 crop; recovery adds the ROI
    // origin back in the oriented space.
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_NEAR(result.value()[0].bounds.x, 10.0F + 7.5F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.y, 20.0F + 10.0F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.width, 15.0F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.height, 20.0F, 0.01F);

    OcrRequest other_roi = request;
    other_roi.roi = RectF{0.0F, 0.0F, 20.0F, 20.0F};
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, other_roi).ok());
    EXPECT_EQ(backend.call_count, 2);  // different ROI => different key

    // Second identical ROI request hits the cache.
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    EXPECT_EQ(backend.call_count, 2);
}

TEST(OcrSessionLoop, OutputSpaceIsPartOfTheKey) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest oriented;
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, oriented).ok());
    OcrRequest cropped = oriented;
    cropped.output_space = CoordinateSpaceId::kCropped;
    const auto in_crop_space = session.run_ocr(image.frame, &backend, cropped);
    ASSERT_TRUE(in_crop_space.ok());
    // Same full-frame crop: kCropped recovery equals the oriented one for a
    // non-rotated frame, but a separate cache entry was created.
    EXPECT_EQ(backend.call_count, 2);
    ASSERT_EQ(in_crop_space.value().size(), 1U);
    EXPECT_NEAR(in_crop_space.value()[0].bounds.x, 25.0F, 0.01F);
}

// --- Coordinate recovery matrix (DOD-03) -----------------------------------------

TEST(CoordinateRecovery, Rotation90RoundTripsThroughFrameAndOrientedSpaces) {
    // Raw 100x80 capture presented as an 80x100 view (k90).
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(80, 100, 80 * 4, PixelFormat::kRgba8, Rotation::k90);

    const mirador::Transform2D frame_to_oriented =
        mirador::make_rotation(Rotation::k90, 100, 80, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);

    const OcrRequest oriented_request;
    const auto in_oriented = session.run_ocr(image.frame, &backend, oriented_request);
    ASSERT_TRUE(in_oriented.ok());
    ASSERT_EQ(in_oriented.value().size(), 1U);
    // Recovered to oriented: the fake's proportional box of the full view.
    EXPECT_NEAR(in_oriented.value()[0].bounds.x, 20.0F, 0.01F);
    EXPECT_NEAR(in_oriented.value()[0].bounds.y, 25.0F, 0.01F);
    EXPECT_NEAR(in_oriented.value()[0].bounds.width, 40.0F, 0.01F);
    EXPECT_NEAR(in_oriented.value()[0].bounds.height, 50.0F, 0.01F);

    // Mapping the recovered oriented bounds to the raw frame through the
    // public inverse factory, then back, reproduces the oriented bounds.
    const mirador::Transform2D oriented_to_frame = mirador::inverse(frame_to_oriented).value();
    const RectF in_frame_mapped = mirador::transform_rect(oriented_to_frame, in_oriented.value()[0].bounds);
    const RectF roundtrip = mirador::transform_rect(frame_to_oriented, in_frame_mapped);
    EXPECT_NEAR(roundtrip.x, 20.0F, 0.01F);
    EXPECT_NEAR(roundtrip.y, 25.0F, 0.01F);
    EXPECT_NEAR(roundtrip.width, 40.0F, 0.01F);
    EXPECT_NEAR(roundtrip.height, 50.0F, 0.01F);

    // Same execution in raw-frame space: the stored entry is separate (a
    // different output space is a different key) and must match the
    // independently mapped frame-space box.
    OcrRequest frame_request;
    frame_request.output_space = CoordinateSpaceId::kFrame;
    const auto in_frame = session.run_ocr(image.frame, &backend, frame_request);
    ASSERT_TRUE(in_frame.ok());
    EXPECT_EQ(backend.call_count, 2);
    ASSERT_EQ(in_frame.value().size(), 1U);
    EXPECT_NEAR(in_frame.value()[0].bounds.x, in_frame_mapped.x, 0.01F);
    EXPECT_NEAR(in_frame.value()[0].bounds.y, in_frame_mapped.y, 0.01F);
    EXPECT_NEAR(in_frame.value()[0].bounds.width, in_frame_mapped.width, 0.01F);
    EXPECT_NEAR(in_frame.value()[0].bounds.height, in_frame_mapped.height, 0.01F);
    // Frame-space polygon survives too.
    ASSERT_EQ(in_frame.value()[0].polygon.size(), 4U);
}

namespace {

/// Presented dimensions of a rotated raw capture (k180 keeps, k90/k270 swap).
std::pair<int32_t, int32_t> view_dims(Rotation rotation, int32_t raw_width, int32_t raw_height) {
    if (rotation == Rotation::k180) {
        return {raw_width, raw_height};
    }
    return {raw_height, raw_width};
}

/// Shared body of the k180/k270 recovery roundtrip: the session's recovered
/// oriented region, mapped to the raw frame and back through the public
/// factories, must reproduce the fake's proportional oriented box.
void check_rotation_roundtrip(Rotation rotation, int32_t raw_width, int32_t raw_height) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const auto [view_w, view_h] = view_dims(rotation, raw_width, raw_height);
    const TestFrame image = make_frame(view_w, view_h, view_w * 4, PixelFormat::kRgba8, rotation);

    const OcrRequest request;
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 1U);

    const RectF oriented_bounds{static_cast<float>(view_w) * 0.25F, static_cast<float>(view_h) * 0.25F,
                                static_cast<float>(view_w) * 0.5F, static_cast<float>(view_h) * 0.5F};
    const mirador::Transform2D to_frame =
        mirador::inverse(mirador::make_rotation(rotation, raw_width, raw_height, CoordinateSpaceId::kFrame,
                                                CoordinateSpaceId::kOriented))
            .value();
    const RectF in_frame = mirador::transform_rect(to_frame, result.value()[0].bounds);
    const mirador::Transform2D back_to_oriented = mirador::make_rotation(
        rotation, raw_width, raw_height, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
    const RectF roundtrip = mirador::transform_rect(back_to_oriented, in_frame);
    EXPECT_NEAR(roundtrip.x, oriented_bounds.x, 0.01F);
    EXPECT_NEAR(roundtrip.y, oriented_bounds.y, 0.01F);
    EXPECT_NEAR(roundtrip.width, oriented_bounds.width, 0.01F);
    EXPECT_NEAR(roundtrip.height, oriented_bounds.height, 0.01F);
}

}  // namespace

TEST(CoordinateRecovery, Rotation180MatchesForwardMapping) {
    check_rotation_roundtrip(Rotation::k180, 100, 80);
}

TEST(CoordinateRecovery, Rotation270MatchesForwardMapping) {
    check_rotation_roundtrip(Rotation::k270, 80, 100);
}

TEST(CoordinateRecovery, PaddedStrideAndOddSizeKeepRegionsExact) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    // 61x45 gray view with 16-byte-aligned (padded) stride.
    const TestFrame image = make_frame(61, 45, 64, PixelFormat::kGray8, Rotation::k0);

    const OcrRequest request;
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(backend.last_format, PixelFormat::kGray8);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_NEAR(result.value()[0].bounds.x, 61.0F * 0.25F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.width, 61.0F * 0.5F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.height, 45.0F * 0.5F, 0.01F);

    // Odd ROI: enclosing rect clamps to the view.
    OcrRequest odd_roi = request;
    odd_roi.roi = RectF{5.5F, 7.25F, 21.5F, 13.5F};
    const auto odd = session.run_ocr(image.frame, &backend, odd_roi);
    ASSERT_TRUE(odd.ok());
    EXPECT_EQ(backend.last_prepared_width, 22);
    EXPECT_EQ(backend.last_prepared_height, 14);
}

TEST(CoordinateRecovery, MaxSideScalesRegionsBackToSourceResolution) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(200, 100, 200 * 4, PixelFormat::kRgba8, Rotation::k0);

    OcrRequest request;
    request.max_side = 50;
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    // 200x100 downscaled to 50x25; the fake's proportional box maps back by 4.
    EXPECT_EQ(backend.last_prepared_width, 50);
    EXPECT_EQ(backend.last_prepared_height, 25);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_NEAR(result.value()[0].bounds.x, 50.0F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.y, 25.0F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.width, 100.0F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.height, 50.0F, 0.01F);
    // The scaled run is cached under its own key.
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    EXPECT_EQ(backend.call_count, 1);
}

TEST(CoordinateRecovery, FrameSpaceRoiIsMappedThroughRotation) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    // Raw 100x80 capture, presented 80x100 (k90).
    const TestFrame image = make_frame(80, 100, 80 * 4, PixelFormat::kRgba8, Rotation::k90);

    // A raw-frame rect whose oriented mapping is a known box.
    OcrRequest request;
    request.roi_space = CoordinateSpaceId::kFrame;
    request.roi = RectF{0.0F, 0.0F, 100.0F, 80.0F};  // the whole raw capture
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    // The executed ROI covers the whole view.
    EXPECT_EQ(backend.last_prepared_width, 80);
    EXPECT_EQ(backend.last_prepared_height, 100);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_NEAR(result.value()[0].bounds.x, 20.0F, 0.01F);
    EXPECT_NEAR(result.value()[0].bounds.y, 25.0F, 0.01F);
}

// --- Format gating and backend errors ---------------------------------------------

TEST(CapabilityGating, FormatConversionHappensForAcceptedAlternative) {
    PerceptionSession session = make_session();
    FakeDetectorBackend backend;  // accepts kGray8 only
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    const DetectionRequest request;
    const auto result = session.run_detector(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    // RGBA converted to gray before the backend ran.
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_NEAR(result.value()[0].bounds.width, 20.0F, 0.01F);

    // Cached per capability kind: the OCR cache does not serve detections.
    FakeOcrBackend ocr;
    const OcrRequest ocr_request;
    ASSERT_TRUE(session.run_ocr(image.frame, &ocr, ocr_request).ok());
    ASSERT_TRUE(session.run_ocr(image.frame, &ocr, ocr_request).ok());
    EXPECT_EQ(ocr.call_count, 1);
}

TEST(CapabilityGating, UnreachableFormatReportsUnsupported) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    backend.info_value.accepted_formats = {PixelFormat::kNv12};  // RGBA -> NV12 is unsupported
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest request;
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kUnsupportedFormat);
    EXPECT_EQ(backend.call_count, 0);
}

TEST(CapabilityGating, NullBackendAndInvalidInfoReportBackendUnavailable) {
    PerceptionSession session = make_session();
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest request;
    const auto null_result = session.run_ocr(image.frame, nullptr, request);
    ASSERT_FALSE(null_result.ok());
    EXPECT_EQ(null_result.status().code(), ErrorCode::kBackendUnavailable);

    FakeOcrBackend backend;
    backend.info_value.name.clear();
    const auto invalid_result = session.run_ocr(image.frame, &backend, request);
    ASSERT_FALSE(invalid_result.ok());
    EXPECT_EQ(invalid_result.status().code(), ErrorCode::kBackendUnavailable);
    EXPECT_EQ(backend.call_count, 0);
}

TEST(CapabilityGating, BackendFailurePropagates) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    backend.next_status = Status(ErrorCode::kBackendFailure, "fake inference failed");
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest request;
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBackendFailure);
    EXPECT_EQ(backend.call_count, 1);

    // Nothing was cached by the failed attempt: clearing the injected failure
    // makes the retry execute and store.
    backend.next_status = Status::success();
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    EXPECT_EQ(backend.call_count, 2);
    ASSERT_TRUE(session.run_ocr(image.frame, &backend, request).ok());
    EXPECT_EQ(backend.call_count, 2);  // now served from the cache
}

// --- Cancellation, deadline, source and budget paths -------------------------------

TEST(SessionErrors, CancellationAndDeadlineAreExplicitBeforeExecution) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    mirador::ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const OcrRequest request;
    const auto cancelled_result = session.run_ocr(image.frame, &backend, request, cancelled);
    ASSERT_FALSE(cancelled_result.ok());
    EXPECT_EQ(cancelled_result.status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(backend.call_count, 0);

    mirador::ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const auto timeout_result = session.run_ocr(image.frame, &backend, request, expired);
    ASSERT_FALSE(timeout_result.ok());
    EXPECT_EQ(timeout_result.status().code(), ErrorCode::kTimeout);
    EXPECT_EQ(backend.call_count, 0);
}

TEST(SessionErrors, ForeignSourceFramesAreRejected) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0, "other-window");

    const OcrRequest request;
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
}

TEST(SessionErrors, InvalidRequestsAreRejected) {
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    OcrRequest negative_side;
    negative_side.max_side = -1;
    EXPECT_EQ(session.run_ocr(image.frame, &backend, negative_side).status().code(), ErrorCode::kInvalidArgument);

    OcrRequest bad_output;
    bad_output.output_space = CoordinateSpaceId::kDisplay;
    EXPECT_EQ(session.run_ocr(image.frame, &backend, bad_output).status().code(), ErrorCode::kInvalidArgument);

    OcrRequest bad_roi_space;
    bad_roi_space.roi_space = CoordinateSpaceId::kModelInput;
    EXPECT_EQ(session.run_ocr(image.frame, &backend, bad_roi_space).status().code(), ErrorCode::kInvalidArgument);

    OcrRequest nan_roi;
    nan_roi.roi = RectF{0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN(), 10.0F};
    EXPECT_EQ(session.run_ocr(image.frame, &backend, nan_roi).status().code(), ErrorCode::kInvalidArgument);

    OcrRequest outside;
    outside.roi = RectF{100.0F, 100.0F, 5.0F, 5.0F};
    EXPECT_EQ(session.run_ocr(image.frame, &backend, outside).status().code(), ErrorCode::kInvalidArgument);

    EXPECT_EQ(backend.call_count, 0);
}

TEST(SessionErrors, CacheBudgetExceededSurfacesWithoutSilentDrop) {
    PerceptionSession session = make_session(96);  // fits no capability result
    FakeOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    const OcrRequest request;
    const auto result = session.run_ocr(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(session.result_cache().entry_count(), 0);
}

// --- Session-level change analysis --------------------------------------------------

TEST(SessionChangeFlow, FirstFrameGlobalThenNoneThenPartial) {
    PerceptionSession session = make_session();
    const TestFrame frame_a = make_frame(320, 240, 320 * 4, PixelFormat::kRgba8, Rotation::k0);

    const auto first = session.analyze_change(frame_a.frame);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().classification, ChangeClassification::kGlobal);
    EXPECT_EQ(first.value().reason, ChangeReason::kFirstFrame);
    EXPECT_NEAR(first.value().frame_similarity, 1.0, 1e-9);

    const auto unchanged = session.analyze_change(frame_a.frame);
    ASSERT_TRUE(unchanged.ok());
    EXPECT_EQ(unchanged.value().classification, ChangeClassification::kNone);
    EXPECT_EQ(unchanged.value().reason, ChangeReason::kFingerprintEarlyExit);

    // Localized change in the bottom-right corner.
    TestFrame frame_b = make_frame(320, 240, 320 * 4, PixelFormat::kRgba8, Rotation::k0);
    for (int32_t y = 200; y < 240; ++y) {
        std::byte* row = frame_b.bytes.data() + static_cast<int64_t>(y) * 320 * 4;
        for (int32_t x = 280; x < 320; ++x) {
            row[static_cast<int64_t>(x) * 4 + 0] = std::byte{250};
            row[static_cast<int64_t>(x) * 4 + 1] = std::byte{250};
            row[static_cast<int64_t>(x) * 4 + 2] = std::byte{250};
        }
    }
    frame_b.frame.owner = std::make_shared<const std::vector<std::byte>>(frame_b.bytes);
    const auto partial = session.analyze_change(frame_b.frame);
    ASSERT_TRUE(partial.ok());
    EXPECT_EQ(partial.value().classification, ChangeClassification::kPartial);
    ASSERT_FALSE(partial.value().changed_regions.empty());

    // Global change (rotated presentation).
    const TestFrame frame_rotated = make_frame(240, 320, 240 * 4, PixelFormat::kRgba8, Rotation::k90);
    const auto global = session.analyze_change(frame_rotated.frame);
    ASSERT_TRUE(global.ok());
    EXPECT_EQ(global.value().classification, ChangeClassification::kGlobal);
}

TEST(SessionChangeFlow, FramesFromOtherSourcesAreRejected) {
    PerceptionSession session = make_session();
    const TestFrame image = make_frame(64, 64, 64 * 4, PixelFormat::kRgba8, Rotation::k0, "elsewhere");
    EXPECT_EQ(session.analyze_change(image.frame).status().code(), ErrorCode::kInvalidArgument);
}

TEST(SessionChangeFlow, IdenticalContentAfterChangeStillHitsCapabilityCache) {
    // A -> B -> A: the A requests share a fingerprint, so the second A call
    // reuses the cached result even though the session saw B in between.
    PerceptionSession session = make_session();
    FakeOcrBackend backend;
    const TestFrame frame_a = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0);
    const TestFrame frame_b = make_frame(100, 80, 100 * 4, PixelFormat::kRgba8, Rotation::k0, kSourceId, 60);

    const OcrRequest request;
    ASSERT_TRUE(session.run_ocr(frame_a.frame, &backend, request).ok());
    ASSERT_TRUE(session.run_ocr(frame_b.frame, &backend, request).ok());
    ASSERT_TRUE(session.run_ocr(frame_a.frame, &backend, request).ok());
    EXPECT_EQ(backend.call_count, 2);
}

}  // namespace
