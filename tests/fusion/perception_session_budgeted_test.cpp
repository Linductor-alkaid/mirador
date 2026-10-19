// M8-04 (DEC-025) tests for PerceptionSession::run_ocr_budgeted over a fake
// BudgetedOcrBackend: entry gating (cancel/deadline/source/finalize pairing),
// the DEC-012 coordinate recovery matrix (rotations, odd sizes, padded
// strides, ROI and max_side, three output spaces), priority-space conversion,
// report enforcement (consistency, byte budget), cache isolation from the
// legacy run_ocr path and report binding fields.

#include <mirador/perception_session.hpp>

#include "fake_backends.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// The budgeted SPI is deliberately independent of the legacy OcrBackend
// (DEC-025): neither derives from the other, so an OcrBackend* can never
// satisfy the budgeted entry and no implicit fallback exists.
static_assert(!std::is_base_of_v<mirador::OcrBackend, mirador::BudgetedOcrBackend>);
static_assert(!std::is_base_of_v<mirador::BudgetedOcrBackend, mirador::OcrBackend>);

using mirador::BackendInfo;
using mirador::BudgetedOcrBackend;
using mirador::BudgetedOcrRequest;
using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::Frame;
using mirador::ImageView;
using mirador::OcrBatchCompletion;
using mirador::OcrBatchRequest;
using mirador::OcrBatchResult;
using mirador::OcrBatchStopReason;
using mirador::OcrCompletionState;
using mirador::OcrLineRecord;
using mirador::OcrPriorityEntry;
using mirador::OcrSelectionStrategy;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::RectI;
using mirador::Rotation;
using mirador::Status;
using mirador::Transform2D;
using mirador::test::FakeOcrBackend;

constexpr const char* kSourceId = "screen-main";
// Coordinate recovery works in exact integer/quarter arithmetic in this
// matrix; 1e-4 px leaves room for one float rounding step.
constexpr float kCoordTol = 1e-4F;

/// Test-owned frame: padded-stride buffer plus a valid view and owner.
struct TestFrame {
    std::vector<std::byte> bytes;
    Frame frame;
};

std::byte pattern_byte(int32_t x, int32_t y, int channel, int tint) {
    return static_cast<std::byte>((x * 31 + y * 17 + channel * 5 + tint) % 251);
}

TestFrame make_frame(int32_t width, int32_t height, int32_t stride, PixelFormat format, Rotation rotation,
                     const char* source_id = kSourceId, uint64_t sequence = 42) {
    const auto stride64 = static_cast<int64_t>(stride);
    const int bpp = mirador::bytes_per_pixel(format);
    TestFrame image;
    image.bytes.assign(static_cast<size_t>(stride64) * static_cast<size_t>(height), std::byte{7});
    for (int32_t y = 0; y < height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * stride64;
        for (int32_t x = 0; x < width; ++x) {
            for (int c = 0; c < bpp; ++c) {
                row[static_cast<int64_t>(x) * bpp + c] = pattern_byte(x, y, c, 0);
            }
        }
    }
    image.frame.image.data = image.bytes.data();
    image.frame.image.width = width;
    image.frame.image.height = height;
    image.frame.image.row_stride_bytes = stride;
    image.frame.image.format = format;
    image.frame.image.rotation = rotation;
    image.frame.sequence = sequence;
    image.frame.source_id = source_id;
    image.frame.owner = std::make_shared<const std::vector<std::byte>>(image.bytes);
    return image;
}

PerceptionSession make_session() {
    PerceptionSessionOptions options;
    options.source_id = kSourceId;
    return PerceptionSession::create(std::move(options)).take_value();
}

BudgetedOcrRequest valid_request() {
    BudgetedOcrRequest request;
    request.max_rec_lines = 4;
    return request;
}

void expect_rect_near(const RectF& actual, const RectF& expected, float tolerance) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.width, expected.width, tolerance);
    EXPECT_NEAR(actual.height, expected.height, tolerance);
}

/// Central quarter of a rectangle: the central-half prepared box a synthesis
/// covers, expressed in the space the box is reported in.
RectF central_quarter_of(const RectI& rect) {
    return RectF{static_cast<float>(rect.x) + static_cast<float>(rect.width) * 0.25F,
                 static_cast<float>(rect.y) + static_cast<float>(rect.height) * 0.25F,
                 static_cast<float>(rect.width) * 0.50F, static_cast<float>(rect.height) * 0.50F};
}

OcrLineRecord recognized_line(int32_t candidate_id, const RectF& bounds, float det_confidence,
                              int32_t execution_order) {
    OcrLineRecord line;
    line.candidate_id = candidate_id;
    line.bounds = bounds;
    line.det_confidence = det_confidence;
    line.state = OcrCompletionState::kRecognized;
    line.utf8_text = "budget";
    line.rec_confidence = 0.75F;
    line.execution_order = execution_order;
    return line;
}

