#include <mirador/set_of_mark.hpp>

#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirador {
namespace {

constexpr int32_t kMaxOutputDimension = 32767;

// Fixed high-visibility palette (design section 17: renderer-owned colors);
// a region's color index is stable_id % 8 so identities keep their color.
constexpr uint8_t kPalette[][3] = {
    {230, 57, 70},    // red
    {42, 157, 143},   // teal
    {69, 123, 157},   // blue
    {233, 196, 106},  // yellow
    {231, 111, 81},   // orange
    {156, 76, 159},   // purple
    {42, 109, 62},    // green
    {188, 108, 37},   // brown
};
constexpr int kPaletteSize = 8;

// 3x5 pixel digit glyphs, rows top to bottom, 3 bits per row (MSB left).
constexpr uint8_t kDigitGlyphs[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111},  // 0
    {0b010, 0b110, 0b010, 0b010, 0b111},  // 1
    {0b111, 0b001, 0b111, 0b100, 0b111},  // 2
    {0b111, 0b001, 0b111, 0b001, 0b111},  // 3
    {0b101, 0b101, 0b111, 0b001, 0b001},  // 4
    {0b111, 0b100, 0b111, 0b001, 0b111},  // 5
    {0b111, 0b100, 0b111, 0b101, 0b111},  // 6
    {0b111, 0b001, 0b010, 0b010, 0b010},  // 7
    {0b111, 0b101, 0b111, 0b101, 0b111},  // 8
    {0b111, 0b101, 0b111, 0b001, 0b111},  // 9
};

Status context_status(const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "render_set_of_mark cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "render_set_of_mark deadline exceeded"};
    }
    return {};
}

Status validate_options(const SoMRenderOptions& options) noexcept {
    if (options.max_bytes <= 0) {
        return {ErrorCode::kInvalidArgument, "max_bytes must be positive"};
    }
    if (options.box_thickness < 1 || options.box_thickness > 16) {
        return {ErrorCode::kInvalidArgument, "box_thickness must be in [1, 16]"};
    }
    if (options.label_height < 0 || options.label_height > 64) {
        return {ErrorCode::kInvalidArgument, "label_height must be in [0, 64]"};
    }
    if (options.max_marks <= 0) {
        return {ErrorCode::kInvalidArgument, "max_marks must be positive"};
    }
    return {};
}

bool is_single_plane_8bit(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::kGray8:
        case PixelFormat::kRgb8:
        case PixelFormat::kBgr8:
        case PixelFormat::kRgba8:
        case PixelFormat::kBgra8:
            return true;
        case PixelFormat::kNv12:
            return false;
    }
    return false;
}

struct Rgb {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

Rgb pixel_at(const ImageView& view, int32_t x, int32_t y) noexcept {
    const uint8_t* row = reinterpret_cast<const uint8_t*>(view.data) + static_cast<int64_t>(y) * view.row_stride_bytes;
    const uint8_t* pixel = row + static_cast<int64_t>(x) * bytes_per_pixel(view.format);
    switch (view.format) {
        case PixelFormat::kGray8:
            return Rgb{*pixel, *pixel, *pixel};
        case PixelFormat::kRgb8:
            return Rgb{pixel[0], pixel[1], pixel[2]};
        case PixelFormat::kBgr8:
            return Rgb{pixel[2], pixel[1], pixel[0]};
        case PixelFormat::kRgba8:
            return Rgb{pixel[0], pixel[1], pixel[2]};
        case PixelFormat::kBgra8:
            return Rgb{pixel[2], pixel[1], pixel[0]};
        case PixelFormat::kNv12:
            break;
    }
    return Rgb{};
}

void set_pixel(MarkedImage& image, int32_t x, int32_t y, Rgb color) noexcept {
    if (x < 0 || x >= image.width || y < 0 || y >= image.height) {
        return;
    }
    uint8_t* row = reinterpret_cast<uint8_t*>(image.pixels.data()) + static_cast<int64_t>(y) * image.width * 3;
    row[static_cast<int64_t>(x) * 3] = color.red;
    row[static_cast<int64_t>(x) * 3 + 1] = color.green;
    row[static_cast<int64_t>(x) * 3 + 2] = color.blue;
}

void fill_rect(MarkedImage& image, const RectI& rect, Rgb color) noexcept {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            set_pixel(image, x, y, color);
        }
    }
}

