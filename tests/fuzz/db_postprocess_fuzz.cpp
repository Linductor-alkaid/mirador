// M5-07 fuzz entry point (design section 23, SCOPE-08): adversarial inputs into
// the DB probability-map post-processing and contour box recovery surfaces
// (DEC-14 reference adaptation). The harness derives the whole map and every
// parameter byte-deterministically, so libFuzzer coverage guidance steers both
// the component-layout search and the parameter-validation search.
//
// Invariants asserted on every input (a break aborts, which libFuzzer reports
// as a crash finding):
//   (a) the input bytes and the map buffer are never modified;
//   (b) on success, every emitted box is finite, has non-negative size and
//       stays inside the map rectangle (unclip is contractually clamped to the
//       map bounds; contour boxes are exact pixel bounds);
//   (c) every error is one of {kInvalidArgument, kUnsupportedFormat,
//       kBudgetExceeded} and never a crash;
//   (d) DB reference output carries no text and no polygon (DEC-014 scope).

#include <mirador/text_postprocess.hpp>

#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

/// Byte-cursor with wraparound: every input of length >= 16 yields a full,
/// deterministic parameter set, and flipping any input byte stays observable.
struct ByteCursor {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

uint8_t next_byte(ByteCursor& cursor) noexcept {
    const uint8_t value = cursor.data[cursor.pos];
    cursor.pos = (cursor.pos + 1) % cursor.size;
    return value;
}

uint32_t next_u32(ByteCursor& cursor) noexcept {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(next_byte(cursor)) << (8 * i);
    }
    return value;
}

int32_t next_i32(ByteCursor& cursor) noexcept {
    return static_cast<int32_t>(next_u32(cursor));
}

int64_t next_i64(ByteCursor& cursor) noexcept {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(next_byte(cursor)) << (8 * i);
    }
    return static_cast<int64_t>(value);
}

double next_f64(ByteCursor& cursor) noexcept {
    const auto bits = static_cast<uint64_t>(next_i64(cursor));
    double value = 0.0;
    static_assert(sizeof(value) == sizeof(bits), "64-bit IEEE 754 double expected");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64_t fnv1a(const void* bytes, size_t size) noexcept {
    const auto* p = static_cast<const uint8_t*>(bytes);
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}

[[noreturn]] void fuzz_fail(const char* what, long detail) noexcept {
    std::fprintf(stderr, "FUZZ INVARIANT BROKEN: %s (%ld)\n", what, detail);
    std::fflush(stderr);
    std::abort();
}

void check(bool condition, const char* what, long detail = 0) noexcept {
    if (!condition) {
        fuzz_fail(what, detail);
    }
}

constexpr double kInMapTolerance = 1e-3;  // float rounding headroom for clamped edges

void check_db_region(const mirador::TextRegion& region, int32_t width, int32_t height, size_t index) noexcept {
    const mirador::RectF& bounds = region.bounds;
    check(std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width) &&
              std::isfinite(bounds.height),
          "DB box coordinates must be finite", static_cast<long>(index));
    check(bounds.width >= 0.0F && bounds.height >= 0.0F, "DB box size must be non-negative", static_cast<long>(index));
    check(bounds.x >= -kInMapTolerance && bounds.y >= -kInMapTolerance, "DB box must start inside the map",
          static_cast<long>(index));
    check(static_cast<double>(bounds.x) + bounds.width <= static_cast<double>(width) + kInMapTolerance &&
              static_cast<double>(bounds.y) + bounds.height <= static_cast<double>(height) + kInMapTolerance,
          "DB box must stay inside the map (unclip clamped)", static_cast<long>(index));
    check(std::isfinite(region.confidence) && region.confidence >= 0.0F && region.confidence <= 1.0F,
          "DB confidence must be a finite [0,1] score", static_cast<long>(index));
    check(region.utf8_text.empty() && region.polygon.empty(), "DB reference output has no text/polygon",
          static_cast<long>(index));
}