/// Scriptable budgeted backend: returns the scripted result verbatim when one
/// is set, the scripted status when not ok, otherwise synthesizes one
/// recognized line over either the full prepared area or its central half.
/// Scripting goes through setters, observations through read-only accessors.
struct FakeBudgetedOcrBackend final : BudgetedOcrBackend {
    FakeBudgetedOcrBackend() {
        info_value_.name = "fake-budgeted-ocr";
        info_value_.implementation_version = "1.0.0";
        info_value_.model_id = "fake-budget-model";
        info_value_.model_revision = "r1";
        info_value_.accepted_formats = {PixelFormat::kRgba8, PixelFormat::kGray8};
        info_value_.thread_safe = true;
    }

    [[nodiscard]] BackendInfo info() const override { return info_value_; }

    mirador::Result<OcrBatchResult> recognize_budgeted(const ImageView& prepared_image, const OcrBatchRequest& request,
                                                       const ExecutionContext& /*context*/) override {
        ++call_count_;
        last_prepared_width_ = prepared_image.width;
        last_prepared_height_ = prepared_image.height;
        last_prepared_stride_ = prepared_image.row_stride_bytes;
        last_format_ = prepared_image.format;
        if (prepared_image.data != nullptr && prepared_image.height > 0 && prepared_image.width > 0) {
            const int bpp = mirador::bytes_per_pixel(prepared_image.format);
            const int64_t last_offset =
                (static_cast<int64_t>(prepared_image.height) - 1) * prepared_image.row_stride_bytes +
                (static_cast<int64_t>(prepared_image.width) - 1) * bpp;
            last_first_pixel_ = prepared_image.data[0];
            last_last_pixel_ = prepared_image.data[last_offset];
        }
        last_request_ = request;
        if (!next_status_.ok()) {
            return next_status_;
        }
        if (next_result_.has_value()) {
            return *next_result_;
        }
        return synthesized_result(prepared_image.width, prepared_image.height);
    }

    // --- scripting knobs -------------------------------------------------------------

    /// When not ok, the next recognize_budgeted call returns this status.
    void set_next_status(Status status) { next_status_ = std::move(status); }

    /// When set, the next recognize_budgeted call returns this report verbatim.
    void set_next_result(OcrBatchResult result) { next_result_ = std::move(result); }

    /// Synthesize the central half of the prepared area instead of the full area.
    void set_emit_central_box(bool emit) { emit_central_box_ = emit; }

    /// Corrupts the capability query (empty name) for gating tests.
    void clear_info_name() { info_value_.name.clear(); }

    // --- read-only observations of the last call --------------------------------------

