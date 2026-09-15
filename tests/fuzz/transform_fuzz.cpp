// M5-07 fuzz entry point (design section 23, SCOPE-08): adversarial inputs into
// the coordinate transform surface. Two full 3x3 double matrices (raw bit
// patterns included: NaN, infinities, denormals, degenerate rows), out-of-range
// from/to space ids and bit-pattern points/rects/segments are driven through
// every mapping entry point, compose, inverse and the Result-returning
// factories. Non-finite mapping output is legal (bounds are the caller's
// concern); crashes, sanitizer reports and broken error contracts are findings.
//
// Invariants asserted on every input (a break aborts, a libFuzzer finding):
//   (a) compose: unchained spaces (first.to != second.from) must fail with
//       kCoordinateTransform; chained spaces must succeed and produce
//       from = first.from, to = second.to;
//   (b) inverse: only kOk or kCoordinateTransform; on success from/to swap;
//   (c) inverse roundtrip transform_point(inverse(t), transform_point(t, p))
//       restores finite p: asserted with the 1e-6 relative tolerance whenever
//       the per-sample floating-point error budget admits it (ill-conditioned
//       matrices escape to the computed error bound instead of producing false
//       findings; genuinely broken inverse formulas still fail the bound);
//   (d) make_letterbox: non-positive sizes must fail with kInvalidArgument,
//       positive sizes must succeed;
//   (e) transform_segment carries the confidence through unchanged;
//   (f) oriented_size swaps dimensions for k90/k270.

#include <mirador/transform.hpp>

#include <mirador/geometry.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

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

/// Mostly moderate values in [-2048, 2048] (so roundtrip assertions get real
/// coverage), with a 1/256 chance of raw bit patterns (NaN/inf/denormal) to
/// exercise the non-finite paths through the mapping entry points.
double next_matrix_double(ByteCursor& cursor) noexcept {
    const auto raw = static_cast<uint16_t>(next_byte(cursor) | (static_cast<uint16_t>(next_byte(cursor)) << 8));
    if ((raw & 0xFFU) == 0xFFU) {
        const auto bits = (static_cast<uint64_t>(next_u32(cursor)) << 32) | next_u32(cursor);
        double value = 0.0;
        static_assert(sizeof(value) == sizeof(bits), "64-bit IEEE 754 double expected");
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return static_cast<double>(static_cast<int16_t>(raw)) / 16.0;
}

float next_f32(ByteCursor& cursor) noexcept {
    const uint32_t bits = next_u32(cursor);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits), "32-bit IEEE 754 float expected");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

mirador::PointF next_point(ByteCursor& cursor) noexcept {
    return mirador::PointF{next_f32(cursor), next_f32(cursor)};
}