void check_contour_box(const mirador::RectF& box, int32_t width, int32_t height, size_t index) noexcept {
    check(std::isfinite(box.x) && std::isfinite(box.y) && std::isfinite(box.width) && std::isfinite(box.height),
          "contour box coordinates must be finite", static_cast<long>(index));
    check(box.width >= 0.0F && box.height >= 0.0F, "contour box size must be non-negative", static_cast<long>(index));
    check(box.x >= -kInMapTolerance && box.y >= -kInMapTolerance, "contour box must start inside the map",
          static_cast<long>(index));
    check(static_cast<double>(box.x) + box.width <= static_cast<double>(width) + kInMapTolerance &&
              static_cast<double>(box.y) + box.height <= static_cast<double>(height) + kInMapTolerance,
          "contour box must stay inside the map", static_cast<long>(index));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) noexcept {
    if (size < 16) {
        return 0;
    }
    ByteCursor cursor{data, size, 0};

    // Map geometry: [1, 64] x [1, 64], tight stride (bounds the runtime).
    const int32_t width = 1 + static_cast<int32_t>(next_byte(cursor) % 64U);
    const int32_t height = 1 + static_cast<int32_t>(next_byte(cursor) % 64U);

    // Format: kGray8 for ~15/16 of the inputs (the supported path); the rest
    // exercise the format gate with other defined single-plane formats.
    const uint8_t format_selector = next_byte(cursor);
    const mirador::PixelFormat format =
        format_selector < 15U ? mirador::PixelFormat::kGray8 : static_cast<mirador::PixelFormat>(format_selector % 5U);
    const int32_t bpp = mirador::bytes_per_pixel(format) > 0 ? mirador::bytes_per_pixel(format) : 1;
    const int64_t stride = static_cast<int64_t>(width) * bpp;

    std::vector<uint8_t> pixels(static_cast<size_t>(height) * static_cast<size_t>(stride), 0);
    // Gray8 uses a tight plane; wider formats read the same bytes through the
    // larger stride, so the fill below covers both layouts.
    for (auto& pixel : pixels) {
        pixel = next_byte(cursor);
    }

    mirador::ImageView view;
    view.data = reinterpret_cast<const std::byte*>(pixels.data());
    view.width = width;
    view.height = height;
    view.row_stride_bytes = stride;
    view.format = format;

    const uint64_t input_hash_before = fnv1a(data, size);
    const uint64_t pixels_hash_before = fnv1a(pixels.data(), pixels.size());

    // DB post-processing: legal and out-of-range values on every field.
    mirador::DbPostprocessParams db_params;
    db_params.binarize_threshold = next_byte(cursor);
    db_params.unclip_ratio = next_f64(cursor);
    db_params.min_box_pixels = next_i32(cursor);
    db_params.min_mean_score = next_f64(cursor);
    db_params.max_boxes = next_i32(cursor);
    db_params.work_budget_bytes = next_i64(cursor);

    const auto db_result = mirador::db_postprocess_aabb(view, db_params);
    if (db_result.ok()) {
        for (size_t i = 0; i < db_result.value().size(); ++i) {
            check_db_region(db_result.value()[i], width, height, i);
        }
    } else {
        const mirador::ErrorCode code = db_result.status().code();
        check(code == mirador::ErrorCode::kInvalidArgument || code == mirador::ErrorCode::kUnsupportedFormat ||
                  code == mirador::ErrorCode::kBudgetExceeded,
              "DB postprocess error code out of contract", static_cast<long>(code));
    }
    check(fnv1a(data, size) == input_hash_before, "input bytes must not be modified", 0);
    check(fnv1a(pixels.data(), pixels.size()) == pixels_hash_before, "map bytes must not be modified", 0);

    // Contour box recovery over the same map and fresh derived parameters.
    mirador::ContourBoxParams contour_params;
    contour_params.foreground_threshold = next_byte(cursor);
    contour_params.min_box_pixels = next_i32(cursor);
    contour_params.max_boxes = next_i32(cursor);
    contour_params.work_budget_bytes = next_i64(cursor);

    const auto contour_result = mirador::recover_contour_boxes(view, contour_params);
    if (contour_result.ok()) {
        for (size_t i = 0; i < contour_result.value().size(); ++i) {
            check_contour_box(contour_result.value()[i], width, height, i);
        }
    } else {
        const mirador::ErrorCode code = contour_result.status().code();
        check(code == mirador::ErrorCode::kInvalidArgument || code == mirador::ErrorCode::kUnsupportedFormat ||
                  code == mirador::ErrorCode::kBudgetExceeded,
              "contour recovery error code out of contract", static_cast<long>(code));
    }
    check(fnv1a(data, size) == input_hash_before, "input bytes must not be modified (contour pass)", 0);
    check(fnv1a(pixels.data(), pixels.size()) == pixels_hash_before, "map bytes must not be modified (contour pass)",
          0);

    return 0;
}