    [[nodiscard]] int call_count() const noexcept { return call_count_; }
    [[nodiscard]] int last_prepared_width() const noexcept { return last_prepared_width_; }
    [[nodiscard]] int last_prepared_height() const noexcept { return last_prepared_height_; }
    [[nodiscard]] int64_t last_prepared_stride() const noexcept { return last_prepared_stride_; }
    [[nodiscard]] PixelFormat last_format() const noexcept { return last_format_; }
    [[nodiscard]] std::byte last_first_pixel() const noexcept { return last_first_pixel_; }
    [[nodiscard]] std::byte last_last_pixel() const noexcept { return last_last_pixel_; }
    [[nodiscard]] const OcrBatchRequest& last_request() const noexcept { return last_request_; }

private:
    [[nodiscard]] OcrBatchResult synthesized_result(int width, int height) const {
        OcrBatchResult result;
        const RectF bounds = emit_central_box_
                                 ? RectF{static_cast<float>(width) * 0.25F, static_cast<float>(height) * 0.25F,
                                         static_cast<float>(width) * 0.50F, static_cast<float>(height) * 0.50F}
                                 : RectF{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
        result.lines = {recognized_line(0, bounds, 0.8F, 0)};
        result.detected_count = 1;
        result.det_limit = 0;
        result.det_limit_hit = false;
        result.recognized_count = 1;
        result.reused_count = 0;
        result.deferred_count = 0;
        result.rec_forward_count = 1;
        result.stop_reason = OcrBatchStopReason::kCompleted;
        result.completion = OcrBatchCompletion::kComplete;
        return result;
    }

    BackendInfo info_value_;
    int call_count_ = 0;
    Status next_status_;
    std::optional<OcrBatchResult> next_result_;
    bool emit_central_box_ = false;
    // Observations of the last call.
    int last_prepared_width_ = 0;
    int last_prepared_height_ = 0;
    int64_t last_prepared_stride_ = 0;
    PixelFormat last_format_ = PixelFormat::kRgba8;
    std::byte last_first_pixel_{};
    std::byte last_last_pixel_{};
    OcrBatchRequest last_request_;
};

/// Raw capture dimensions behind a view presented with `rotation`.
std::pair<int32_t, int32_t> raw_dims(Rotation rotation, int32_t view_width, int32_t view_height) {
    if (rotation == Rotation::k90 || rotation == Rotation::k270) {
        return {view_height, view_width};
    }
    return {view_width, view_height};
}

// AC8-07 rotation matrix legs. Each leg issues its own request and asserts one
// recovery path; expectations derive from the executed (clamped) pixel ROI.

/// Shared observation asserts: the fake recorded this call count and these
/// prepared dimensions for the leg's single backend call.
void check_backend_observed(const FakeBudgetedOcrBackend& backend, int expected_calls, int expected_width,
                            int expected_height) {
    EXPECT_EQ(backend.call_count(), expected_calls);
    EXPECT_EQ(backend.last_prepared_width(), expected_width);
    EXPECT_EQ(backend.last_prepared_height(), expected_height);
}

/// Leg 1, full frame in kOriented: the prepared full area maps back to the
/// executed ROI (the whole view).
void check_full_frame_orients_to_roi(const PerceptionSession& session, FakeBudgetedOcrBackend& backend,
                                     const Frame& frame, Rotation rotation, int32_t view_width, int32_t view_height) {
    const BudgetedOcrRequest full_request = valid_request();
    const auto full = session.run_ocr_budgeted(frame, &backend, full_request);
    ASSERT_TRUE(full.ok()) << "rotation " << static_cast<int>(rotation);
    check_backend_observed(backend, 1, view_width, view_height);
    const RectI full_roi{0, 0, view_width, view_height};
    EXPECT_EQ(full.value().executed_roi, full_roi);
    ASSERT_EQ(full.value().batch.lines.size(), 1U);
    expect_rect_near(full.value().batch.lines[0].bounds,
                     RectF{0.0F, 0.0F, static_cast<float>(view_width), static_cast<float>(view_height)}, kCoordTol);
    EXPECT_EQ(full.value().output_space, CoordinateSpaceId::kOriented);
    EXPECT_EQ(full.value().geometry_space, CoordinateSpaceId::kOriented);
}

/// Leg 2, ROI crop in kOriented: central-half prepared bounds recover to the
/// central quarter of the executed ROI, which is reported to the caller legs.
void check_central_box_orients_to_roi(const PerceptionSession& session, FakeBudgetedOcrBackend& backend,
                                      const Frame& frame, const RectF& oriented_roi, Rotation rotation,
                                      RectI& executed) {
    backend.set_emit_central_box(true);
    BudgetedOcrRequest roi_request = valid_request();
    roi_request.roi = oriented_roi;
    const auto central = session.run_ocr_budgeted(frame, &backend, roi_request);
    ASSERT_TRUE(central.ok()) << "rotation " << static_cast<int>(rotation);
    executed = central.value().executed_roi;
    ASSERT_EQ(central.value().batch.lines.size(), 1U);
    expect_rect_near(central.value().batch.lines[0].bounds, central_quarter_of(executed), kCoordTol);
}

/// Leg 3, same ROI crop recovered into raw frame coordinates.
void check_central_box_recovers_to_frame(const PerceptionSession& session, FakeBudgetedOcrBackend& backend,
                                         const Frame& frame, const RectF& oriented_roi, Rotation rotation,
                                         int32_t raw_width, int32_t raw_height, const RectI& executed) {
    BudgetedOcrRequest frame_request = valid_request();
    frame_request.roi = oriented_roi;
    frame_request.output_space = CoordinateSpaceId::kFrame;
    const auto in_frame = session.run_ocr_budgeted(frame, &backend, frame_request);
    ASSERT_TRUE(in_frame.ok()) << "rotation " << static_cast<int>(rotation);
    const Transform2D oriented_to_frame =
        mirador::inverse(mirador::make_rotation(rotation, raw_width, raw_height, CoordinateSpaceId::kFrame,
                                                CoordinateSpaceId::kOriented))
            .value();
    const RectF expected_frame = mirador::transform_rect(oriented_to_frame, central_quarter_of(executed));
    ASSERT_EQ(in_frame.value().batch.lines.size(), 1U);
    expect_rect_near(in_frame.value().batch.lines[0].bounds, expected_frame, kCoordTol);
    EXPECT_EQ(in_frame.value().output_space, CoordinateSpaceId::kFrame);
    EXPECT_EQ(in_frame.value().geometry_space, CoordinateSpaceId::kFrame);
}

/// Leg 4, same ROI crop recovered into crop-local coordinates.
void check_central_box_recovers_to_crop(const PerceptionSession& session, FakeBudgetedOcrBackend& backend,
                                        const Frame& frame, const RectF& oriented_roi, Rotation rotation,
                                        const RectI& executed) {
    BudgetedOcrRequest cropped_request = valid_request();
    cropped_request.roi = oriented_roi;
    cropped_request.output_space = CoordinateSpaceId::kCropped;
    const auto in_crop = session.run_ocr_budgeted(frame, &backend, cropped_request);
    ASSERT_TRUE(in_crop.ok()) << "rotation " << static_cast<int>(rotation);
    const RectF expected_cropped = central_quarter_of(RectI{0, 0, executed.width, executed.height});
    ASSERT_EQ(in_crop.value().batch.lines.size(), 1U);
    expect_rect_near(in_crop.value().batch.lines[0].bounds, expected_cropped, kCoordTol);
    EXPECT_EQ(in_crop.value().geometry_space, CoordinateSpaceId::kCropped);
}

/// Shared rotation matrix body (AC8-07): a full-frame run recovers the
/// prepared full area to the executed ROI; central-half runs recover to the
/// oriented view, raw frame and crop-local spaces of an ROI crop.
void check_rotation_recovery(Rotation rotation, int32_t view_width, int32_t view_height, const RectF& oriented_roi) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const auto [raw_width, raw_height] = raw_dims(rotation, view_width, view_height);
    const TestFrame image = make_frame(view_width, view_height, view_width * 4 + 13, PixelFormat::kRgba8, rotation);
    const Frame& frame = image.frame;

