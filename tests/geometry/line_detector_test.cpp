// M3-02: unit tests for the LineDetector SPI contract and the first-party
// segment filter/merge utilities (design section 15).

#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/line_detector.hpp>
#include <mirador/pixel_format.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using mirador::BackendInfo;
using mirador::CollinearMergeParams;
using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::LineDetector;
using mirador::LineDetectRequest;
using mirador::LineFilterParams;
using mirador::LineSegment;
using mirador::LineSegmentSet;
using mirador::PixelFormat;
using mirador::RectI;
using mirador::Result;
using mirador::Status;

/// Fake detector injected through the SPI (DEC-012 pattern): returns a fixed
/// set and enforces the grayscale input contract the SPI documents.
class FakeLineDetector final : public LineDetector {
public:
    explicit FakeLineDetector(std::vector<LineSegment> segments) : segments_(std::move(segments)) {}

    [[nodiscard]] BackendInfo info() const override {
        BackendInfo info;
        info.name = "fake-lines";
        info.implementation_version = "1.0.0";
        info.accepted_formats = {PixelFormat::kGray8};
        return info;
    }

    Result<LineSegmentSet> detect(const ImageView& gray, const LineDetectRequest& request,
                                  const ExecutionContext& context) override {
        if (gray.format != PixelFormat::kGray8) {
            return Status(ErrorCode::kUnsupportedFormat, "fake detector requires kGray8");
        }
        if (request.min_confidence > 1.0F) {
            return Status(ErrorCode::kInvalidArgument, "invalid min_confidence");
        }
        if (mirador::is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "cancelled");
        }
        LineSegmentSet set;
        set.space = CoordinateSpaceId::kOriented;
        for (const LineSegment& segment : segments_) {
            if (segment.confidence >= request.min_confidence) {
                set.segments.push_back(segment);
            }
        }
        return set;
    }

private:
    std::vector<LineSegment> segments_;
};

/// Minimal valid grayscale view over static storage (the SPI input contract).
ImageView gray_view() {
    static std::array<std::byte, 64> storage{};
    ImageView view;
    view.data = storage.data();
    view.width = 8;
    view.height = 8;
    view.row_stride_bytes = 8;
    view.format = PixelFormat::kGray8;
    return view;
}

TEST(LineDetectorSpi, InjectsAndDispatchesThroughBasePointer) {
    FakeLineDetector fake({LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 0.9F}});
    LineDetector* spi = &fake;

    const auto info = spi->info();
    ASSERT_EQ(info.name, "fake-lines");
    ASSERT_EQ(info.accepted_formats.size(), 1U);
    ASSERT_EQ(info.accepted_formats[0], PixelFormat::kGray8);

    LineDetectRequest request;
    const auto detected = spi->detect(gray_view(), request, ExecutionContext{});
    ASSERT_TRUE(detected.ok());
    ASSERT_EQ(detected.value().space, CoordinateSpaceId::kOriented);
    ASSERT_EQ(detected.value().segments.size(), 1U);
}

TEST(LineDetectorSpi, FakeHonorsConfidenceGateAndFormatContract) {
    FakeLineDetector fake(
        {LineSegment{{0.0F, 0.0F}, {4.0F, 0.0F}, 0.4F}, LineSegment{{0.0F, 1.0F}, {4.0F, 1.0F}, 0.8F}});

    LineDetectRequest request;
    request.min_confidence = 0.5F;
    const auto detected = fake.detect(gray_view(), request, ExecutionContext{});
    ASSERT_TRUE(detected.ok());
    ASSERT_EQ(detected.value().segments.size(), 1U);
    ASSERT_FLOAT_EQ(detected.value().segments[0].confidence, 0.8F);

    ImageView color_view;
    color_view.format = PixelFormat::kRgb8;
    const auto rejected = fake.detect(color_view, LineDetectRequest{}, ExecutionContext{});
    ASSERT_FALSE(rejected.ok());
    ASSERT_EQ(rejected.status().code(), ErrorCode::kUnsupportedFormat);

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const auto stop = fake.detect(gray_view(), LineDetectRequest{}, cancelled);
    ASSERT_FALSE(stop.ok());
    ASSERT_EQ(stop.status().code(), ErrorCode::kCancelled);
}

TEST(SegmentMath, LengthAndOrientationAreDirectionIndependent) {
    const LineSegment horizontal{{1.0F, 3.0F}, {11.0F, 3.0F}, 1.0F};
    EXPECT_DOUBLE_EQ(mirador::segment_length(horizontal), 10.0);
    EXPECT_DOUBLE_EQ(mirador::segment_angle_deg(horizontal), 0.0);
    // Reversal keeps the orientation (mod 180) and the length.
    const LineSegment reversed{horizontal.end, horizontal.begin, 1.0F};
    EXPECT_DOUBLE_EQ(mirador::segment_length(reversed), 10.0);
    EXPECT_NEAR(mirador::segment_angle_deg(reversed), 0.0, 1e-12);

    const LineSegment vertical{{2.0F, 2.0F}, {2.0F, 7.0F}, 1.0F};
    EXPECT_NEAR(mirador::segment_angle_deg(vertical), 90.0, 1e-12);

    const LineSegment diagonal{{0.0F, 0.0F}, {3.0F, 3.0F}, 1.0F};
    EXPECT_NEAR(mirador::segment_length(diagonal), 4.242640687119285, 1e-9);
    EXPECT_NEAR(mirador::segment_angle_deg(diagonal), 45.0, 1e-9);
}

