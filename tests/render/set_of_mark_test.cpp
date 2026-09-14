// M4 Set-of-Mark renderer tests (M4-06): complete mark mapping, deterministic
// output, palette/box pixel assertions, deterministic label placement with
// occlusion avoidance, budget and input-validation errors, cancellation.

#include <mirador/set_of_mark.hpp>

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::MarkedImage;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::RectF;
using mirador::RectI;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::SetOfMarkResult;
using mirador::SoMMark;
using mirador::SoMRenderOptions;
using mirador::VisualRegion;

/// The renderer-owned fixed palette (design section 17, src/render/set_of_mark.cpp):
/// a region's color is kPalette[stable_id % 8].
struct Rgb {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};
constexpr Rgb kPalette[8]{
    {230, 57, 70},  {42, 157, 143}, {69, 123, 157}, {233, 196, 106},
    {231, 111, 81}, {156, 76, 159}, {42, 109, 62},  {188, 108, 37},
};

constexpr Rgb kBackground{10, 20, 30};

Rgb pixel_of(const MarkedImage& image, int32_t x, int32_t y) {
    const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 3U;
    return Rgb{static_cast<uint8_t>(image.pixels[offset]), static_cast<uint8_t>(image.pixels[offset + 1U]),
               static_cast<uint8_t>(image.pixels[offset + 2U])};
}

bool same_color(const MarkedImage& image, int32_t x, int32_t y, Rgb color) {
    const Rgb actual = pixel_of(image, x, y);
    return actual.red == color.red && actual.green == color.green && actual.blue == color.blue;
}

/// Owned RGB8 background: packed rows, filled with kBackground.
struct TestBackground {
    std::vector<std::byte> bytes;
    ImageView view;

    TestBackground(int32_t width, int32_t height) {
        bytes.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U, std::byte{0});
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const size_t offset =
                    (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3U;
                bytes[offset] = static_cast<std::byte>(kBackground.red);
                bytes[offset + 1U] = static_cast<std::byte>(kBackground.green);
                bytes[offset + 2U] = static_cast<std::byte>(kBackground.blue);
            }
        }
        view.data = bytes.data();
        view.width = width;
        view.height = height;
        view.row_stride_bytes = static_cast<int64_t>(width) * 3;
        view.format = PixelFormat::kRgb8;
        view.rotation = Rotation::k0;
    }
};

VisualRegion make_region(double x, double y, double width, double height, uint64_t stable_id) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds =
        RectF{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height)};
    region.anchor = PointF{static_cast<float>(x + width / 2.0F), static_cast<float>(y + height / 2.0F)};
    return region;
}

bool has_white_pixel_in(const MarkedImage& image, const RectI& area) {
    for (int32_t y = area.y; y < area.y + area.height; ++y) {
        for (int32_t x = area.x; x < area.x + area.width; ++x) {
            if (same_color(image, x, y, Rgb{255, 255, 255})) {
                return true;
            }
        }
    }
    return false;
}

bool marks_equal(const SoMMark& left, const SoMMark& right) {
    return left.mark_id == right.mark_id && left.stable_id == right.stable_id &&
           left.drawn_bounds == right.drawn_bounds && left.anchor == right.anchor;
}

// --- Mark mapping -------------------------------------------------------------------