    check_full_frame_orients_to_roi(session, backend, frame, rotation, view_width, view_height);
    RectI executed;
    check_central_box_orients_to_roi(session, backend, frame, oriented_roi, rotation, executed);
    check_central_box_recovers_to_frame(session, backend, frame, oriented_roi, rotation, raw_width, raw_height,
                                        executed);
    check_central_box_recovers_to_crop(session, backend, frame, oriented_roi, rotation, executed);
}

// --- Entry gating -------------------------------------------------------------------

TEST(BudgetedEntryGating, NullBackendReportsUnavailable) {
    PerceptionSession session = make_session();
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);
    const auto result = session.run_ocr_budgeted(image.frame, nullptr, valid_request());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBackendUnavailable);
    EXPECT_EQ(session.result_cache().entry_count(), 0U);
}

TEST(BudgetedEntryGating, CancelledContextFailsAtEntryWithZeroBackendCalls) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    ExecutionContext context;
    context.is_cancelled = [] { return true; };
    const auto result = session.run_ocr_budgeted(image.frame, &backend, valid_request(), context);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, ExpiredDeadlineFailsAtEntryWithZeroBackendCalls) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const auto result = session.run_ocr_budgeted(image.frame, &backend, valid_request(), context);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kTimeout);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, CancellationBeatsSimultaneouslyExpiredDeadline) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    ExecutionContext context;
    context.is_cancelled = [] { return true; };
    context.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const auto result = session.run_ocr_budgeted(image.frame, &backend, valid_request(), context);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, ForeignSourceFrameIsRejected) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0, "other-window");
    const auto result = session.run_ocr_budgeted(image.frame, &backend, valid_request());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, FinalizeReserveRequiresAContextDeadline) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest unpaired = valid_request();
    unpaired.finalize_reserve = std::chrono::milliseconds{500};
    const auto without_deadline = session.run_ocr_budgeted(image.frame, &backend, unpaired);
    ASSERT_FALSE(without_deadline.ok());
    EXPECT_EQ(without_deadline.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(backend.call_count(), 0);

    // With a hard deadline present the pairing is valid and the run executes.
    ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
    const auto with_deadline = session.run_ocr_budgeted(image.frame, &backend, unpaired, context);
    ASSERT_TRUE(with_deadline.ok());
    EXPECT_EQ(backend.call_count(), 1);
}

TEST(BudgetedEntryGating, InvalidPrioritySpaceIsRejected) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.priority_space = CoordinateSpaceId::kDisplay;
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, NonPositiveRecLineBudgetIsRejectedBeforeTheBackend) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest zero = valid_request();
    zero.max_rec_lines = 0;
    const auto zero_result = session.run_ocr_budgeted(image.frame, &backend, zero);
    ASSERT_FALSE(zero_result.ok());
    EXPECT_EQ(zero_result.status().code(), ErrorCode::kInvalidArgument);

    BudgetedOcrRequest negative = valid_request();
    negative.max_rec_lines = -3;
    const auto negative_result = session.run_ocr_budgeted(image.frame, &backend, negative);
    ASSERT_FALSE(negative_result.ok());
    EXPECT_EQ(negative_result.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, NegativeSoftBudgetIsRejectedBeforeTheBackend) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.soft_budget = std::chrono::milliseconds{-1};
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, TooManyPrioritiesIsRejectedBeforeTheBackend) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.priorities.assign(65U, OcrPriorityEntry{});
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(backend.call_count(), 0);
}

