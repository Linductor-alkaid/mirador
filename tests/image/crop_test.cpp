#include <mirador/crop.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::compose;
using mirador::contains;
using mirador::CoordinateSpaceId;
using mirador::crop;
using mirador::ErrorCode;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::inverse;
using mirador::make_crop;
using mirador::make_rotation;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::RectI;
using mirador::Rotation;
using mirador::transform_rect;
using mirador::validate;

constexpr int64_t kBudget = 1 << 20;

/// 5x3 RGB (minimum stride 15) on a loose stride of 23; pixel (x, y) carries the
/// value y * 5 + x in all three channels.
ImageView make_loose_rgb_view(std::vector<std::byte>& storage) {
    storage.assign(69, std::byte{0});  // 3 loose rows of 23 bytes
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 5; ++x) {
            for (int32_t c = 0; c < 3; ++c) {
                storage[static_cast<size_t>(y) * 23 + static_cast<size_t>(x) * 3 + c] =
                    static_cast<std::byte>(y * 5 + x);
            }
        }
    }
    ImageView src;
    src.data = storage.data();
    src.width = 5;
    src.height = 3;
    src.row_stride_bytes = 23;
    src.format = PixelFormat::kRgb8;
    return src;
}

void verify_cropped_rgb_row(const ImageView& view) {
    for (int32_t x = 0; x < 3; ++x) {
        for (int32_t c = 0; c < 3; ++c) {
            EXPECT_EQ(view.data[x * 3 + c], static_cast<std::byte>(5 + 1 + x)) << "x=" << x << " c=" << c;
        }
    }
}