TEST(SetOfMarkTest, EveryRegionProducesAWellNumberedMark) {
    const TestBackground background(40, 30);
    SemanticSnapshot snapshot;
    snapshot.generation = 4U;
    snapshot.frame_sequence = 11U;
    snapshot.regions = {make_region(2, 2, 6, 6, 7U), make_region(12, 2, 6, 6, 8U),
                        make_region(100, 100, 10, 10, 9U)};  // last one fully outside

    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());
    EXPECT_EQ(rendered.value().generation, 4U);
    EXPECT_EQ(rendered.value().frame_sequence, 11U);
    ASSERT_EQ(rendered.value().marks.size(), snapshot.regions.size());  // mapping is complete

    for (size_t index = 0; index < rendered.value().marks.size(); ++index) {
        const SoMMark& mark = rendered.value().marks[index];
        EXPECT_EQ(mark.mark_id, index + 1U);  // 1-based snapshot order
        EXPECT_EQ(mark.stable_id, snapshot.regions[index].stable_id);
        EXPECT_TRUE(mark.anchor == snapshot.regions[index].anchor);
    }
    // The out-of-bounds region still emits a mark with empty drawn bounds.
    EXPECT_EQ(rendered.value().marks[2].drawn_bounds, (RectI{}));
    // The in-bounds regions carry their clipped boxes.
    EXPECT_EQ(rendered.value().marks[0].drawn_bounds, (RectI{2, 2, 6, 6}));
    EXPECT_EQ(rendered.value().marks[1].drawn_bounds, (RectI{12, 2, 6, 6}));
}

TEST(SetOfMarkTest, EmptySnapshotCopiesTheBackground) {
    const TestBackground background(16, 12);
    const SemanticSnapshot snapshot;
    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());
    EXPECT_TRUE(rendered.value().marks.empty());
    EXPECT_EQ(rendered.value().image.width, 16);
    EXPECT_EQ(rendered.value().image.height, 12);
    EXPECT_EQ(rendered.value().image.pixels, background.bytes);
}

// --- Deterministic drawing --------------------------------------------------------------

TEST(SetOfMarkTest, RepeatedRendersAreByteIdentical) {
    const TestBackground background(48, 36);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(1, 1, 10, 8, 3U), make_region(6, 6, 14, 10, 11U), make_region(30, 20, 10, 10, 5U)};

    const auto first = mirador::render_set_of_mark(background.view, snapshot);
    const auto second = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().image.pixels, second.value().image.pixels);
    ASSERT_EQ(first.value().marks.size(), second.value().marks.size());
    for (size_t index = 0; index < first.value().marks.size(); ++index) {
        EXPECT_TRUE(marks_equal(first.value().marks[index], second.value().marks[index]));
    }
}

TEST(SetOfMarkTest, BoxOutlineUsesThePaletteColorOfTheStableId) {
    const TestBackground background(40, 30);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(5, 5, 10, 10, 3U)};  // palette[3]: yellow

    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());
    ASSERT_EQ(rendered.value().marks.size(), 1U);
    EXPECT_EQ(rendered.value().marks[0].drawn_bounds, (RectI{5, 5, 10, 10}));

    const Rgb expected = kPalette[3 % 8];
    // Border pixels (thickness 2) carry the palette color...
    EXPECT_TRUE(same_color(rendered.value().image, 5, 5, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 14, 5, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 5, 14, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 14, 14, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 6, 6, expected));
    // ...and the interior keeps the background.
    EXPECT_TRUE(same_color(rendered.value().image, 10, 10, kBackground));
    EXPECT_TRUE(same_color(rendered.value().image, 7, 7, kBackground));
}

TEST(SetOfMarkTest, PaletteIndexWrapsWithStableIdModuloEight) {
    const TestBackground background(16, 16);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(0, 0, 4, 4, 10U)};  // 10 % 8 == 2

    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());
    EXPECT_TRUE(same_color(rendered.value().image, 0, 0, kPalette[2]));
}

TEST(SetOfMarkTest, ClippedRegionDrawsOnlyTheVisiblePart) {
    const TestBackground background(40, 30);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(-5, -5, 10, 10, 1U)};

    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());
    EXPECT_EQ(rendered.value().marks[0].drawn_bounds, (RectI{0, 0, 5, 5}));
    const Rgb expected = kPalette[1];
    EXPECT_TRUE(same_color(rendered.value().image, 0, 0, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 4, 4, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 5, 5, kBackground));  // just outside the clip
}

TEST(SetOfMarkTest, FullyOutOfBoundsRegionLeavesPixelsUntouched) {
    const TestBackground background(40, 30);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(100, 100, 10, 10, 2U)};

    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());
    EXPECT_EQ(rendered.value().image.pixels, background.bytes);
}