TEST(BudgetedEntryGating, InvalidBackendInfoReportsUnavailable) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    backend.clear_info_name();
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);
    const auto result = session.run_ocr_budgeted(image.frame, &backend, valid_request());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBackendUnavailable);
    EXPECT_EQ(backend.call_count(), 0);
}

// --- Report enforcement ---------------------------------------------------------------

TEST(BudgetedReportEnforcement, InconsistentBackendReportFailsTheSession) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    // Count conservation violation: two detected lines claimed, one reported.
    OcrBatchResult inconsistent;
    inconsistent.lines = {recognized_line(0, RectF{0.0F, 0.0F, 10.0F, 5.0F}, 0.8F, 0)};
    inconsistent.detected_count = 2;
    inconsistent.recognized_count = 1;
    inconsistent.rec_forward_count = 1;
    inconsistent.stop_reason = OcrBatchStopReason::kCompleted;
    inconsistent.completion = OcrBatchCompletion::kComplete;
    backend.set_next_result(inconsistent);

    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);
    const auto result = session.run_ocr_budgeted(image.frame, &backend, valid_request());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBackendFailure);
    EXPECT_EQ(backend.call_count(), 1);  // executed, then rejected at the boundary
}

TEST(BudgetedReportEnforcement, ReportOverMaxReportBytesFailsWithBudgetExceeded) {
    PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.max_report_bytes = 1;  // any report owns more than one byte
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(backend.call_count(), 1);
    EXPECT_EQ(session.result_cache().entry_count(), 0U);
}

TEST(BudgetedReportEnforcement, BackendFailurePropagates) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    backend.set_next_status(Status(ErrorCode::kBackendFailure, "fake budgeted inference failed"));
    const TestFrame image = make_frame(40, 30, 40 * 4, PixelFormat::kRgba8, Rotation::k0);
    const auto result = session.run_ocr_budgeted(image.frame, &backend, valid_request());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBackendFailure);
    EXPECT_EQ(backend.call_count(), 1);
}

// --- Coordinate recovery matrix (AC8-07) -----------------------------------------------

TEST(BudgetedCoordinateRecovery, Rotation0RecoversToOrientedFrameAndCrop) {
    check_rotation_recovery(Rotation::k0, 64, 48, RectF{10.0F, 8.0F, 30.0F, 24.0F});
}

TEST(BudgetedCoordinateRecovery, Rotation90RecoversToOrientedFrameAndCrop) {
    // Raw 100x80 capture presented as an 80x100 view.
    check_rotation_recovery(Rotation::k90, 80, 100, RectF{10.0F, 20.0F, 30.0F, 40.0F});
}

TEST(BudgetedCoordinateRecovery, Rotation180RecoversToOrientedFrameAndCrop) {
    check_rotation_recovery(Rotation::k180, 100, 80, RectF{10.0F, 20.0F, 30.0F, 40.0F});
}

TEST(BudgetedCoordinateRecovery, Rotation270RecoversToOrientedFrameAndCrop) {
    // Raw 100x80 capture presented as an 80x100 view.
    check_rotation_recovery(Rotation::k270, 80, 100, RectF{10.0F, 20.0F, 30.0F, 40.0F});
}

TEST(BudgetedCoordinateRecovery, OddSizeWithPaddedStrideKeepsGeometryAndPixelsExact) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    // 33x17 RGBA view, row stride padded to 145 bytes (>= 132 minimum).
    const TestFrame image = make_frame(33, 17, 145, PixelFormat::kRgba8, Rotation::k0);

    const auto full = session.run_ocr_budgeted(image.frame, &backend, valid_request());
    ASSERT_TRUE(full.ok());
    EXPECT_EQ(backend.last_prepared_width(), 33);
    EXPECT_EQ(backend.last_prepared_height(), 17);
    EXPECT_EQ(backend.last_format(), PixelFormat::kRgba8);
    // Distinguishable pixels prove the crop read real pixels, not padding.
    EXPECT_EQ(backend.last_first_pixel(), pattern_byte(0, 0, 0, 0));
    EXPECT_EQ(backend.last_last_pixel(), pattern_byte(32, 16, 0, 0));
    ASSERT_EQ(full.value().batch.lines.size(), 1U);
    expect_rect_near(full.value().batch.lines[0].bounds, RectF{0.0F, 0.0F, 33.0F, 17.0F}, kCoordTol);

    backend.set_emit_central_box(true);
    BudgetedOcrRequest roi_request = valid_request();
    roi_request.roi = RectF{5.0F, 4.0F, 21.0F, 9.0F};
    const auto central = session.run_ocr_budgeted(image.frame, &backend, roi_request);
    ASSERT_TRUE(central.ok());
    EXPECT_EQ(backend.last_prepared_width(), 21);
    EXPECT_EQ(backend.last_prepared_height(), 9);
    ASSERT_EQ(central.value().batch.lines.size(), 1U);
    expect_rect_near(central.value().batch.lines[0].bounds, RectF{10.25F, 6.25F, 10.5F, 4.5F}, kCoordTol);

    BudgetedOcrRequest cropped_request = roi_request;
    cropped_request.output_space = CoordinateSpaceId::kCropped;
    const auto in_crop = session.run_ocr_budgeted(image.frame, &backend, cropped_request);
    ASSERT_TRUE(in_crop.ok());
    ASSERT_EQ(in_crop.value().batch.lines.size(), 1U);
    expect_rect_near(in_crop.value().batch.lines[0].bounds, RectF{5.25F, 2.25F, 10.5F, 4.5F}, kCoordTol);
}