void draw_outline(MarkedImage& image, const RectI& rect, int32_t thickness, Rgb color) noexcept {
    const RectI top{rect.x, rect.y, rect.width, thickness};
    const RectI bottom{rect.x, rect.y + rect.height - thickness, rect.width, thickness};
    const RectI left{rect.x, rect.y, thickness, rect.height};
    const RectI right{rect.x + rect.width - thickness, rect.y, thickness, rect.height};
    fill_rect(image, top, color);
    fill_rect(image, bottom, color);
    fill_rect(image, left, color);
    fill_rect(image, right, color);
}

void draw_digit(MarkedImage& image, int32_t x, int32_t y, int32_t scale, int32_t digit, Rgb color) noexcept {
    for (int32_t row = 0; row < 5; ++row) {
        for (int32_t column = 0; column < 3; ++column) {
            if ((kDigitGlyphs[digit][row] >> (2 - column)) & 0x1) {
                fill_rect(image, RectI{x + column * scale, y + row * scale, scale, scale}, color);
            }
        }
    }
}

int32_t decimal_digit_count(uint32_t value) noexcept {
    int32_t count = 1;
    while (value >= 10) {
        value /= 10;
        ++count;
    }
    return count;
}

void draw_number(MarkedImage& image, const RectI& chip, uint32_t value, int32_t scale, Rgb color) noexcept {
    const int32_t digit_width = 3 * scale;
    const int32_t count = decimal_digit_count(value);
    int32_t x = chip.x + (chip.width - count * digit_width) / 2;
    const int32_t y = chip.y + (chip.height - 5 * scale) / 2;
    for (int32_t place = count - 1; place >= 0; --place) {
        uint32_t divisor = 1;
        for (int32_t index = 0; index < place; ++index) {
            divisor *= 10;
        }
        draw_digit(image, x, y, scale, static_cast<int32_t>((value / divisor) % 10), color);
        x += digit_width;
    }
}

