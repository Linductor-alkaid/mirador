// M3-06: unit tests for DB postprocess (AABB reference) and contour box
// recovery over the shared 8-connected component labeling (design section 13,
// DEC-014 reference adaptation scope).

#include <mirador/text_postprocess.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace {

using mirador::ContourBoxParams;
using mirador::ErrorCode;
using mirador::ImageView;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::TextRegion;
using mirador::DbPostprocessParams;

/// Gray view over a byte grid with explicit stride control.
struct Map {
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

[[nodiscard]] Map make_map(int32_t width, int32_t height, int64_t stride) {
    Map map;
    map.width = width;
    map.height = height;
    map.stride = stride;
    map.pixels.assign(static_cast<size_t>(height) * static_cast<size_t>(stride), 0);
    return map;
}

void fill_rect(Map& map, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t value) {
    for (int32_t yy = y; yy < y + h; ++yy) {
        for (int32_t xx = x; xx < x + w; ++xx) {
            map.at(xx, yy) = value;
        }
    }
}

TEST(DbPostprocess, RecoversExpandedBoxWithMeanScore) {
    Map map = make_map(40, 30, 40);
    fill_rect(map, 10, 8, 12, 6, 230);  // mean score = 230/255 ≈ 0.902

    const auto boxes = mirador::db_postprocess_aabb(map.view(), DbPostprocessParams{});
    ASSERT_TRUE(boxes.ok()) << boxes.status().message();
    ASSERT_EQ(boxes.value().size(), 1U);
    const TextRegion& box = boxes.value()[0];
    EXPECT_TRUE(box.utf8_text.empty());  // reference output: no text, no polygon
    EXPECT_TRUE(box.polygon.empty());

    const RectF raw{10.0F, 8.0F, 12.0F, 6.0F};
    const double offset = 12.0 * 6.0 * 1.5 / (2.0 * (12.0 + 6.0));  // = 3.0
    EXPECT_NEAR(box.bounds.x, raw.x - offset, 1e-4);
    EXPECT_NEAR(box.bounds.y, raw.y - offset, 1e-4);
    EXPECT_NEAR(box.bounds.width, raw.width + 2 * offset, 1e-4);
    EXPECT_NEAR(box.bounds.height, raw.height + 2 * offset, 1e-4);
    EXPECT_NEAR(box.confidence, 230.0 / 255.0, 1e-4);
}

TEST(DbPostprocess, ScanOrderAndMultipleBoxes) {
    Map map = make_map(40, 30, 40);
    fill_rect(map, 20, 15, 5, 5, 200);  // second in scan order
    fill_rect(map, 2, 4, 5, 5, 200);    // first: topmost, then leftmost

    DbPostprocessParams params;
    const auto boxes = mirador::db_postprocess_aabb(map.view(), params);
    ASSERT_TRUE(boxes.ok());
    ASSERT_EQ(boxes.value().size(), 2U);
    // 5x5 box, ratio 1.5: offset = 25 * 1.5 / 20 = 1.875 per side.
    EXPECT_NEAR(boxes.value()[0].bounds.x, 2.0 - 1.875, 1e-4);
    EXPECT_NEAR(boxes.value()[0].bounds.y, 4.0 - 1.875, 1e-4);
    EXPECT_LT(boxes.value()[0].bounds.y, boxes.value()[1].bounds.y);
}

TEST(DbPostprocess, FiltersSmallAndDimComponents) {
    Map map = make_map(40, 30, 40);
    fill_rect(map, 1, 1, 2, 2, 250);    // 4 px: exactly at the default min_box_pixels, kept
    fill_rect(map, 10, 1, 1, 1, 250);   // 1 px: dropped by min_box_pixels
    fill_rect(map, 20, 1, 5, 5, 100);   // mean 100/255 ≈ 0.39: dropped by min_mean_score

    const auto boxes = mirador::db_postprocess_aabb(map.view(), DbPostprocessParams{});
    ASSERT_TRUE(boxes.ok());
    ASSERT_EQ(boxes.value().size(), 1U);
    // 2x2 box: offset = 4 * 1.5 / 8 = 0.75 per side.
    EXPECT_NEAR(boxes.value()[0].bounds.x, 1.0 - 0.75, 1e-4);
    EXPECT_NEAR(boxes.value()[0].confidence, 250.0 / 255.0, 1e-4);
}

TEST(DbPostprocess, BoxCapFailsExplicitlyAndClampsExpansion) {
    Map map = make_map(20, 20, 20);
    fill_rect(map, 0, 0, 4, 4, 200);     // corner box: expansion clamps to the map
    fill_rect(map, 10, 10, 4, 4, 200);

    DbPostprocessParams params;
    params.max_boxes = 1;
    const auto capped = mirador::db_postprocess_aabb(map.view(), params);
    ASSERT_FALSE(capped.ok());
    ASSERT_EQ(capped.status().code(), ErrorCode::kBudgetExceeded);

    const auto boxes = mirador::db_postprocess_aabb(map.view(), DbPostprocessParams{});
    ASSERT_TRUE(boxes.ok());
    ASSERT_EQ(boxes.value().size(), 2U);
    const RectF& corner = boxes.value()[0].bounds;
    EXPECT_GE(corner.x, 0.0F);  // unclip clamped at the map origin
    EXPECT_GE(corner.y, 0.0F);
}

TEST(DbPostprocess, ExplicitErrors) {
    Map map = make_map(20, 20, 20);
    fill_rect(map, 2, 2, 4, 4, 200);

    DbPostprocessParams bad_threshold;
    bad_threshold.binarize_threshold = 0;
    ASSERT_EQ(mirador::db_postprocess_aabb(map.view(), bad_threshold).status().code(), ErrorCode::kInvalidArgument);

    DbPostprocessParams bad_budget;
    bad_budget.work_budget_bytes = 16;
    ASSERT_EQ(mirador::db_postprocess_aabb(map.view(), bad_budget).status().code(), ErrorCode::kInvalidArgument);

    // A bitmap beyond the (otherwise valid) budget fails explicitly.
    Map big = make_map(128, 128, 128);
    DbPostprocessParams tight_budget;
    tight_budget.work_budget_bytes = 1024;  // bitmap needs 2048 bytes
    ASSERT_EQ(mirador::db_postprocess_aabb(big.view(), tight_budget).status().code(), ErrorCode::kBudgetExceeded);

    ImageView invalid;
    ASSERT_EQ(mirador::db_postprocess_aabb(invalid, DbPostprocessParams{}).status().code(),
              ErrorCode::kInvalidArgument);

    // A structurally valid non-gray view is rejected by format.
    std::vector<uint8_t> rgba(8 * 32, 0);
    ImageView wrong_format;
    wrong_format.data = reinterpret_cast<const std::byte*>(rgba.data());
    wrong_format.width = 8;
    wrong_format.height = 8;
    wrong_format.row_stride_bytes = 32;
    wrong_format.format = PixelFormat::kRgba8;
    ASSERT_EQ(mirador::db_postprocess_aabb(wrong_format, DbPostprocessParams{}).status().code(),
              ErrorCode::kUnsupportedFormat);
}

TEST(DbPostprocess, StridePaddingDoesNotChangeResults) {
    Map tight = make_map(20, 16, 20);
    fill_rect(tight, 3, 3, 6, 5, 220);
    Map padded = make_map(20, 16, 24);
    for (int32_t y = 0; y < 16; ++y) {
        for (int32_t x = 0; x < 20; ++x) {
            padded.at(x, y) = tight.at(x, y);
        }
    }
    const auto first = mirador::db_postprocess_aabb(tight.view(), DbPostprocessParams{});
    const auto second = mirador::db_postprocess_aabb(padded.view(), DbPostprocessParams{});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(first.value().size(), second.value().size());
    EXPECT_EQ(first.value()[0].bounds, second.value()[0].bounds);
    EXPECT_EQ(first.value()[0].confidence, second.value()[0].confidence);
}

TEST(ContourBoxes, ExactBoundsInScanOrder) {
    Map map = make_map(30, 20, 30);
    // L-shaped component: bounds span the full extent, not the shape.
    map.at(2, 2) = 255;
    map.at(3, 2) = 255;
    map.at(2, 3) = 255;
    map.at(2, 4) = 255;
    fill_rect(map, 15, 10, 6, 3, 128);  // second component (diagonal to the first)

    ContourBoxParams params;
    const auto boxes = mirador::recover_contour_boxes(map.view(), params);
    ASSERT_TRUE(boxes.ok()) << boxes.status().message();
    ASSERT_EQ(boxes.value().size(), 2U);
    EXPECT_EQ(boxes.value()[0], (RectF{2.0F, 2.0F, 2.0F, 3.0F}));   // exact AABB, no expansion
    EXPECT_EQ(boxes.value()[1], (RectF{15.0F, 10.0F, 6.0F, 3.0F}));
}

TEST(ContourBoxes, EightConnectivityJoinsDiagonalsAndThresholdGates) {
    Map map = make_map(20, 20, 20);
    map.at(2, 2) = 255;
    map.at(3, 3) = 255;  // diagonal neighbor: one component with 8-connectivity
    map.at(10, 2) = 10;  // below the default threshold of 1? no: >= 1 -> foreground

    ContourBoxParams diagonal_params;
    diagonal_params.foreground_threshold = 1;
    const auto boxes = mirador::recover_contour_boxes(map.view(), diagonal_params);
    ASSERT_TRUE(boxes.ok());
    ASSERT_EQ(boxes.value().size(), 2U);

    ContourBoxParams gated;
    gated.foreground_threshold = 32;  // value 10 no longer counts
    const auto gated_boxes = mirador::recover_contour_boxes(map.view(), gated);
    ASSERT_TRUE(gated_boxes.ok());
    ASSERT_EQ(gated_boxes.value().size(), 1U);
    EXPECT_EQ(gated_boxes.value()[0], (RectF{2.0F, 2.0F, 2.0F, 2.0F}));
}

TEST(ContourBoxes, ExplicitErrors) {
    Map map = make_map(10, 10, 10);
    fill_rect(map, 1, 1, 2, 2, 255);

    ContourBoxParams bad;
    bad.max_boxes = 0;
    ASSERT_EQ(mirador::recover_contour_boxes(map.view(), bad).status().code(), ErrorCode::kInvalidArgument);

    ContourBoxParams capped;
    capped.max_boxes = 1;
    fill_rect(map, 5, 5, 2, 2, 255);  // second component
    ASSERT_EQ(mirador::recover_contour_boxes(map.view(), capped).status().code(), ErrorCode::kBudgetExceeded);
}

}  // namespace