TEST(BudgetedCoordinateRecovery, FractionalRoiClampsAndRecoversExactly) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.roi = RectF{5.5F, 3.25F, 20.5F, 11.75F};
    backend.set_emit_central_box(true);
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    // The executed ROI is the enclosing integer rect clamped to the view.
    const RectI clamped{5, 3, 21, 12};
    EXPECT_EQ(result.value().executed_roi, clamped);
    ASSERT_EQ(result.value().batch.lines.size(), 1U);
    // Recovery tracks the executed integer crop, so expectations are exact.
    expect_rect_near(result.value().batch.lines[0].bounds, RectF{10.25F, 6.0F, 10.5F, 6.0F}, kCoordTol);
}

TEST(BudgetedCoordinateRecovery, MaxSideScalingRecoversExactly) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    // Full frame 64x48 downscaled to 16x12; the prepared full area still maps
    // back to the whole frame and the central half to the central quarter.
    BudgetedOcrRequest scaled = valid_request();
    scaled.max_side = 16;
    const auto full = session.run_ocr_budgeted(image.frame, &backend, scaled);
    ASSERT_TRUE(full.ok());
    EXPECT_EQ(backend.last_prepared_width(), 16);
    EXPECT_EQ(backend.last_prepared_height(), 12);
    ASSERT_EQ(full.value().batch.lines.size(), 1U);
    expect_rect_near(full.value().batch.lines[0].bounds, RectF{0.0F, 0.0F, 64.0F, 48.0F}, kCoordTol);

    // ROI 30x40 downscaled to 12x16 (long edge 40 -> 16).
    BudgetedOcrRequest roi_scaled = scaled;
    roi_scaled.roi = RectF{10.0F, 8.0F, 30.0F, 40.0F};
    backend.set_emit_central_box(true);
    const auto central = session.run_ocr_budgeted(image.frame, &backend, roi_scaled);
    ASSERT_TRUE(central.ok());
    EXPECT_EQ(backend.last_prepared_width(), 12);
    EXPECT_EQ(backend.last_prepared_height(), 16);
    ASSERT_EQ(central.value().batch.lines.size(), 1U);
    expect_rect_near(central.value().batch.lines[0].bounds, RectF{17.5F, 18.0F, 15.0F, 20.0F}, kCoordTol);
}

TEST(BudgetedCoordinateRecovery, FrameSpaceRoiExecutesTheRotatedCrop) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    // Raw 100x80 capture, presented 80x100 (k90).
    const TestFrame image = make_frame(80, 100, 80 * 4, PixelFormat::kRgba8, Rotation::k90);

    BudgetedOcrRequest request = valid_request();
    request.roi_space = CoordinateSpaceId::kFrame;
    request.roi = RectF{0.0F, 0.0F, 100.0F, 80.0F};  // the whole raw capture
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    // The executed ROI covers the whole oriented view.
    const RectI whole_view{0, 0, 80, 100};
    EXPECT_EQ(result.value().executed_roi, whole_view);
    EXPECT_EQ(backend.last_prepared_width(), 80);
    EXPECT_EQ(backend.last_prepared_height(), 100);
    ASSERT_EQ(result.value().batch.lines.size(), 1U);
    expect_rect_near(result.value().batch.lines[0].bounds, RectF{0.0F, 0.0F, 80.0F, 100.0F}, kCoordTol);
}

// --- Priority space conversion ---------------------------------------------------------