// --- Label chips --------------------------------------------------------------------------

TEST(SetOfMarkTest, LabelChipIsPlacedAboveTheBoxWhenSpaceAllows) {
    const TestBackground background(40, 30);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(10, 14, 20, 10, 1U)};  // box y 14..23

    SoMRenderOptions options;  // label_height 14 -> scale 2, chip 8x12 for mark 1
    const auto rendered = mirador::render_set_of_mark(background.view, snapshot, options);
    ASSERT_TRUE(rendered.ok());

    const Rgb expected = kPalette[1];
    // Chip occupies {10, 1, 8, 12} (box.y - chip.height - 1).
    EXPECT_TRUE(same_color(rendered.value().image, 10, 1, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 17, 1, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 10, 12, expected));
    EXPECT_TRUE(same_color(rendered.value().image, 9, 1, kBackground));    // left of the chip
    EXPECT_TRUE(same_color(rendered.value().image, 18, 4, kBackground));   // right of the chip
    EXPECT_TRUE(same_color(rendered.value().image, 10, 13, kBackground));  // gap row above the box
    // The digit glyph is drawn white inside the chip.
    EXPECT_TRUE(has_white_pixel_in(rendered.value().image, RectI{10, 1, 8, 12}));
}

TEST(SetOfMarkTest, SecondNeighbouringChipAvoidsThePlacedOne) {
    const TestBackground background(40, 60);
    SemanticSnapshot snapshot;
    // Region A (mark 1): box y 5..14; its chip falls to below-A = {5,16,8,12}.
    // Region B (mark 2): box y 29..48; its first candidate above-B = {5,16,8,12}
    // collides with A's chip, so it must land elsewhere (inside-top {5,29,8,12}).
    snapshot.regions = {make_region(5, 5, 20, 10, 1U), make_region(5, 29, 20, 20, 2U)};

    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());

    const Rgb first_color = kPalette[1];
    const Rgb second_color = kPalette[2];
    EXPECT_TRUE(same_color(rendered.value().image, 5, 16, first_color));   // A's chip below A
    EXPECT_TRUE(same_color(rendered.value().image, 5, 29, second_color));  // B's chip inside-top
    EXPECT_TRUE(same_color(rendered.value().image, 5, 28, kBackground));   // chips never overlap
    // The avoided position shows A's color, not B's.
    EXPECT_FALSE(same_color(rendered.value().image, 5, 16, second_color));
}

TEST(SetOfMarkTest, LabelHeightZeroDisablesChips) {
    const TestBackground background(40, 30);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(10, 14, 20, 10, 1U)};

    SoMRenderOptions options;
    options.label_height = 0;
    const auto rendered = mirador::render_set_of_mark(background.view, snapshot, options);
    ASSERT_TRUE(rendered.ok());
    EXPECT_TRUE(same_color(rendered.value().image, 10, 1, kBackground));   // no chip above
    EXPECT_TRUE(same_color(rendered.value().image, 10, 14, kPalette[1]));  // box still drawn
    EXPECT_EQ(rendered.value().marks[0].drawn_bounds, (RectI{10, 14, 20, 10}));
}

// --- Budgets and validation ----------------------------------------------------------------

TEST(SetOfMarkTest, MoreRegionsThanMaxMarksFailsWithBudgetExceeded) {
    const TestBackground background(40, 30);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(1, 1, 4, 4, 1U), make_region(10, 1, 4, 4, 2U)};

    SoMRenderOptions options;
    options.max_marks = 1;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, options).status().code(),
              ErrorCode::kBudgetExceeded);
}

TEST(SetOfMarkTest, OutputOverByteBudgetFailsWithBudgetExceeded) {
    const TestBackground background(40, 30);  // 40*30*3 = 3600 bytes
    const SemanticSnapshot snapshot;

    SoMRenderOptions options;
    options.max_bytes = 3599;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, options).status().code(),
              ErrorCode::kBudgetExceeded);
}

