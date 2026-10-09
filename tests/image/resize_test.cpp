#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::kMaxImageDimension;
using mirador::make_scale;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::resize_area;
using mirador::transform_rect;
using mirador::validate;

constexpr int64_t kBudget = 1 << 20;

/// Tight gray view over caller-owned storage filled by `fill`.
ImageView make_gray_view(std::vector<std::byte>& storage, int32_t width, int32_t height,
                         const std::vector<int32_t>& values) {
    storage.assign(values.size(), std::byte{0});
    for (size_t i = 0; i < values.size(); ++i) {
        storage[i] = static_cast<std::byte>(values[i]);
    }
    ImageView src;
    src.data = storage.data();
    src.width = width;
    src.height = height;
    src.row_stride_bytes = width;
    src.format = PixelFormat::kGray8;
    return src;
}

TEST(ResizeArea, DownscaleByTwoAveragesExactBlocks) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 4, 2, {10, 20, 30, 40, 50, 60, 70, 80});

    auto dst = resize_area(src, 2, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    const ImageView view = dst.value().view();
    ASSERT_TRUE(validate(view).ok());
    EXPECT_EQ(view.format, PixelFormat::kGray8);
    // Each destination pixel is the mean of its 2x2 source block.
    EXPECT_EQ(std::to_integer<int32_t>(view.data[0]), 35);
    EXPECT_EQ(std::to_integer<int32_t>(view.data[1]), 55);
}

TEST(ResizeArea, NonIntegerRatioUsesExactCoverageWeights) {
    // 3x1 -> 2x1: destination 0 covers [0, 1.5) with weights 2:1 on the columns,
    // destination 1 covers [1.5, 3) with weights 1:2.
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 3, 1, {30, 60, 90});

    auto dst = resize_area(src, 2, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(std::to_integer<int32_t>(dst.value().view().data[0]), 40);  // (2*30 + 60) / 3
    EXPECT_EQ(std::to_integer<int32_t>(dst.value().view().data[1]), 80);  // (60 + 2*90) / 3
}

TEST(ResizeArea, UpscaleReplicatesCoverageExactly) {
    // 2x1 -> 4x1: destination pixels fully inside a source column replicate it.
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 2, 1, {10, 20});

    auto dst = resize_area(src, 4, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    const std::vector<int32_t> expected = {10, 10, 20, 20};
    for (int32_t x = 0; x < 4; ++x) {
        EXPECT_EQ(std::to_integer<int32_t>(dst.value().view().data[x]), expected[x]) << "x=" << x;
    }
}

/// Collapses an image to its single-pixel 1x1 area average.
int32_t single_pixel_average(const ImageView& src) {
    auto dst = resize_area(src, 1, 1, kBudget);
    EXPECT_TRUE(dst.ok());
    if (!dst.ok()) {
        return -1;
    }
    return std::to_integer<int32_t>(dst.value().view().data[0]);
}

TEST(ResizeArea, RoundsHalfUpOnFractionalAverages) {
    // 2x1 -> 1x1 of {1, 2}: average is 1.5 and rounds up to 2; {0, 1} averages
    // 0.5 and rounds up to 1; {1, 1} is exact and stays 1.
    std::vector<std::byte> storage;
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {1, 2})), 2);
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {0, 1})), 1);
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {1, 1})), 1);
    EXPECT_EQ(single_pixel_average(make_gray_view(storage, 2, 1, {254, 255})), 255);  // clamped
}

TEST(ResizeArea, OutputIsStrideInvariant) {
    const std::vector<int32_t> kValues = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    std::vector<std::byte> tight_storage;
    const ImageView tight = make_gray_view(tight_storage, 4, 3, kValues);

    // Same logical pixels on a loose stride of 7 (minimum is 4).
    std::vector<std::byte> loose_storage(21, std::byte{0});  // 3 loose rows of 7 bytes
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            loose_storage[static_cast<size_t>(y) * 7 + x] = static_cast<std::byte>(kValues[y * 4 + x]);
        }
    }
    ImageView loose;
    loose.data = loose_storage.data();
    loose.width = 4;
    loose.height = 3;
    loose.row_stride_bytes = 7;
    loose.format = PixelFormat::kGray8;

    auto from_tight = resize_area(tight, 2, 2, kBudget);
    auto from_loose = resize_area(loose, 2, 2, kBudget);
    ASSERT_TRUE(from_tight.ok());
    ASSERT_TRUE(from_loose.ok());
    for (int32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(from_tight.value().view().data[i], from_loose.value().view().data[i]) << "byte " << i;
    }
}