TEST(Crop, InterleavedOddViewOnLooseStride) {
    std::vector<std::byte> storage;
    const ImageView src = make_loose_rgb_view(storage);
    ASSERT_TRUE(validate(src).ok());

    const RectI roi{1, 1, 3, 1};
    auto dst = crop(src, roi, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(dst.value().width(), 3);
    EXPECT_EQ(dst.value().height(), 1);
    EXPECT_EQ(dst.value().byte_size(), int64_t{3} * 3);  // packed RGB rows
    const ImageView view = dst.value().view();
    ASSERT_TRUE(validate(view).ok());
    verify_cropped_rgb_row(view);
}

struct RotationCase {
    Rotation rotation;
    int32_t presented_width;
    int32_t presented_height;
    RectF expected_raw_rect;
};

constexpr int32_t kRawWidth = 6;
constexpr int32_t kRawHeight = 4;

/// Presented-space pixel value for a raw gradient v(x, y) = y * kRawWidth + x
/// physically rotated by `rotation` (make_rotation convention: k90 is clockwise).
int32_t presented_value(const RotationCase& case_data, int32_t x, int32_t y) {
    switch (case_data.rotation) {
        case Rotation::k0:
            return y * kRawWidth + x;
        case Rotation::k90:
            return (kRawHeight - x) * kRawWidth + y;
        case Rotation::k180:
            return (kRawHeight - 1 - y) * kRawWidth + (kRawWidth - 1 - x);
        case Rotation::k270:
            return x * kRawWidth + (kRawWidth - 1 - y);
    }
    return 0;
}

/// Physically rotated gradient in the presented orientation (tight gray rows).
ImageView make_rotated_view(const RotationCase& case_data, std::vector<std::byte>& storage) {
    const auto plane_bytes = static_cast<size_t>(case_data.presented_width) * case_data.presented_height;
    storage.assign(plane_bytes, std::byte{0});
    for (int32_t y = 0; y < case_data.presented_height; ++y) {
        for (int32_t x = 0; x < case_data.presented_width; ++x) {
            const int32_t value = presented_value(case_data, x, y);
            storage[static_cast<size_t>(y) * case_data.presented_width + x] = static_cast<std::byte>(value);
        }
    }
    ImageView src;
    src.data = storage.data();
    src.width = case_data.presented_width;
    src.height = case_data.presented_height;
    src.row_stride_bytes = case_data.presented_width;
    src.format = PixelFormat::kGray8;
    src.rotation = case_data.rotation;
    return src;
}

void verify_crop_content(const ImageView& view, const RotationCase& case_data, const RectI& roi) {
    for (int32_t j = 0; j < roi.height; ++j) {
        const std::byte* row = view.data + static_cast<int64_t>(j) * view.row_stride_bytes;
        for (int32_t i = 0; i < roi.width; ++i) {
            const int32_t expected = presented_value(case_data, roi.x + i, roi.y + j);
            EXPECT_EQ(std::to_integer<int32_t>(row[i]), expected)
                << "rotation " << static_cast<int>(case_data.rotation) << " pixel (" << i << ", " << j << ")";
        }
    }
}

void verify_coordinate_recovery(const RotationCase& case_data, const RectI& roi) {
    // Coordinate recovery (RULE-05): the rotation->crop chain inverted maps the
    // cropped rect back to the analytically known raw-frame rect.
    const auto rotation_transform = make_rotation(case_data.rotation, kRawWidth, kRawHeight, CoordinateSpaceId::kFrame,
                                                  CoordinateSpaceId::kOriented);
    const RectF roi_rect{static_cast<float>(roi.x), static_cast<float>(roi.y), static_cast<float>(roi.width),
                         static_cast<float>(roi.height)};
    const auto chain =
        compose(rotation_transform, make_crop(roi_rect, CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped));
    ASSERT_TRUE(chain.ok());
    const auto recovered_chain = inverse(chain.value());
    ASSERT_TRUE(recovered_chain.ok());
    const RectF cropped{0.0F, 0.0F, static_cast<float>(roi.width), static_cast<float>(roi.height)};
    const RectF recovered = transform_rect(recovered_chain.value(), cropped);
    EXPECT_NEAR(recovered.x, case_data.expected_raw_rect.x, 1e-3) << static_cast<int>(case_data.rotation);
    EXPECT_NEAR(recovered.y, case_data.expected_raw_rect.y, 1e-3) << static_cast<int>(case_data.rotation);
    EXPECT_NEAR(recovered.width, case_data.expected_raw_rect.width, 1e-3) << static_cast<int>(case_data.rotation);
    EXPECT_NEAR(recovered.height, case_data.expected_raw_rect.height, 1e-3) << static_cast<int>(case_data.rotation);
}

TEST(Crop, RotationMatrixKeepsPresentedSpaceAndRecoversFrameRect) {
    const RectI roi{1, 1, 3, 2};
    const std::vector<RotationCase> kCases = {
        {Rotation::k0, kRawWidth, kRawHeight, RectF{1, 1, 3, 2}},
        {Rotation::k90, kRawHeight, kRawWidth, RectF{1, kRawHeight - 1 - 3, 2, 3}},
        {Rotation::k180, kRawWidth, kRawHeight, RectF{kRawWidth - 1 - 3, kRawHeight - 1 - 2, 3, 2}},
        {Rotation::k270, kRawHeight, kRawWidth, RectF{kRawWidth - 1 - 2, 1, 2, 3}},
    };

    for (const RotationCase& case_data : kCases) {
        std::vector<std::byte> storage;
        const ImageView src = make_rotated_view(case_data, storage);
        ASSERT_TRUE(validate(src).ok());

        auto dst = crop(src, roi, kBudget);
        ASSERT_TRUE(dst.ok()) << static_cast<int>(case_data.rotation);
        const ImageView view = dst.value().view();
        ASSERT_TRUE(validate(view).ok());
        EXPECT_EQ(view.rotation, Rotation::k0) << "output is a fresh k0 presentation";
        verify_crop_content(view, case_data, roi);
        verify_coordinate_recovery(case_data, roi);
    }
}

/// 8x8 NV12: luma byte = index, chroma byte = 100 + chroma plane index.
ImageBuffer make_nv12_gradient() {
    auto src = ImageBuffer::create(PixelFormat::kNv12, 8, 8, kBudget);
    EXPECT_TRUE(src.ok());
    ImageBuffer buffer = src.take_value();
    std::byte* bytes = buffer.data();
    for (int32_t i = 0; i < 64; ++i) {
        bytes[i] = static_cast<std::byte>(i);
        bytes[64 + i] = static_cast<std::byte>(100 + i);
    }
    return buffer;
}

TEST(Crop, Nv12OddRoiSubsamplesChroma) {
    const ImageBuffer src_buffer = make_nv12_gradient();

    const RectI roi{3, 3, 5, 5};  // odd ROI: chroma origin (1, 1), size 3x3
    auto dst = crop(src_buffer.view(), roi, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(dst.value().width(), 5);
    EXPECT_EQ(dst.value().height(), 5);
    // Luma 25 bytes + chroma rows ceil(5 / 2) UV pairs = 6 bytes x 3 rows.
    EXPECT_EQ(dst.value().byte_size(), 25 + 18);

    const ImageView view = dst.value().view();
    ASSERT_TRUE(validate(view).ok());
    EXPECT_EQ(view.secondary_plane.row_stride_bytes, 6);
    for (int32_t y = 0; y < 5; ++y) {
        const std::byte* dst_row = view.data + static_cast<int64_t>(y) * view.row_stride_bytes;
        for (int32_t x = 0; x < 5; ++x) {
            EXPECT_EQ(dst_row[x], static_cast<std::byte>((3 + y) * 8 + (3 + x))) << "luma " << x << "," << y;
        }
    }
    for (int32_t y = 0; y < 3; ++y) {
        const std::byte* dst_row = view.secondary_plane.data + static_cast<int64_t>(y) * 6;
        for (int32_t x = 0; x < 3; ++x) {
            const auto expected = static_cast<int32_t>(100 + (1 + y) * 8 + (1 + x));
            EXPECT_EQ(dst_row[x], static_cast<std::byte>(expected)) << "chroma " << x << "," << y;
        }
    }
}

/// 4x4 tight gray view over caller-owned storage.
ImageView make_gray_view(std::vector<std::byte>& storage, int32_t width, int32_t height) {
    storage.assign(static_cast<size_t>(width) * height, std::byte{0});
    ImageView src;
    src.data = storage.data();
    src.width = width;
    src.height = height;
    src.row_stride_bytes = width;
    src.format = PixelFormat::kGray8;
    return src;
}

void expect_crop_rejected(const ImageView& src, const RectI& roi) {
    const auto result = crop(src, roi, kBudget);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
}

TEST(Crop, RejectsEmptyOutOfBoundsAndSmallBudgets) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 4, 4);

    expect_crop_rejected(src, RectI{0, 0, 0, 4});          // empty width
    expect_crop_rejected(src, RectI{0, 0, 4, 0});          // empty height
    expect_crop_rejected(src, RectI{-1, 0, 2, 2});         // negative origin
    expect_crop_rejected(src, RectI{2, 2, 3, 2});          // extends past the right edge
    expect_crop_rejected(src, RectI{0, 0, 5, 2});          // extends past the view
    expect_crop_rejected(src, RectI{INT32_MAX, 0, 2, 2});  // unusable edge arithmetic

    const auto budget = crop(src, RectI{0, 0, 4, 4}, 15);  // needs 16 bytes
    ASSERT_FALSE(budget.ok());
    EXPECT_EQ(budget.status().code(), ErrorCode::kBudgetExceeded);

    const auto invalid_view = crop(ImageView{}, RectI{0, 0, 2, 2}, kBudget);
    ASSERT_FALSE(invalid_view.ok());
    EXPECT_EQ(invalid_view.status().code(), ErrorCode::kInvalidArgument);
}