TEST(BudgetedPrioritySpaces, OrientedPrioritiesAreConvertedIntoCropSpace) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.roi = RectF{10.0F, 4.0F, 30.0F, 40.0F};
    request.priority_space = CoordinateSpaceId::kOriented;
    request.priorities = {OcrPriorityEntry{RectF{12.0F, 6.0F, 5.0F, 6.0F}, 0.7F}};
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(backend.last_request().priorities.size(), 1U);
    // Prepared space is the crop shifted by the ROI origin (no max_side here).
    expect_rect_near(backend.last_request().priorities[0].roi, RectF{2.0F, 2.0F, 5.0F, 6.0F}, kCoordTol);
    EXPECT_FLOAT_EQ(backend.last_request().priorities[0].priority, 0.7F);
}

TEST(BudgetedPrioritySpaces, FramePrioritiesAreConvertedThroughRotationAndCrop) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    // Raw 100x80 capture, presented 80x100 (k90).
    const TestFrame image = make_frame(80, 100, 80 * 4, PixelFormat::kRgba8, Rotation::k90);

    BudgetedOcrRequest request = valid_request();
    request.roi = RectF{10.0F, 20.0F, 30.0F, 40.0F};
    request.priority_space = CoordinateSpaceId::kFrame;
    request.priorities = {OcrPriorityEntry{RectF{0.0F, 0.0F, 100.0F, 80.0F}, 0.5F}};  // whole raw frame
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());

    // Expected chain: raw frame -> oriented (k90) -> crop, computed through
    // the public transform factories.
    const Transform2D frame_to_oriented =
        mirador::make_rotation(Rotation::k90, 100, 80, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
    const Transform2D oriented_to_prepared = mirador::make_crop(
        RectF{10.0F, 20.0F, 30.0F, 40.0F}, CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
    const Transform2D frame_to_prepared = mirador::compose(frame_to_oriented, oriented_to_prepared).value();
    const RectF expected = mirador::transform_rect(frame_to_prepared, RectF{0.0F, 0.0F, 100.0F, 80.0F});
    ASSERT_EQ(backend.last_request().priorities.size(), 1U);
    expect_rect_near(backend.last_request().priorities[0].roi, expected, kCoordTol);
}

TEST(BudgetedPrioritySpaces, PrioritiesFollowTheMaxSideScaleIntoPreparedSpace) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.roi = RectF{10.0F, 8.0F, 30.0F, 40.0F};  // prepared 12x16 at max_side 16
    request.max_side = 16;
    request.priority_space = CoordinateSpaceId::kOriented;
    request.priorities = {OcrPriorityEntry{RectF{12.0F, 10.0F, 5.0F, 6.0F}, 0.7F}};
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(backend.last_prepared_width(), 12);
    EXPECT_EQ(backend.last_prepared_height(), 16);

    const Transform2D oriented_to_prepared =
        mirador::compose(
            mirador::make_crop(RectF{10.0F, 8.0F, 30.0F, 40.0F}, CoordinateSpaceId::kOriented,
                               CoordinateSpaceId::kCropped),
            mirador::make_scale(12.0 / 30.0, 16.0 / 40.0, CoordinateSpaceId::kCropped, CoordinateSpaceId::kModelInput))
            .value();
    const RectF expected = mirador::transform_rect(oriented_to_prepared, RectF{12.0F, 10.0F, 5.0F, 6.0F});
    ASSERT_EQ(backend.last_request().priorities.size(), 1U);
    expect_rect_near(backend.last_request().priorities[0].roi, expected, kCoordTol);
}

TEST(BudgetedPrioritySpaces, ReportDigestMatchesTheEffectiveBackendRequest) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.roi = RectF{10.0F, 4.0F, 30.0F, 40.0F};
    request.priorities = {OcrPriorityEntry{RectF{12.0F, 6.0F, 5.0F, 6.0F}, 0.7F}};
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().request_digest, mirador::ocr_batch_request_params_digest(backend.last_request()));
}

// --- Backend field pass-through ---------------------------------------------------------

TEST(BudgetedPassThrough, BackendFieldsAndBudgetKnobsAreForwarded) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    BudgetedOcrRequest request = valid_request();
    request.language_hint = "zh-Hans";
    request.min_confidence = 0.35F;
    request.backend_params = "{\"beam\":4}";
    request.max_rec_lines = 7;
    request.selection = OcrSelectionStrategy::kDetScore;
    request.soft_budget = std::chrono::milliseconds{200};
    request.finalize_reserve = std::chrono::milliseconds{300};
    request.max_priority_entries = 32;
    request.max_priority_bytes = int64_t{32} * 1024;
    request.max_report_bytes = int64_t{1024} * 1024;

    ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);  // finalize pairing
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request, context);
    ASSERT_TRUE(result.ok());

    const OcrBatchRequest& seen = backend.last_request();
    EXPECT_EQ(seen.language_hint, "zh-Hans");
    EXPECT_FLOAT_EQ(seen.min_confidence, 0.35F);
    EXPECT_EQ(seen.backend_params, "{\"beam\":4}");
    EXPECT_EQ(seen.max_rec_lines, 7);
    EXPECT_EQ(seen.selection, OcrSelectionStrategy::kDetScore);
    // Optional fields compared whole: the exact forwarded duration or absence
    // is a mismatch either way (no unchecked dereference needed).
    using steady_duration = std::chrono::steady_clock::duration;
    EXPECT_EQ(seen.soft_budget, std::optional<steady_duration>(std::chrono::milliseconds{200}));
    EXPECT_EQ(seen.finalize_reserve, std::optional<steady_duration>(std::chrono::milliseconds{300}));
    EXPECT_EQ(seen.max_priority_entries, 32);
    EXPECT_EQ(seen.max_priority_bytes, 32 * 1024);
    EXPECT_EQ(seen.max_report_bytes, 1024 * 1024);
}

