// NOLINTBEGIN(misc-include-cleaner): OpenCV headers arrive as SYSTEM includes;
// see the adapter translation unit for why the check is silenced file-wide.
#include <mirador/opencv_adapter.hpp>

#include <mirador/fingerprint.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <opencv2/core.hpp>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::export_mat;
using mirador::export_nv12_mat;
using mirador::PixelFormat;
using mirador::wrap_mat;
using mirador::wrap_nv12_mat;

TEST(WrapMat, MapsContinuousMatWithOpenCVChannelOrder) {
    cv::Mat mat(4, 6, CV_8UC3);
    mat = cv::Scalar(10, 20, 30);  // B, G, R per OpenCV convention.

    const auto view = wrap_mat(mat);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value().format, PixelFormat::kBgr8);
    EXPECT_EQ(view.value().width, 6);
    EXPECT_EQ(view.value().height, 4);
    EXPECT_EQ(view.value().row_stride_bytes, int64_t{6} * 3);
    EXPECT_EQ(static_cast<const void*>(view.value().data), static_cast<const void*>(mat.data));
    // Channel 0 (blue) is first in memory for every pixel.
    EXPECT_EQ(static_cast<int>(view.value().data[0]), 10);
    EXPECT_EQ(static_cast<int>(view.value().data[2]), 30);
}

TEST(WrapMat, RoiMatKeepsParentStride) {
    const cv::Mat parent(10, 10, CV_8UC1, cv::Scalar(0));
    cv::Mat roi = parent(cv::Rect(2, 1, 6, 4));
    roi.setTo(cv::Scalar(9));

    const auto view = wrap_mat(roi);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value().width, 6);
    EXPECT_EQ(view.value().height, 4);
    EXPECT_EQ(view.value().row_stride_bytes, static_cast<int64_t>(parent.step));
    EXPECT_EQ(static_cast<const void*>(view.value().data),
              static_cast<const void*>(roi.data));  // Points at the ROI origin.
    EXPECT_EQ(static_cast<int>(view.value().data[0]), 9);
}

TEST(WrapMat, MapsGrayAndBgra) {
    const cv::Mat gray(3, 3, CV_8UC1, cv::Scalar(7));
    const auto gray_view = wrap_mat(gray);
    ASSERT_TRUE(gray_view.ok());
    EXPECT_EQ(gray_view.value().format, PixelFormat::kGray8);

    const cv::Mat bgra(3, 3, CV_8UC4, cv::Scalar(1, 2, 3, 4));
    const auto bgra_view = wrap_mat(bgra);
    ASSERT_TRUE(bgra_view.ok());
    EXPECT_EQ(bgra_view.value().format, PixelFormat::kBgra8);
}

TEST(WrapMat, RejectsEmptyAndUnsupportedTypes) {
    const auto empty = wrap_mat(cv::Mat());
    ASSERT_FALSE(empty.ok());
    EXPECT_EQ(empty.status().code(), ErrorCode::kInvalidArgument);

    const cv::Mat two_channel(4, 4, CV_8UC2, cv::Scalar(0));
    const auto two = wrap_mat(two_channel);
    ASSERT_FALSE(two.ok());
    EXPECT_EQ(two.status().code(), ErrorCode::kUnsupportedFormat);

    const cv::Mat float_mat(4, 4, CV_32FC1, cv::Scalar(0.5F));
    const auto floating = wrap_mat(float_mat);
    ASSERT_FALSE(floating.ok());
    EXPECT_EQ(floating.status().code(), ErrorCode::kUnsupportedFormat);

    const cv::Mat short_mat(4, 4, CV_16UC1, cv::Scalar(0));
    const auto sixteen = wrap_mat(short_mat);
    ASSERT_FALSE(sixteen.ok());
    EXPECT_EQ(sixteen.status().code(), ErrorCode::kUnsupportedFormat);
}

/// NV12 in the conventional OpenCV shape: CV_8UC1 with height + ceil(height/2)
/// rows. `stride_pad` widens the step to exercise stride handling. The struct
/// owns the storage so the Mat stays valid for the test scope.
struct Nv12Scene {
    std::vector<uint8_t> storage;
    cv::Mat mat;
};

Nv12Scene make_nv12_scene(int32_t width, int32_t height, int32_t stride_pad, uint8_t luma, uint8_t chroma) {
    const int64_t step = static_cast<int64_t>(width) + (width % 2) + stride_pad;
    const int32_t rows = height + (height + 1) / 2;
    Nv12Scene scene;
    scene.storage.assign(static_cast<size_t>(step) * rows, luma);
    for (int32_t y = height; y < rows; ++y) {
        std::fill(scene.storage.begin() + static_cast<int64_t>(y) * step,
                  scene.storage.begin() + static_cast<int64_t>(y) * step + static_cast<int64_t>(width) + (width % 2),
                  chroma);
    }
    scene.mat = cv::Mat(rows, width, CV_8UC1, scene.storage.data(), step);
    return scene;
}