TEST(SetOfMarkTest, InvalidOptionsAreRejected) {
    const TestBackground background(8, 8);
    const SemanticSnapshot snapshot;

    SoMRenderOptions zero_bytes;
    zero_bytes.max_bytes = 0;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, zero_bytes).status().code(),
              ErrorCode::kInvalidArgument);

    SoMRenderOptions thin_box;
    thin_box.box_thickness = 0;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, thin_box).status().code(),
              ErrorCode::kInvalidArgument);

    SoMRenderOptions thick_box;
    thick_box.box_thickness = 17;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, thick_box).status().code(),
              ErrorCode::kInvalidArgument);

    SoMRenderOptions negative_label;
    negative_label.label_height = -1;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, negative_label).status().code(),
              ErrorCode::kInvalidArgument);

    SoMRenderOptions huge_label;
    huge_label.label_height = 65;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, huge_label).status().code(),
              ErrorCode::kInvalidArgument);

    SoMRenderOptions zero_marks;
    zero_marks.max_marks = 0;
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, zero_marks).status().code(),
              ErrorCode::kInvalidArgument);
}

TEST(SetOfMarkTest, Nv12BackgroundIsUnsupportedAndRotatedBackgroundIsInvalid) {
    // Valid NV12 4x4 view: Y plane + interleaved chroma plane.
    std::vector<std::byte> nv12(16 + 8, std::byte{5});
    ImageView nv12_view;
    nv12_view.data = nv12.data();
    nv12_view.width = 4;
    nv12_view.height = 4;
    nv12_view.row_stride_bytes = 4;
    nv12_view.format = PixelFormat::kNv12;
    nv12_view.rotation = Rotation::k0;
    nv12_view.secondary_plane = mirador::ImagePlane{nv12.data() + 16, 4};
    const SemanticSnapshot snapshot;
    EXPECT_EQ(mirador::render_set_of_mark(nv12_view, snapshot).status().code(), ErrorCode::kUnsupportedFormat);

    const TestBackground background(8, 6);
    ImageView rotated = background.view;
    rotated.rotation = Rotation::k90;
    EXPECT_EQ(mirador::render_set_of_mark(rotated, snapshot).status().code(), ErrorCode::kInvalidArgument);

    ImageView no_data;
    no_data.width = 8;
    no_data.height = 6;
    no_data.row_stride_bytes = 24;
    no_data.format = PixelFormat::kRgb8;
    EXPECT_EQ(mirador::render_set_of_mark(no_data, snapshot).status().code(), ErrorCode::kInvalidArgument);
}

TEST(SetOfMarkTest, CancelledAndExpiredContextsAreExplicit) {
    const TestBackground background(8, 8);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(1, 1, 4, 4, 1U)};

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, {}, cancelled).status().code(),
              ErrorCode::kCancelled);

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    EXPECT_EQ(mirador::render_set_of_mark(background.view, snapshot, {}, expired).status().code(), ErrorCode::kTimeout);
}

// --- MarkedImage view projection -------------------------------------------------------------

TEST(SetOfMarkTest, MarkedImageProjectsAsRgb8K0View) {
    const TestBackground background(9, 7);
    SemanticSnapshot snapshot;
    snapshot.regions = {make_region(1, 1, 4, 4, 5U)};
    const auto rendered = mirador::render_set_of_mark(background.view, snapshot);
    ASSERT_TRUE(rendered.ok());

    const ImageView view = rendered.value().image.view();
    EXPECT_EQ(view.format, PixelFormat::kRgb8);
    EXPECT_EQ(view.rotation, Rotation::k0);
    EXPECT_EQ(view.width, 9);
    EXPECT_EQ(view.height, 7);
    EXPECT_EQ(view.row_stride_bytes, 27);
    EXPECT_EQ(view.data, rendered.value().image.pixels.data());

    // An empty image projects to an invalid view.
    mirador::MarkedImage empty;
    EXPECT_EQ(empty.view().data, nullptr);
}

}  // namespace