TEST(Crop, OutputMatchesSourceSlice) {
    std::vector<std::byte> storage(63, std::byte{0});  // 9x7 tight gray
    for (int32_t y = 0; y < 7; ++y) {
        for (int32_t x = 0; x < 9; ++x) {
            const auto value = static_cast<int32_t>((x * 31 + y * 17) % 256);
            storage[static_cast<size_t>(y) * 9 + x] = static_cast<std::byte>(value);
        }
    }
    ImageView src;
    src.data = storage.data();
    src.width = 9;
    src.height = 7;
    src.row_stride_bytes = 9;
    src.format = PixelFormat::kGray8;

    const RectI view_rect{0, 0, 9, 7};
    const RectI roi{2, 1, 6, 5};
    ASSERT_TRUE(contains(view_rect, roi));
    auto dst = crop(src, roi, kBudget);
    ASSERT_TRUE(dst.ok());
    const ImageView view = dst.value().view();
    for (int32_t y = 0; y < roi.height; ++y) {
        const std::byte* dst_row = view.data + static_cast<int64_t>(y) * view.row_stride_bytes;
        for (int32_t x = 0; x < roi.width; ++x) {
            const auto src_index = static_cast<size_t>(roi.y + y) * 9 + (roi.x + x);
            EXPECT_EQ(dst_row[x], storage[src_index]);
        }
    }
}

}  // namespace