TEST(ResizeArea, SameSizeRequestIsACopy) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 3, 2, {5, 10, 15, 20, 25, 30});

    auto dst = resize_area(src, 3, 2, kBudget);
    ASSERT_TRUE(dst.ok());
    for (int32_t i = 0; i < 6; ++i) {
        EXPECT_EQ(dst.value().view().data[i], static_cast<std::byte>(5 * (i + 1))) << "byte " << i;
    }
}

/// 2x2 RGB on a padded stride: (0,0)=red, (1,0)=green, (0,1)=blue, (1,1)=white.
ImageView make_rgb_quad(std::vector<std::byte>& storage) {
    storage.assign(12, std::byte{0});  // 2 rows of stride 6
    const std::vector<std::vector<int32_t>> pixels = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int32_t p = 0; p < 4; ++p) {
        const auto base = static_cast<size_t>(p / 2) * 6 + static_cast<size_t>(p % 2) * 3;
        for (int32_t c = 0; c < 3; ++c) {
            storage[base + static_cast<size_t>(c)] = static_cast<std::byte>(pixels[p][c]);
        }
    }
    ImageView src;
    src.data = storage.data();
    src.width = 2;
    src.height = 2;
    src.row_stride_bytes = 6;
    src.format = PixelFormat::kRgb8;
    return src;
}

TEST(ResizeArea, InterleavedResizeAveragesChannelsIndependently) {
    std::vector<std::byte> storage;
    const ImageView src = make_rgb_quad(storage);
    ASSERT_TRUE(validate(src).ok());

    auto dst = resize_area(src, 1, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(dst.value().format(), PixelFormat::kRgb8);
    const std::byte* pixel = dst.value().view().data;
    // Averages are 127.5; the documented half-up rounding yields 128.
    EXPECT_EQ(std::to_integer<int32_t>(pixel[0]), 128);
    EXPECT_EQ(std::to_integer<int32_t>(pixel[1]), 128);
    EXPECT_EQ(std::to_integer<int32_t>(pixel[2]), 128);
}

TEST(ResizeArea, Nv12SourceProducesGrayThumbnail) {
    auto src = ImageBuffer::create(PixelFormat::kNv12, 4, 4, kBudget);
    ASSERT_TRUE(src.ok());
    ImageBuffer buffer = src.take_value();
    std::byte* bytes = buffer.data();
    for (int32_t i = 0; i < 16; ++i) {
        bytes[i] = static_cast<std::byte>(i * 4);
    }
    for (int32_t i = 0; i < 8; ++i) {  // chroma: stride 4 x ceil(4 / 2) rows = 8 bytes
        bytes[16 + i] = static_cast<std::byte>(200);
    }

    auto dst = resize_area(buffer.view(), 2, 2, kBudget);
    ASSERT_TRUE(dst.ok());
    EXPECT_EQ(dst.value().format(), PixelFormat::kGray8);
    // 2x2 luma block averages of the 4x4 luma plane.
    const std::byte* luma = dst.value().view().data;
    EXPECT_EQ(std::to_integer<int32_t>(luma[0]), (0 + 4 + 16 + 20) / 4);
    EXPECT_EQ(std::to_integer<int32_t>(luma[1]), (8 + 12 + 24 + 28) / 4);
}

TEST(ResizeArea, ScaleTransformRecoversSourceRect) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 8, 6, std::vector<int32_t>(48, 7));
    auto dst = resize_area(src, 4, 3, kBudget);
    ASSERT_TRUE(dst.ok());

    // RULE-05: the resize is exactly the scale (sw/dw, sh/dh) in coordinate terms.
    const auto scale = make_scale(8.0 / 4.0, 6.0 / 3.0, CoordinateSpaceId::kModelInput, CoordinateSpaceId::kOriented);
    const RectF recovered = transform_rect(scale, RectF{0.0F, 0.0F, 4.0F, 3.0F});
    EXPECT_NEAR(recovered.x, 0.0, 1e-3);
    EXPECT_NEAR(recovered.y, 0.0, 1e-3);
    EXPECT_NEAR(recovered.width, 8.0, 1e-3);
    EXPECT_NEAR(recovered.height, 6.0, 1e-3);
}

void expect_resize_rejected(const ImageView& src, int32_t w, int32_t h) {
    const auto result = resize_area(src, w, h, kBudget);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument);
}

TEST(ResizeArea, RejectsInvalidDestinationSizes) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 4, 4, std::vector<int32_t>(16, 1));
    expect_resize_rejected(src, 0, 2);
    expect_resize_rejected(src, 2, 0);
    expect_resize_rejected(src, -1, 2);
    expect_resize_rejected(src, kMaxImageDimension + 1, 2);
}

