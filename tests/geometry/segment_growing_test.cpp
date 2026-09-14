// M3-03: unit tests for the first-party segment-growing line detector
// (design section 15, DEC-009 first-party equivalent implementation).

#include <mirador/execution_context.hpp>
#include <mirador/line_detector.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/segment_growing_line_detector.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using mirador::BackendInfo;
using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::LineDetectRequest;
using mirador::LineSegment;
using mirador::LineSegmentSet;
using mirador::PixelFormat;
using mirador::RectI;
using mirador::SegmentGrowingLineDetector;
using mirador::SegmentGrowingParams;

/// Paints a filled axis-aligned band, then wraps it in a valid gray view.
struct GrayImage {
    std::vector<uint8_t> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int64_t stride = 0;

    [[nodiscard]] uint8_t& at(int32_t x, int32_t y) {
        return pixels[static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)];
    }

    [[nodiscard]] ImageView view() const {
        ImageView v;
        v.data = reinterpret_cast<const std::byte*>(pixels.data());
        v.width = width;
        v.height = height;
        v.row_stride_bytes = stride;
        v.format = PixelFormat::kGray8;
        return v;
    }
};

[[nodiscard]] GrayImage make_image(int32_t width, int32_t height, int64_t stride, uint8_t fill) {
    GrayImage image;
    image.width = width;
    image.height = height;
    image.stride = stride;
    image.pixels.assign(static_cast<size_t>(height) * static_cast<size_t>(stride), fill);
    return image;
}

void draw_horizontal_line(GrayImage& image, int32_t y, int32_t x_begin, int32_t x_end, uint8_t value) {
    for (int32_t x = x_begin; x < x_end; ++x) {
        image.at(x, y) = value;
    }
}

void draw_vertical_line(GrayImage& image, int32_t x, int32_t y_begin, int32_t y_end, uint8_t value) {
    for (int32_t y = y_begin; y < y_end; ++y) {
        image.at(x, y) = value;
    }
}

TEST(SegmentGrowing, InfoIsValidAndModelFree) {
    const SegmentGrowingLineDetector detector;
    const BackendInfo info = detector.info();
    const auto validated = mirador::validate(info);
    ASSERT_TRUE(validated.ok()) << validated.status().message();
    EXPECT_EQ(info.name, "mirador-segment-growing");
    ASSERT_EQ(info.accepted_formats.size(), 1U);
    EXPECT_EQ(info.accepted_formats[0], PixelFormat::kGray8);
    EXPECT_TRUE(info.thread_safe);
}

TEST(SegmentGrowing, ThinHorizontalLineYieldsTwoRidges) {
    GrayImage image = make_image(64, 48, 64, 0);
    draw_horizontal_line(image, 20, 5, 58, 220);

    SegmentGrowingLineDetector detector;
    const auto detected = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(detected.ok()) << detected.status().message();
    ASSERT_EQ(detected.value().space, CoordinateSpaceId::kOriented);
    ASSERT_EQ(detected.value().segments.size(), 2U);  // one ridge per side of the line
    for (const LineSegment& segment : detected.value().segments) {
        EXPECT_DOUBLE_EQ(mirador::segment_angle_deg(segment), 0.0);
        EXPECT_GE(segment.begin.y, 17.0F);
        EXPECT_LE(segment.begin.y, 23.0F);
        EXPECT_GE(mirador::segment_length(segment), 45.0);
        EXPECT_LE(mirador::segment_length(segment), 56.0);
        EXPECT_GT(segment.confidence, 0.5F);
    }
}

TEST(SegmentGrowing, ThinVerticalLineYieldsTwoRidges) {
    GrayImage image = make_image(48, 64, 48, 0);
    draw_vertical_line(image, 30, 10, 50, 220);

    SegmentGrowingLineDetector detector;
    const auto detected = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(detected.ok()) << detected.status().message();
    ASSERT_EQ(detected.value().segments.size(), 2U);
    for (const LineSegment& segment : detected.value().segments) {
        EXPECT_NEAR(mirador::segment_angle_deg(segment), 90.0, 1e-6);
        EXPECT_GE(mirador::segment_length(segment), 30.0);
    }
}