TEST(WrapNv12Mat, WrapsEvenWidthWithChromaPlane) {
    const Nv12Scene scene = make_nv12_scene(8, 4, 2, 100, 200);

    const auto view = wrap_nv12_mat(scene.mat, 8, 4);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value().format, PixelFormat::kNv12);
    EXPECT_EQ(view.value().width, 8);
    EXPECT_EQ(view.value().height, 4);
    EXPECT_EQ(view.value().row_stride_bytes, 10);
    ASSERT_NE(view.value().secondary_plane.data, nullptr);
    EXPECT_EQ(view.value().secondary_plane.data, view.value().data + int64_t{10} * 4);
    EXPECT_EQ(view.value().secondary_plane.row_stride_bytes, 10);
    EXPECT_EQ(static_cast<int>(view.value().data[0]), 100);
    EXPECT_EQ(static_cast<int>(view.value().secondary_plane.data[0]), 200);
}

TEST(WrapNv12Mat, OddWidthRequiresExtendedStep) {
    // Odd width 5: the chroma rows need width + 1 = 6 bytes; a tight step of 5
    // cannot hold them and must be rejected.
    const cv::Mat tight(3, 5, CV_8UC1, cv::Scalar(0));  // 2 luma rows + 1 chroma row
    const auto rejected = wrap_nv12_mat(tight, 5, 2);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument);

    std::vector<uint8_t> storage(static_cast<size_t>(6) * 3, 128);
    const cv::Mat padded(3, 5, CV_8UC1, storage.data(), 6);
    const auto ok = wrap_nv12_mat(padded, 5, 2);
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value().secondary_plane.row_stride_bytes, 6);
}

TEST(WrapNv12Mat, RejectsBadGeometryAndTypes) {
    std::vector<uint8_t> storage(static_cast<size_t>(8) * 5, 0);  // 5 rows: too few for 4-row luma
    const cv::Mat too_few_rows(5, 8, CV_8UC1, storage.data(), 8);
    const auto rows = wrap_nv12_mat(too_few_rows, 8, 4);  // needs 4 + 2 = 6 rows
    ASSERT_FALSE(rows.ok());
    EXPECT_EQ(rows.status().code(), ErrorCode::kInvalidArgument);

    const cv::Mat three_channel(6, 8, CV_8UC3, cv::Scalar(0));
    const auto wrong_type = wrap_nv12_mat(three_channel, 8, 4);
    ASSERT_FALSE(wrong_type.ok());
    EXPECT_EQ(wrong_type.status().code(), ErrorCode::kUnsupportedFormat);

    const auto empty = wrap_nv12_mat(cv::Mat(), 8, 4);
    ASSERT_FALSE(empty.ok());
    EXPECT_EQ(empty.status().code(), ErrorCode::kInvalidArgument);
}

TEST(ExportMat, CopiesPixelsIntoPackedBuffer) {
    const cv::Mat strided(4, 6, CV_8UC3, cv::Scalar(0));
    cv::Mat roi = strided(cv::Rect(0, 0, 6, 4));  // full size; step may still exceed minimum
    roi.setTo(cv::Scalar(11, 22, 33));

    auto exported = export_mat(roi, 4096);
    ASSERT_TRUE(exported.ok());
    mirador::ImageBuffer buffer = exported.take_value();
    EXPECT_EQ(buffer.format(), PixelFormat::kBgr8);
    EXPECT_EQ(buffer.width(), 6);
    EXPECT_EQ(buffer.height(), 4);
    EXPECT_EQ(buffer.byte_size(), int64_t{6} * 3 * 4);
    for (int64_t i = 0; i < buffer.byte_size(); i += 3) {
        EXPECT_EQ(static_cast<int>(buffer.data()[i + 0]), 11);
        EXPECT_EQ(static_cast<int>(buffer.data()[i + 1]), 22);
        EXPECT_EQ(static_cast<int>(buffer.data()[i + 2]), 33);
    }

    const auto over_budget = export_mat(roi, 8);
    ASSERT_FALSE(over_budget.ok());
    EXPECT_EQ(over_budget.status().code(), ErrorCode::kBudgetExceeded);
}

void expect_plane_filled(const std::byte* plane, const int64_t count, const uint8_t value) {
    for (int64_t i = 0; i < count; ++i) {
        EXPECT_EQ(static_cast<int>(plane[i]), static_cast<int>(value)) << "byte " << i;
    }
}

TEST(ExportNv12Mat, ReproducesDec007PlaneLayout) {
    const Nv12Scene scene = make_nv12_scene(8, 4, 2, 100, 200);

    auto exported = export_nv12_mat(scene.mat, 8, 4, 4096);
    ASSERT_TRUE(exported.ok());
    mirador::ImageBuffer buffer = exported.take_value();
    EXPECT_EQ(buffer.byte_size(), int64_t{8} * 4 + int64_t{8} * 2);
    expect_plane_filled(buffer.data(), int64_t{8} * 4, 100);  // tight luma rows
    // Tight chroma rows at the DEC-007 offset.
    expect_plane_filled(buffer.data() + int64_t{8} * 4, int64_t{8} * 2, 200);
}

TEST(OpenCVAdapterIntegration, FingerprintWorksWithWrappedViews) {
    const cv::Mat mat(32, 32, CV_8UC1, cv::Scalar(5));
    const auto view = wrap_mat(mat);
    ASSERT_TRUE(view.ok());
    const auto hash = mirador::fingerprint(view.value());
    ASSERT_TRUE(hash.ok());
    EXPECT_TRUE(view.value().rotation == mirador::Rotation::k0);
}

// NOLINTEND(misc-include-cleaner)

}  // namespace