TEST(FilterSegments, LengthBoundsAreInclusiveAndZeroDisables) {
    const std::vector<LineSegment> segments{
        LineSegment{{0.0F, 0.0F}, {2.0F, 0.0F}, 1.0F},  // length 2
        LineSegment{{0.0F, 1.0F}, {5.0F, 1.0F}, 1.0F},  // length 5
        LineSegment{{0.0F, 2.0F}, {9.0F, 2.0F}, 1.0F},  // length 9
    };
    LineFilterParams params;
    params.min_length = 2.0;  // inclusive: the length-2 segment stays
    auto kept = mirador::filter_segments(segments, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 3U);

    params.max_length = 5.0;  // inclusive upper bound
    kept = mirador::filter_segments(segments, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 2U);
    EXPECT_DOUBLE_EQ(mirador::segment_length(kept.value()[0]), 2.0);
    EXPECT_DOUBLE_EQ(mirador::segment_length(kept.value()[1]), 5.0);
}

TEST(FilterSegments, AngleWindowSupportsWrapAround180) {
    const std::vector<LineSegment> segments{
        LineSegment{{0.0F, 0.0F}, {10.0F, 0.9F}, 1.0F},   // ~5.1 degrees
        LineSegment{{0.0F, 0.0F}, {-10.0F, 0.9F}, 1.0F},  // ~174.9 degrees
        LineSegment{{0.0F, 0.0F}, {5.0F, 5.0F}, 1.0F},    // 45 degrees
    };
    LineFilterParams params;
    params.min_angle_deg = 170.0;
    params.max_angle_deg = 10.0;  // min > max: wrap through the 180-degree seam
    auto kept = mirador::filter_segments(segments, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 2U);
    EXPECT_LT(mirador::segment_angle_deg(kept.value()[0]), 10.0);  // input order preserved
    EXPECT_GT(mirador::segment_angle_deg(kept.value()[1]), 170.0);

    params.min_angle_deg = 40.0;
    params.max_angle_deg = 50.0;
    kept = mirador::filter_segments(segments, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 1U);
    EXPECT_NEAR(mirador::segment_angle_deg(kept.value()[0]), 45.0, 1e-9);
}

TEST(FilterSegments, ConfidenceGateAndOrderStability) {
    const std::vector<LineSegment> segments{
        LineSegment{{0.0F, 0.0F}, {1.0F, 0.0F}, 0.3F},
        LineSegment{{0.0F, 1.0F}, {2.0F, 1.0F}, 0.6F},
        LineSegment{{0.0F, 2.0F}, {3.0F, 2.0F}, 0.6F},
    };
    LineFilterParams params;
    params.min_confidence = 0.6F;  // inclusive
    auto kept = mirador::filter_segments(segments, params);
    ASSERT_TRUE(kept.ok());
    ASSERT_EQ(kept.value().size(), 2U);
    EXPECT_FLOAT_EQ(kept.value()[0].begin.y, 1.0F);
    EXPECT_FLOAT_EQ(kept.value()[1].begin.y, 2.0F);
}

TEST(FilterSegments, RejectsInvalidParamsAndInputs) {
    const std::vector<LineSegment> segments{LineSegment{{0.0F, 0.0F}, {1.0F, 0.0F}, 1.0F}};

    LineFilterParams params;
    params.min_length = -1.0;
    ASSERT_EQ(mirador::filter_segments(segments, params).status().code(), ErrorCode::kInvalidArgument);

    params = LineFilterParams{};
    params.min_angle_deg = 10.0;
    params.max_angle_deg = 190.0;
    ASSERT_EQ(mirador::filter_segments(segments, params).status().code(), ErrorCode::kInvalidArgument);

    params = LineFilterParams{};
    params.min_confidence = 1.5F;
    ASSERT_EQ(mirador::filter_segments(segments, params).status().code(), ErrorCode::kInvalidArgument);

    params = LineFilterParams{};
    params.min_length = std::numeric_limits<double>::quiet_NaN();
    ASSERT_EQ(mirador::filter_segments(segments, params).status().code(), ErrorCode::kInvalidArgument);

    const std::vector<LineSegment> nan_segment{
        LineSegment{{0.0F, 0.0F}, {1.0F, 0.0F}, std::numeric_limits<float>::quiet_NaN()}};
    ASSERT_EQ(mirador::filter_segments(nan_segment, LineFilterParams{}).status().code(), ErrorCode::kInvalidArgument);
}