TEST(SegmentGrowing, DiagonalLineIsDetected) {
    GrayImage image = make_image(64, 64, 64, 0);
    for (int32_t i = 8; i < 56; ++i) {
        image.at(i, i) = 220;
    }

    SegmentGrowingLineDetector detector;
    const auto detected = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(detected.ok()) << detected.status().message();
    ASSERT_FALSE(detected.value().segments.empty());
    for (const LineSegment& segment : detected.value().segments) {
        const double angle = mirador::segment_angle_deg(segment);
        EXPECT_TRUE(std::fabs(angle - 135.0) < 5.0 || std::fabs(angle - 45.0) < 5.0) << angle;
        EXPECT_GE(mirador::segment_length(segment), 30.0);
    }
}

TEST(SegmentGrowing, UniformImageYieldsNoSegments) {
    GrayImage image = make_image(64, 64, 64, 128);
    SegmentGrowingLineDetector detector;
    const auto detected = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(detected.ok());
    EXPECT_TRUE(detected.value().segments.empty());
}

TEST(SegmentGrowing, RoiRestrictsScanAndKeepsAbsoluteCoordinates) {
    GrayImage image = make_image(64, 64, 64, 0);
    draw_horizontal_line(image, 20, 5, 58, 220);
    draw_horizontal_line(image, 40, 5, 58, 220);

    SegmentGrowingLineDetector detector;
    LineDetectRequest request;
    request.roi = RectI{20, 10, 20, 24};  // columns 20..39, rows 10..33: only the row-20 line
    const auto detected = detector.detect(image.view(), request, {});
    ASSERT_TRUE(detected.ok()) << detected.status().message();
    ASSERT_EQ(detected.value().segments.size(), 2U);  // only the row-20 line, not row 40
    for (const LineSegment& segment : detected.value().segments) {
        EXPECT_GE(segment.begin.x, 20.0F);  // absolute view coordinates, not ROI-local
        EXPECT_LE(segment.end.x, 40.0F);
        EXPECT_GE(mirador::segment_length(segment), 10.0);
    }
}

TEST(SegmentGrowing, MinConfidenceGatesOutput) {
    GrayImage image = make_image(64, 48, 64, 0);
    draw_horizontal_line(image, 20, 5, 58, 220);
    // A small blob above the line adds slightly offset edge pixels to the
    // bottom ridge, so its fit keeps a nonzero residual and its confidence
    // drops below 1; the top ridge stays pixel-perfect.
    image.at(30, 19) = 220;
    image.at(31, 19) = 220;

    SegmentGrowingLineDetector detector;
    const auto ungated = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(ungated.ok());
    ASSERT_GE(ungated.value().segments.size(), 2U);

    LineDetectRequest request;
    request.min_confidence = 0.99F;
    const auto gated = detector.detect(image.view(), request, {});
    ASSERT_TRUE(gated.ok());
    ASSERT_FALSE(gated.value().segments.empty());
    ASSERT_LT(gated.value().segments.size(), ungated.value().segments.size());
    for (const LineSegment& segment : gated.value().segments) {
        EXPECT_GE(segment.confidence, 0.99F);
        EXPECT_LT(segment.begin.y, 20.5F);  // the perfect top ridge survives
    }
}

TEST(SegmentGrowing, StridePaddingDoesNotChangeResults) {
    GrayImage tight = make_image(48, 40, 48, 0);
    draw_horizontal_line(tight, 20, 4, 44, 200);
    GrayImage padded = make_image(48, 40, 56, 0);
    for (int32_t y = 0; y < 40; ++y) {
        for (int32_t x = 0; x < 48; ++x) {
            padded.at(x, y) = tight.at(x, y);
        }
    }

    SegmentGrowingLineDetector detector;
    const auto first = detector.detect(tight.view(), LineDetectRequest{}, {});
    const auto second = detector.detect(padded.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().segments, second.value().segments);
}