// --- Cache isolation (DEC-025 section 5, AC8-08) -----------------------------------------

TEST(BudgetedCacheIsolation, BudgetedEntryNeverTouchesTheResultCache) {
    PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    EXPECT_EQ(session.result_cache().entry_count(), 0U);
    EXPECT_EQ(session.result_cache().byte_size(), 0);

    const auto first = session.run_ocr_budgeted(image.frame, &backend, valid_request());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(backend.call_count(), 1);
    EXPECT_EQ(session.result_cache().entry_count(), 0U);
    EXPECT_EQ(session.result_cache().byte_size(), 0);

    // A repeated identical budgeted call re-executes: nothing was stored.
    const auto second = session.run_ocr_budgeted(image.frame, &backend, valid_request());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(backend.call_count(), 2);
    EXPECT_EQ(session.result_cache().entry_count(), 0U);
    EXPECT_EQ(session.result_cache().byte_size(), 0);
}

TEST(BudgetedCacheIsolation, LegacyRunOcrCacheIsUnaffectedByBudgetedCalls) {
    PerceptionSession session = make_session();
    FakeBudgetedOcrBackend budgeted;
    FakeOcrBackend legacy;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0);

    // Seed one cache entry through the legacy path.
    const mirador::OcrRequest legacy_request;
    const auto seeded = session.run_ocr(image.frame, &legacy, legacy_request);
    ASSERT_TRUE(seeded.ok());
    ASSERT_EQ(session.result_cache().entry_count(), 1U);
    const auto cached_bytes = session.result_cache().byte_size();

    // Budgeted calls neither read nor write that cache.
    const auto budgeted_first = session.run_ocr_budgeted(image.frame, &budgeted, valid_request());
    ASSERT_TRUE(budgeted_first.ok());
    EXPECT_EQ(budgeted.call_count(), 1);
    const auto budgeted_second = session.run_ocr_budgeted(image.frame, &budgeted, valid_request());
    ASSERT_TRUE(budgeted_second.ok());
    EXPECT_EQ(budgeted.call_count(), 2);  // re-executed, not served from the cache
    EXPECT_EQ(session.result_cache().entry_count(), 1U);
    EXPECT_EQ(session.result_cache().byte_size(), cached_bytes);

    // The legacy entry is still a hit after the budgeted interludes.
    const auto again = session.run_ocr(image.frame, &legacy, legacy_request);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(legacy.call_count, 1);
    EXPECT_EQ(session.result_cache().entry_count(), 1U);
}

// --- Report binding -----------------------------------------------------------------------

TEST(BudgetedReportBinding, ReportBindsSourceFrameRoiAndBackendIdentity) {
    const PerceptionSession session = make_session();
    FakeBudgetedOcrBackend backend;
    const TestFrame image = make_frame(64, 48, 64 * 4, PixelFormat::kRgba8, Rotation::k0, kSourceId, 42);

    BudgetedOcrRequest request = valid_request();
    request.roi = RectF{10.0F, 8.0F, 30.0F, 24.0F};
    request.output_space = CoordinateSpaceId::kOriented;
    const auto result = session.run_ocr_budgeted(image.frame, &backend, request);
    ASSERT_TRUE(result.ok());

    EXPECT_EQ(result.value().source_id, kSourceId);
    EXPECT_EQ(result.value().frame_sequence, 42U);
    const RectI executed_expected{10, 8, 30, 24};
    EXPECT_EQ(result.value().executed_roi, executed_expected);
    EXPECT_EQ(result.value().backend_info.name, "fake-budgeted-ocr");
    EXPECT_EQ(result.value().backend_info.model_revision, "r1");
    EXPECT_EQ(result.value().output_space, CoordinateSpaceId::kOriented);
    EXPECT_EQ(result.value().geometry_space, request.output_space);
}

}  // namespace