TEST(MergeCollinear, MergesGapWithinToleranceAndKeepsMaxConfidence) {
    const std::vector<LineSegment> segments{
        LineSegment{{0.0F, 0.0F}, {4.0F, 0.0F}, 0.5F},
        LineSegment{{6.0F, 0.0F}, {10.0F, 0.0F}, 0.8F},  // gap of 2 (default tolerance 4)
    };
    const auto merged = mirador::merge_collinear(segments, CollinearMergeParams{});
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 1U);
    EXPECT_FLOAT_EQ(merged.value()[0].begin.x, 0.0F);
    EXPECT_FLOAT_EQ(merged.value()[0].end.x, 10.0F);
    EXPECT_FLOAT_EQ(merged.value()[0].confidence, 0.8F);
}

TEST(MergeCollinear, RejectsAngleDistanceAndGapBeyondTolerance) {
    const CollinearMergeParams tight{1.0, 0.5, 0.5};

    // 3 degrees of orientation difference exceeds the 1-degree tolerance.
    const std::vector<LineSegment> angled{
        LineSegment{{0.0F, 0.0F}, {8.0F, 0.0F}, 1.0F}, LineSegment{{0.0F, 0.0F}, {8.0F, 0.42F}, 1.0F},  // ~3 degrees
    };
    auto merged = mirador::merge_collinear(angled, tight);
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 2U);

    // Parallel but offset by 1 pixel exceeds the 0.5 distance tolerance.
    const std::vector<LineSegment> offset{
        LineSegment{{0.0F, 0.0F}, {8.0F, 0.0F}, 1.0F},
        LineSegment{{0.0F, 1.0F}, {8.0F, 1.0F}, 1.0F},
    };
    merged = mirador::merge_collinear(offset, tight);
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 2U);

    // Aligned but with a gap of 2 exceeds the 0.5 gap tolerance.
    const std::vector<LineSegment> gapped{
        LineSegment{{0.0F, 0.0F}, {4.0F, 0.0F}, 1.0F},
        LineSegment{{6.0F, 0.0F}, {10.0F, 0.0F}, 1.0F},
    };
    merged = mirador::merge_collinear(gapped, tight);
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 2U);
}

TEST(MergeCollinear, TransitiveAbsorptionReachesFixpoint) {
    const std::vector<LineSegment> segments{
        LineSegment{{0.0F, 0.0F}, {3.0F, 0.0F}, 0.4F},
        LineSegment{{8.0F, 0.0F}, {9.0F, 0.0F}, 0.6F},  // only reachable via the middle piece
        LineSegment{{4.0F, 0.0F}, {7.0F, 0.0F}, 0.5F},
    };
    const auto merged = mirador::merge_collinear(segments, CollinearMergeParams{});
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 1U);
    EXPECT_FLOAT_EQ(merged.value()[0].begin.x, 0.0F);
    EXPECT_FLOAT_EQ(merged.value()[0].end.x, 9.0F);
    EXPECT_FLOAT_EQ(merged.value()[0].confidence, 0.6F);
}

TEST(MergeCollinear, DropsZeroLengthAndPreservesOrderAndInput) {
    std::vector<LineSegment> segments{
        LineSegment{{5.0F, 1.0F}, {7.0F, 1.0F}, 0.7F},
        LineSegment{{2.0F, 2.0F}, {2.0F, 2.0F}, 1.0F},  // zero length: dropped
        LineSegment{{0.0F, 0.0F}, {2.0F, 0.0F}, 0.9F},
    };
    const std::vector<LineSegment> copy = segments;
    // Tight tolerances keep the y=0 and y=1 lines apart.
    const auto merged = mirador::merge_collinear(segments, CollinearMergeParams{1.0, 0.5, 1.0});
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 2U);
    EXPECT_FLOAT_EQ(merged.value()[0].begin.x, 5.0F);  // seed order preserved
    EXPECT_FLOAT_EQ(merged.value()[1].begin.x, 0.0F);
    EXPECT_EQ(segments, copy);  // input untouched (RULE-04)
}

TEST(MergeCollinear, ExplicitErrors) {
    const std::vector<LineSegment> segments{LineSegment{{0.0F, 0.0F}, {1.0F, 0.0F}, 1.0F}};

    CollinearMergeParams params;
    params.angle_tolerance_deg = -1.0;
    ASSERT_EQ(mirador::merge_collinear(segments, params).status().code(), ErrorCode::kInvalidArgument);

    params = CollinearMergeParams{};
    params.distance_tolerance = std::numeric_limits<double>::infinity();
    ASSERT_EQ(mirador::merge_collinear(segments, params).status().code(), ErrorCode::kInvalidArgument);

    std::vector<LineSegment> too_many(mirador::kMaxMergeSegments + 1, LineSegment{{0.0F, 0.0F}, {1.0F, 0.0F}, 1.0F});
    ASSERT_EQ(mirador::merge_collinear(too_many, CollinearMergeParams{}).status().code(), ErrorCode::kBudgetExceeded);
}

}  // namespace