TEST(ResizeArea, RejectsSmallBudgetsAndInvalidViews) {
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 4, 4, std::vector<int32_t>(16, 1));

    const auto budget = resize_area(src, 2, 2, 3);  // needs 4 bytes
    ASSERT_FALSE(budget.ok());
    EXPECT_EQ(budget.status().code(), ErrorCode::kBudgetExceeded);

    const auto invalid_view = resize_area(ImageView{}, 2, 2, kBudget);
    ASSERT_FALSE(invalid_view.ok());
    EXPECT_EQ(invalid_view.status().code(), ErrorCode::kInvalidArgument);
}

TEST(ResizeArea, UpscaleSharedSourceWeightsNotOverwritten) {
    // MIRADOR-20261009-001 regression: in a 2 -> 3 upscale one source column
    // backs two destination columns, so coverage weights must be keyed per
    // destination. The old per-source cache let the last write win and turned a
    // fully white row into 128 255 255.
    std::vector<std::byte> storage(6, static_cast<std::byte>(255));  // 1 row of stride 6: two Rgb8 pixels
    ImageView src;
    src.data = storage.data();
    src.width = 2;
    src.height = 1;
    src.row_stride_bytes = 6;
    src.format = PixelFormat::kRgb8;
    ASSERT_TRUE(validate(src).ok());

    auto dst = resize_area(src, 3, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    const std::byte* pixel = dst.value().view().data;
    for (int32_t x = 0; x < 3; ++x) {
        EXPECT_EQ(std::to_integer<int32_t>(pixel[static_cast<ptrdiff_t>(x) * 3]), 255) << "R at x=" << x;
        EXPECT_EQ(std::to_integer<int32_t>(pixel[static_cast<ptrdiff_t>(x) * 3 + 1]), 255) << "G at x=" << x;
        EXPECT_EQ(std::to_integer<int32_t>(pixel[static_cast<ptrdiff_t>(x) * 3 + 2]), 255) << "B at x=" << x;
    }
}

TEST(ResizeArea, UpscaleTwoToThreeBlendsSharedSourceExactWeights) {
    // 2x1 -> 3x1 of {10, 20}: destination 0 is source 0 alone (weight 2/2),
    // destination 1 blends both sources (1/2 + 1/2), destination 2 is source 1
    // alone. The old per-source cache produced {5, 25, 20}.
    std::vector<std::byte> storage;
    const ImageView src = make_gray_view(storage, 2, 1, {10, 20});

    auto dst = resize_area(src, 3, 1, kBudget);
    ASSERT_TRUE(dst.ok());
    const std::vector<int32_t> expected = {10, 15, 20};
    for (int32_t x = 0; x < 3; ++x) {
        EXPECT_EQ(std::to_integer<int32_t>(dst.value().view().data[x]), expected[x]) << "x=" << x;
    }
}

TEST(ResizeArea, VerticalUpscaleTwoToThreeBlendsSharedSourceExactWeights) {
    // Row-direction mirror of the same defect: 1x2 -> 1x3 of {10, 20} must stay
    // {10, 15, 20}; a 2x2 resampled to 2x3 keeps its columns intact.
    std::vector<std::byte> storage;
    auto single = resize_area(make_gray_view(storage, 1, 2, {10, 20}), 1, 3, kBudget);
    ASSERT_TRUE(single.ok());
    const std::vector<int32_t> single_expected = {10, 15, 20};
    for (int32_t y = 0; y < 3; ++y) {
        EXPECT_EQ(std::to_integer<int32_t>(single.value().view().data[y]), single_expected[y]) << "y=" << y;
    }

    const ImageView quad = make_gray_view(storage, 2, 2, {10, 30, 20, 40});
    auto doubled = resize_area(quad, 2, 3, kBudget);
    ASSERT_TRUE(doubled.ok());
    const std::vector<int32_t> doubled_expected = {10, 30, 15, 35, 20, 40};
    for (int32_t i = 0; i < 6; ++i) {
        EXPECT_EQ(std::to_integer<int32_t>(doubled.value().view().data[i]), doubled_expected[i]) << "byte " << i;
    }
}

/// Every channel of a resampled constant image must stay at the constant: the
/// coverage weights of each destination pixel sum to exactly sw * sh, so any
/// ratio — including upscales and non-integer ratios — is a fixed point.
void expect_constant_preserved(PixelFormat format, int32_t sw, int32_t sh, int32_t dw, int32_t dh, int32_t value) {
    const auto bpp = mirador::bytes_per_pixel(format);
    std::vector<std::byte> storage(static_cast<size_t>(sw) * sh * bpp, static_cast<std::byte>(value));
    ImageView src;
    src.data = storage.data();
    src.width = sw;
    src.height = sh;
    src.row_stride_bytes = static_cast<int64_t>(sw) * bpp;
    src.format = format;
    ASSERT_TRUE(validate(src).ok());

    auto dst = resize_area(src, dw, dh, kBudget);
    ASSERT_TRUE(dst.ok());
    const ImageView view = dst.value().view();
    for (int32_t y = 0; y < dh; ++y) {
        for (int32_t x = 0; x < dw; ++x) {
            for (int32_t c = 0; c < bpp; ++c) {
                const auto byte =
                    view.data[static_cast<int64_t>(y) * view.row_stride_bytes + static_cast<int64_t>(x) * bpp + c];
                EXPECT_EQ(std::to_integer<int32_t>(byte), value)
                    << "channel " << c << " at (" << x << ", " << y << ") for " << sw << "x" << sh << " -> " << dw
                    << "x" << dh;
            }
        }
    }
}

TEST(ResizeArea, ConstantImagesStayConstantAcrossAllRatios) {
    const std::vector<std::pair<int32_t, int32_t>> ratios = {{2, 3}, {3, 2}, {2, 5}, {5, 3}, {1, 7}};
    for (const auto& [src_count, dst_count] : ratios) {
        expect_constant_preserved(PixelFormat::kGray8, src_count, 2, dst_count, 2, 137);  // horizontal axis
        expect_constant_preserved(PixelFormat::kGray8, 2, src_count, 2, dst_count, 137);  // vertical axis
        expect_constant_preserved(PixelFormat::kRgb8, src_count, 3, dst_count, 2, 255);   // both axes
    }
}

/// Naive ground-truth resample used to guard the coverage-table rewrite: every
/// destination pixel accumulates the exact source overlap
/// `min((d+1)*src, (s+1)*dst) - max(d*src, s*dst)` per source pixel directly
/// and rounds half up by (sw * sh). Deliberately table-free so it cannot share
/// a defect with the implementation under test.
void reference_resize_area(const std::byte* src, int64_t src_stride, int32_t sw, int32_t sh, std::byte* dst,
                           int64_t dst_stride, int32_t bpp, int32_t dw, int32_t dh) {
    const int64_t total_weight = static_cast<int64_t>(sw) * sh;
    const int64_t half = total_weight / 2;
    for (int32_t dy = 0; dy < dh; ++dy) {
        for (int32_t dx = 0; dx < dw; ++dx) {
            std::array<int64_t, 4> acc{};
            for (int32_t sy = 0; sy < sh; ++sy) {
                for (int32_t sx = 0; sx < sw; ++sx) {
                    const int64_t wy = std::min(static_cast<int64_t>(dy + 1) * sh, static_cast<int64_t>(sy + 1) * dh) -
                                       std::max(static_cast<int64_t>(dy) * sh, static_cast<int64_t>(sy) * dh);
                    const int64_t wx = std::min(static_cast<int64_t>(dx + 1) * sw, static_cast<int64_t>(sx + 1) * dw) -
                                       std::max(static_cast<int64_t>(dx) * sw, static_cast<int64_t>(sx) * dw);
                    const int64_t weight = std::max<int64_t>(wy, 0) * std::max<int64_t>(wx, 0);
                    if (weight == 0) {
                        continue;
                    }
                    const std::byte* pixel =
                        src + static_cast<int64_t>(sy) * src_stride + static_cast<int64_t>(sx) * bpp;
                    for (int32_t c = 0; c < bpp; ++c) {
                        acc[c] += weight * std::to_integer<int32_t>(pixel[c]);
                    }
                }
            }
            std::byte* out = dst + static_cast<int64_t>(dy) * dst_stride + static_cast<int64_t>(dx) * bpp;
            for (int32_t c = 0; c < bpp; ++c) {
                const auto value = static_cast<int32_t>((acc[c] + half) / total_weight);
                out[c] = static_cast<std::byte>(std::clamp(value, 0, 255));
            }
        }
    }
}

/// Deterministic pattern byte (32-bit LCG) so reference comparisons never
/// depend on rand() or platform seeding.
int32_t next_pattern_byte(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return static_cast<int32_t>((state >> 16) % 256U);
}

/// Builds a pseudo-random padded source view. `stride_padding` extra bytes
/// per row are poisoned so reads outside the logical pixels surface as wrong
/// values instead of silent luck.
ImageView make_pattern_source(std::vector<std::byte>& storage, PixelFormat format, int32_t sw, int32_t sh,
                              int64_t stride_padding, uint32_t& seed) {
    const auto bpp = mirador::bytes_per_pixel(format);
    const int64_t src_stride = static_cast<int64_t>(sw) * bpp + stride_padding;
    storage.assign(static_cast<size_t>(src_stride) * sh, std::byte{0});
    for (int32_t y = 0; y < sh; ++y) {
        for (int32_t x = 0; x < sw; ++x) {
            for (int32_t c = 0; c < bpp; ++c) {
                storage[static_cast<size_t>(y) * src_stride + static_cast<size_t>(x) * bpp + c] =
                    static_cast<std::byte>(next_pattern_byte(seed));
            }
        }
        for (int64_t p = static_cast<int64_t>(sw) * bpp; p < src_stride; ++p) {
            storage[static_cast<size_t>(y) * src_stride + static_cast<size_t>(p)] = std::byte{0xAA};
        }
    }
    ImageView src;
    src.data = storage.data();
    src.width = sw;
    src.height = sh;
    src.row_stride_bytes = src_stride;
    src.format = format;
    return src;
}

/// Compares every destination byte of `view` against the tight packed
/// `expected` buffer.
void expect_view_equals(const ImageView& view, const std::vector<std::byte>& expected, int32_t dw, int32_t dh,
                        const std::string& context) {
    const auto bpp = mirador::bytes_per_pixel(view.format);
    for (int32_t y = 0; y < dh; ++y) {
        for (int32_t x = 0; x < dw; ++x) {
            for (int32_t c = 0; c < bpp; ++c) {
                const size_t index = (static_cast<size_t>(y) * dw + x) * bpp + c;
                const auto actual =
                    view.data[static_cast<int64_t>(y) * view.row_stride_bytes + static_cast<int64_t>(x) * bpp + c];
                EXPECT_EQ(actual, expected[index]) << context << " channel " << c << " at (" << x << ", " << y << ")";
            }
        }
    }
}

/// Resamples pseudo-random content with `resize_area` and compares every
/// destination byte against `reference_resize_area`.
void expect_matches_reference(PixelFormat format, int32_t sw, int32_t sh, int32_t dw, int32_t dh, uint32_t seed,
                              int64_t stride_padding) {
    const auto bpp = mirador::bytes_per_pixel(format);
    std::vector<std::byte> storage;
    const ImageView src = make_pattern_source(storage, format, sw, sh, stride_padding, seed);
    ASSERT_TRUE(validate(src).ok());

    auto dst = resize_area(src, dw, dh, kBudget);
    ASSERT_TRUE(dst.ok());
    const ImageView view = dst.value().view();

    std::vector<std::byte> expected(static_cast<size_t>(dw) * dh * bpp, std::byte{0});
    reference_resize_area(src.data, src.row_stride_bytes, sw, sh, expected.data(), static_cast<int64_t>(dw) * bpp, bpp,
                          dw, dh);
    expect_view_equals(
        view, expected, dw, dh,
        std::to_string(sw) + "x" + std::to_string(sh) + " -> " + std::to_string(dw) + "x" + std::to_string(dh));
}

TEST(ResizeArea, MatchesDirectCoverageReferenceOnMixedRatios) {
    // Strongest guard for MIRADOR-20261009-001: the destination-keyed coverage
    // tables must stay bit-identical to the direct per-pixel accumulation for
    // upscales, downscales, non-integer ratios and two-axis mixes.
    expect_matches_reference(PixelFormat::kGray8, 3, 5, 7, 2, 0x51ED270BU, 0);
    expect_matches_reference(PixelFormat::kGray8, 2, 2, 3, 3, 0x2705C0DEU, 0);
    expect_matches_reference(PixelFormat::kGray8, 4, 4, 1, 6, 0x1BADB002U, 0);
    expect_matches_reference(PixelFormat::kGray8, 5, 3, 2, 7, 0x0D15EA5EU, 0);
    expect_matches_reference(PixelFormat::kGray8, 7, 2, 3, 5, 0x5EED5EEDU, 0);  // inverse of the first ratio
    expect_matches_reference(PixelFormat::kRgb8, 2, 2, 3, 3, 0x216E1D0EU, 0);
    expect_matches_reference(PixelFormat::kRgb8, 5, 3, 2, 7, 0x9C0FFEE0U, 0);
}

TEST(ResizeArea, MatchesDirectCoverageReferenceWithPaddedStride) {
    expect_matches_reference(PixelFormat::kGray8, 3, 5, 7, 2, 0x51ED270BU, 5);
    expect_matches_reference(PixelFormat::kRgb8, 4, 3, 9, 2, 0x24424424U, 4);
}

}  // namespace