/// Bit-pattern equality with a NaN exception: copying a signaling NaN through
/// FP registers may quiet it on x86, so payload-identical NaNs compare equal
/// here while `==` (false for every NaN) would fire falsely.
bool confidence_carried(float original, float carried) noexcept {
    if (std::isnan(original) && std::isnan(carried)) {
        return true;
    }
    uint32_t original_bits = 0;
    uint32_t carried_bits = 0;
    std::memcpy(&original_bits, &original, sizeof(original_bits));
    std::memcpy(&carried_bits, &carried, sizeof(carried_bits));
    return original_bits == carried_bits;
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

/// (a) compose: the space chaining contract.
void check_compose(const mirador::Transform2D& first, const mirador::Transform2D& second) noexcept {
    const auto chained = mirador::compose(first, second);
    const long detail = chained.ok() ? 0 : static_cast<long>(chained.status().code());
    if (first.to != second.from) {
        check(!chained.ok() && chained.status().code() == mirador::ErrorCode::kCoordinateTransform,
              "compose with unchained spaces must fail with kCoordinateTransform", detail);
        return;
    }
    check(chained.ok(), "compose with chained spaces must succeed", detail);
    if (chained.ok()) {
        check(chained.value().from == first.from && chained.value().to == second.to,
              "compose must produce from = first.from, to = second.to", detail);
    }
}

/// (c) one roundtrip sample. The caller gates on the determinant (finite and
/// >= 1e-6); within the gate a sample is skipped only when its numerics are
/// genuinely undefined: float overflow of the mapped/restored point, or an
/// error budget pushed out of double range by extreme raw-bit matrix entries.
constexpr double kDoubleEps = 2.220446049250313e-16;
constexpr double kFloatEps = 1.1920928955078125e-7;

void check_roundtrip_sample(const mirador::Transform2D& transform, const mirador::Transform2D& inverted,
                            double condition, double inverse_norm, ByteCursor& cursor) noexcept {
    const mirador::PointF p = next_point(cursor);
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
        return;
    }
    const mirador::PointF mapped = mirador::transform_point(transform, p);
    if (!std::isfinite(mapped.x) || !std::isfinite(mapped.y)) {
        return;  // overflow through float storage: roundtrip undefined
    }
    const mirador::PointF restored = mirador::transform_point(inverted, mapped);
    if (!std::isfinite(restored.x) || !std::isfinite(restored.y)) {
        return;
    }
    const double mapped_magnitude =
        std::max({1.0, std::fabs(static_cast<double>(mapped.x)), std::fabs(static_cast<double>(mapped.y))});
    const double p_magnitude =
        std::max({1.0, std::fabs(static_cast<double>(p.x)), std::fabs(static_cast<double>(p.y))});
    // Contract tolerance: 1e-6 relative to p, widened by the sample's own
    // floating-point budget (condition amplification of the double rounding
    // plus float storage of the mapped point). Raw-bit matrix entries can push
    // the budget beyond double range even for a finite, non-singular
    // determinant; such a sample is numerically undefined and is skipped.
    const double tolerance =
        std::max(1e-6 * p_magnitude, 32.0 * kDoubleEps * condition * p_magnitude +
                                         8.0 * kFloatEps * (inverse_norm * mapped_magnitude + p_magnitude));
    if (!std::isfinite(tolerance)) {
        return;
    }
    const double error_x = std::fabs(static_cast<double>(restored.x) - static_cast<double>(p.x));
    const double error_y = std::fabs(static_cast<double>(restored.y) - static_cast<double>(p.y));
    check(error_x <= tolerance && error_y <= tolerance, "inverse roundtrip exceeded tolerance",
          static_cast<long>(std::min(error_x * 1e9, 1e12)));
}

/// (b)/(c) inverse: error contract, from/to swap, finite-entries guarantee and
/// the guarded roundtrip.
///
/// Roundtrip gate policy (measured, M5-07 re-verification): the product now
/// rejects every ok-inverse hazard the earlier fuzz pass found (det = +-inf,
/// entry overflow under a finite det), so ok implies a finite determinant and
/// finite inverse entries. That still does not imply 1e-6-relative roundtrip
/// precision: for {2048, 2047.9375, 0.0625, 0.0625} (det = 1/256, finite,
/// inverse ok) the measured roundtrip error at p = (0.1, 0.3) is 1.95e-4 --
/// legitimate conditioning, not a bug. The gate therefore skips only
/// determinants that are non-finite or below 1e-6 (conditioning beyond ~1e18
/// has no meaningful bound at all); above the gate the adaptive per-sample
/// budget decides, and only numerically undefined samples (non-finite budget,
/// float overflow of the mapped/restored point) are exempted.
void check_inverse(const mirador::Transform2D& first, ByteCursor& cursor) noexcept {
    const auto inverted = mirador::inverse(first);
    const long detail = inverted.ok() ? 0 : static_cast<long>(inverted.status().code());
    check(inverted.ok() || inverted.status().code() == mirador::ErrorCode::kCoordinateTransform,
          "inverse must return ok or kCoordinateTransform", detail);
    if (!inverted.ok()) {
        return;
    }
    check(inverted.value().from == first.to && inverted.value().to == first.from, "inverse must swap from/to", detail);
    // Locked by tests/core/transform_test.cpp: an ok inverse is fully usable,
    // i.e. every matrix entry is finite (rejects det = +-inf and entry
    // overflow under a finite determinant).
    for (const double entry : inverted.value().matrix) {
        const long detail = std::isfinite(entry) ? 0 : 1;  // no double->long cast before finiteness is proven
        check(std::isfinite(entry), "ok inverse must carry finite entries", detail);
    }

    const std::array<double, 9>& m = first.matrix;
    const double det = m[0] * m[4] - m[1] * m[3];
    double entry_scale = 0.0;
    for (int i = 0; i < 6; ++i) {  // the affine 2x3 block drives point mapping
        entry_scale = std::max(entry_scale, std::fabs(m[static_cast<size_t>(i)]));
    }
    // Per-sample error budget (in |p| units): double rounding amplified by the
    // condition estimate entry_scale^2/|det|, plus the two float roundings of
    // the mapped point amplified by |A^-1| ~ entry_scale/|det|.
    const double det_abs = std::fabs(det);
    const double condition = entry_scale * entry_scale / det_abs;
    const double inverse_norm = 2.0 * entry_scale / det_abs;
    const bool roundtrip_defined = std::isfinite(det_abs) && det_abs >= 1e-6;
    for (int sample = 0; sample < 4; ++sample) {
        if (roundtrip_defined) {
            check_roundtrip_sample(first, inverted.value(), condition, inverse_norm, cursor);
        } else {
            (void)next_point(cursor);  // keep the byte stream aligned
        }
    }
}