TEST(SegmentGrowing, OddSizedImageWorks) {
    GrayImage image = make_image(33, 17, 33, 0);
    draw_horizontal_line(image, 8, 2, 30, 220);

    SegmentGrowingLineDetector detector;
    const auto detected = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(detected.ok()) << detected.status().message();
    EXPECT_EQ(detected.value().segments.size(), 2U);
}

TEST(SegmentGrowing, RepeatedRunsAreBitIdentical) {
    GrayImage image = make_image(64, 64, 64, 0);
    draw_horizontal_line(image, 20, 5, 58, 220);
    for (int32_t i = 8; i < 56; ++i) {
        image.at(i, i) = 200;
    }

    SegmentGrowingLineDetector detector;
    const auto first = detector.detect(image.view(), LineDetectRequest{}, {});
    const auto second = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().segments, second.value().segments);
}

TEST(SegmentGrowing, ExplicitErrors) {
    SegmentGrowingLineDetector detector;
    GrayImage image = make_image(64, 64, 64, 0);
    draw_horizontal_line(image, 20, 5, 58, 220);

    // Wrong format (a structurally valid RGB view).
    std::vector<uint8_t> rgb_pixels(8 * 24, 0);
    ImageView color;
    color.data = reinterpret_cast<const std::byte*>(rgb_pixels.data());
    color.width = 8;
    color.height = 8;
    color.row_stride_bytes = 24;
    color.format = PixelFormat::kRgb8;
    ASSERT_EQ(detector.detect(color, LineDetectRequest{}, {}).status().code(), ErrorCode::kUnsupportedFormat);

    // Invalid view (null data).
    ImageView invalid;
    ASSERT_EQ(detector.detect(invalid, LineDetectRequest{}, {}).status().code(), ErrorCode::kInvalidArgument);

    // ROI outside the view.
    LineDetectRequest bad_roi;
    bad_roi.roi = RectI{60, 0, 10, 10};
    ASSERT_EQ(detector.detect(image.view(), bad_roi, {}).status().code(), ErrorCode::kInvalidArgument);

    // min_confidence out of range.
    LineDetectRequest bad_confidence;
    bad_confidence.min_confidence = 1.5F;
    ASSERT_EQ(detector.detect(image.view(), bad_confidence, {}).status().code(), ErrorCode::kInvalidArgument);

    // Out-of-range construction parameters.
    SegmentGrowingParams bad_params;
    bad_params.gradient_threshold = 0;
    SegmentGrowingLineDetector bad_detector(bad_params);
    ASSERT_EQ(bad_detector.detect(image.view(), LineDetectRequest{}, {}).status().code(), ErrorCode::kInvalidArgument);

    // Cancellation and deadline.
    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    ASSERT_EQ(detector.detect(image.view(), LineDetectRequest{}, cancelled).status().code(), ErrorCode::kCancelled);

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    ASSERT_EQ(detector.detect(image.view(), LineDetectRequest{}, expired).status().code(), ErrorCode::kTimeout);
}

TEST(SegmentGrowing, SegmentCapFailsExplicitly) {
    GrayImage image = make_image(64, 64, 64, 0);
    for (int32_t row = 4; row < 64; row += 7) {
        draw_horizontal_line(image, row, 2, 62, 200);
    }

    SegmentGrowingParams params;
    params.max_segments = 4;  // eight lines produce sixteen ridges
    SegmentGrowingLineDetector detector(params);
    const auto detected = detector.detect(image.view(), LineDetectRequest{}, {});
    ASSERT_FALSE(detected.ok());
    ASSERT_EQ(detected.status().code(), ErrorCode::kBudgetExceeded);
}

}  // namespace