bool rects_overlap(const RectI& a, const RectI& b) noexcept {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

bool inside_image(const RectI& rect, int32_t width, int32_t height) noexcept {
    return rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= width && rect.y + rect.height <= height;
}

RectI intersect_rects(const RectI& a, const RectI& b) noexcept {
    const int32_t x0 = std::max(a.x, b.x);
    const int32_t y0 = std::max(a.y, b.y);
    const int32_t x1 = std::min(a.x + a.width, b.x + b.width);
    const int32_t y1 = std::min(a.y + a.height, b.y + b.height);
    return RectI{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

/// Saturating float -> int32 for region coordinates far outside the image.
int32_t to_int32_clamped(double value) noexcept {
    const double clamped = std::clamp(value, -2.147483648e9, 2.147483647e9);
    return static_cast<int32_t>(std::lround(clamped));
}

Rgb palette_color(uint64_t stable_id) noexcept {
    const uint8_t* entry = kPalette[stable_id % kPaletteSize];
    return Rgb{entry[0], entry[1], entry[2]};
}

/// Copies the background into the packed RGB8 output, converting formats.
void copy_background(MarkedImage& output, const ImageView& background) noexcept {
    for (int32_t y = 0; y < background.height; ++y) {
        for (int32_t x = 0; x < background.width; ++x) {
            set_pixel(output, x, y, pixel_at(background, x, y));
        }
    }
}

/// Label placement candidates in priority order (design section 17: label
/// placement with occlusion avoidance): above, below, inside top, inside
/// bottom — all left-aligned to the box.
std::vector<RectI> label_candidates(const RectI& box, const RectI& chip) noexcept {
    constexpr int32_t kGap = 1;
    return {
        RectI{box.x, box.y - chip.height - kGap, chip.width, chip.height},
        RectI{box.x, box.y + box.height + kGap, chip.width, chip.height},
        RectI{box.x, box.y, chip.width, chip.height},
        RectI{box.x, box.y + box.height - chip.height, chip.width, chip.height},
    };
}

}  // namespace

ImageView MarkedImage::view() const noexcept {
    if (pixels.empty() || width <= 0 || height <= 0) {
        return ImageView{};
    }
    ImageView view;
    view.data = pixels.data();
    view.width = width;
    view.height = height;
    view.row_stride_bytes = static_cast<int64_t>(width) * 3;
    view.format = PixelFormat::kRgb8;
    view.rotation = Rotation::k0;
    return view;
}

Result<SetOfMarkResult> render_set_of_mark(const ImageView& background, const SemanticSnapshot& snapshot,
                                           const SoMRenderOptions& options, const ExecutionContext& context) noexcept {
    try {
        if (const Status stage = context_status(context); !stage.ok()) {
            return stage;
        }
        if (const Status valid = validate_options(options); !valid.ok()) {
            return valid;
        }
        if (const Result<void> view_valid = validate(background); !view_valid.ok()) {
            return view_valid.status();
        }
        if (background.rotation != Rotation::k0) {
            return Status(ErrorCode::kInvalidArgument, "background must be a rotation-k0 view (M4 render contract)");
        }
        if (!is_single_plane_8bit(background.format)) {
            return Status(ErrorCode::kUnsupportedFormat, "background format is not a single-plane 8-bit format");
        }
        if (static_cast<int64_t>(snapshot.regions.size()) > static_cast<int64_t>(options.max_marks)) {
            return Status(ErrorCode::kBudgetExceeded, "region count exceeds max_marks");
        }

        SetOfMarkResult result;
        result.generation = snapshot.generation;
        result.frame_sequence = snapshot.frame_sequence;
        result.marks.reserve(snapshot.regions.size());

        MarkedImage& output = result.image;
        output.width = background.width;
        output.height = background.height;
        const int64_t output_bytes = static_cast<int64_t>(background.width) * background.height * 3;
        if (output_bytes > options.max_bytes || background.width > kMaxOutputDimension ||
            background.height > kMaxOutputDimension) {
            return Status(ErrorCode::kBudgetExceeded, "marked image exceeds max_bytes");
        }
        output.pixels.assign(static_cast<size_t>(output_bytes), std::byte{0});
        copy_background(output, background);

        const RectI image_rect{0, 0, background.width, background.height};
        std::vector<RectI> placed_labels;
        for (size_t index = 0; index < snapshot.regions.size(); ++index) {
            if ((index % 64) == 0) {
                if (const Status stage = context_status(context); !stage.ok()) {
                    return stage;
                }
            }
            const VisualRegion& region = snapshot.regions[index];
            SoMMark mark;
            mark.mark_id = static_cast<uint32_t>(index + 1);
            mark.stable_id = region.stable_id;
            mark.anchor = region.anchor;

            const RectI clipped = intersect_rects(RectI{to_int32_clamped(static_cast<double>(region.bounds.x)),
                                                        to_int32_clamped(static_cast<double>(region.bounds.y)),
                                                        to_int32_clamped(static_cast<double>(region.bounds.width)),
                                                        to_int32_clamped(static_cast<double>(region.bounds.height))},
                                                  image_rect);
            if (clipped.width > 0 && clipped.height > 0) {
                const Rgb color = palette_color(region.stable_id);
                draw_outline(output, clipped, std::min(options.box_thickness, std::min(clipped.width, clipped.height)),
                             color);
                mark.drawn_bounds = clipped;

                if (options.label_height > 0) {
                    const int32_t scale = std::max(1, options.label_height / 6);
                    const int32_t digits = decimal_digit_count(mark.mark_id);
                    const RectI chip{0, 0, digits * 3 * scale + 2, 5 * scale + 2};
                    for (const RectI& candidate : label_candidates(clipped, chip)) {
                        if (!inside_image(candidate, output.width, output.height)) {
                            continue;
                        }
                        const bool occluded =
                            std::any_of(placed_labels.begin(), placed_labels.end(),
                                        [candidate](const RectI& placed) { return rects_overlap(candidate, placed); });
                        if (occluded) {
                            continue;
                        }
                        fill_rect(output, candidate, color);
                        draw_number(output, candidate, mark.mark_id, scale, Rgb{255, 255, 255});
                        placed_labels.push_back(candidate);
                        break;
                    }
                }
            }
            result.marks.push_back(mark);
        }
        return result;
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "render_set_of_mark: internal allocation failed");
    }
}

}  // namespace mirador