/// (d)/(f) factories: make_letterbox parameter domain, make_rotation over the
/// legal rotation domain, oriented_size dimension swap.
void check_factories(ByteCursor& cursor) noexcept {
    const auto from = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    const auto to = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    const int32_t src_width = next_i32(cursor);
    const int32_t src_height = next_i32(cursor);
    const int32_t dst_width = next_i32(cursor);
    const int32_t dst_height = next_i32(cursor);
    const auto letterbox = mirador::make_letterbox(src_width, src_height, dst_width, dst_height, from, to);
    const bool sizes_legal = src_width > 0 && src_height > 0 && dst_width > 0 && dst_height > 0;
    if (!sizes_legal) {
        check(!letterbox.ok() && letterbox.status().code() == mirador::ErrorCode::kInvalidArgument,
              "make_letterbox must reject non-positive sizes with kInvalidArgument",
              letterbox.ok() ? 0 : static_cast<long>(letterbox.status().code()));
    } else {
        check(letterbox.ok(), "make_letterbox must accept positive sizes", 0);
    }

    // make_rotation has no error channel; undefined rotation values are a
    // caller contract violation, so only the defined domain is generated.
    constexpr std::array<mirador::Rotation, 4> kRotations{mirador::Rotation::k0, mirador::Rotation::k90,
                                                          mirador::Rotation::k180, mirador::Rotation::k270};
    const mirador::Rotation rotation = kRotations[next_byte(cursor) % 4U];
    const int32_t raw_width = 1 + static_cast<int32_t>(next_byte(cursor) % 255U);
    const int32_t raw_height = 1 + static_cast<int32_t>(next_byte(cursor) % 255U);
    const mirador::Transform2D rotated = mirador::make_rotation(rotation, raw_width, raw_height, from, to);
    const auto [oriented_width, oriented_height] = mirador::oriented_size(rotation, raw_width, raw_height);
    const bool swapped = rotation == mirador::Rotation::k90 || rotation == mirador::Rotation::k270;
    check(oriented_width == (swapped ? raw_height : raw_width) && oriented_height == (swapped ? raw_width : raw_height),
          "oriented_size must swap dimensions for k90/k270", static_cast<long>(oriented_width));
    (void)rotated;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) noexcept {
    if (size < 16) {
        return 0;
    }
    ByteCursor cursor{data, size, 0};

    mirador::Transform2D first;
    mirador::Transform2D second;
    for (int i = 0; i < 9; ++i) {
        first.matrix[static_cast<size_t>(i)] = next_matrix_double(cursor);
        second.matrix[static_cast<size_t>(i)] = next_matrix_double(cursor);
    }
    // Out-of-range space ids are constructible by callers and must not break
    // the transform algebra (spaces only ever compare or swap).
    const auto first_from = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    const auto first_to = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    const auto second_from = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    const auto second_to = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    first.from = first_from;
    first.to = first_to;
    second.from = second_from;
    second.to = second_to;

    // Mapping entry points: no crash, no sanitizer report (output may be any
    // value, including non-finite).
    (void)mirador::transform_point(first, next_point(cursor));
    (void)mirador::transform_rect(
        first, mirador::RectF{next_f32(cursor), next_f32(cursor), next_f32(cursor), next_f32(cursor)});
    const std::array<mirador::PointF, 4> points{next_point(cursor), next_point(cursor), next_point(cursor),
                                                next_point(cursor)};
    const std::vector<mirador::PointF> mapped_points = mirador::transform_points(first, points);
    check(mapped_points.size() == points.size(), "transform_points must preserve the point count",
          static_cast<long>(mapped_points.size()));
    const mirador::LineSegment segment{next_point(cursor), next_point(cursor), next_f32(cursor)};
    const mirador::LineSegment mapped_segment = mirador::transform_segment(first, segment);
    check(confidence_carried(segment.confidence, mapped_segment.confidence),
          "transform_segment must carry confidence unchanged", 0);

    check_compose(first, second);
    check_inverse(first, cursor);
    check_factories(cursor);

    return 0;
}
