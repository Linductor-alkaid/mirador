// Independent verification suite for the M7-04 global shift estimation
// primitive (include/mirador/shift_estimation.hpp). Covers the frozen contract:
// known-shift recovery, the frozen confidence/precision rules, the winner
// total order, bit-identical determinism (view and signature overloads), the
// DOD-03 coordinate matrix (rotation metadata x odd sizes x non-tight stride),
// budget protection (RULE-06), explicit error conversion (RULE-08: parameter
// validation, kCancelled/kTimeout) and the privacy default (RULE-10: no files
// written). Pure function boundaries (no tracker state, no change
// classification, M7-07 ownership) are structural and stay with the
// architecture tests.

#include <mirador/shift_estimation.hpp>

#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using mirador::ChangeDetectionParams;
using mirador::ChangeSignature;
using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::estimate_global_shift;
using mirador::ExecutionContext;
using mirador::ImageBuffer;
using mirador::ImageView;
using mirador::make_change_signature;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::Result;
using mirador::Rotation;
using mirador::ShiftEstimate;
using mirador::ShiftEstimationParams;

constexpr int64_t kBudget = int64_t{512} * 1024;

/// Test-owned gray image with an explicit (possibly padded) row stride.
struct GrayImage {
    std::vector<std::byte> bytes;
    ImageView view;
};

GrayImage make_gray(int32_t width, int32_t height, int64_t stride, uint8_t fill) {
    GrayImage image;
    image.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{0});
    for (int32_t y = 0; y < height; ++y) {
        std::byte* row = image.bytes.data() + static_cast<int64_t>(y) * stride;
        std::fill(row, row + width, static_cast<std::byte>(fill));
    }
    image.view.data = image.bytes.data();
    image.view.width = width;
    image.view.height = height;
    image.view.row_stride_bytes = stride;
    image.view.format = PixelFormat::kGray8;
    return image;
}

std::byte& pixel(GrayImage& image, int32_t x, int32_t y) {
    return image
        .bytes[static_cast<size_t>(y) * static_cast<size_t>(image.view.row_stride_bytes) + static_cast<size_t>(x)];
}

const std::byte& pixel(const GrayImage& image, int32_t x, int32_t y) {
    return image
        .bytes[static_cast<size_t>(y) * static_cast<size_t>(image.view.row_stride_bytes) + static_cast<size_t>(x)];
}

/// Fixed-seed LCG byte stream: test inputs stay deterministic without any
/// library randomness.
class DeterministicRandom {
public:
    explicit DeterministicRandom(uint32_t seed) : state_(seed) {}

    uint8_t next_byte() {
        state_ = state_ * 1664525U + 1013904223U;
        return static_cast<uint8_t>((state_ >> 16) & 0xFFU);
    }

private:
    uint32_t state_;
};

void fill_random(GrayImage& image, uint32_t seed) {
    DeterministicRandom random(seed);
    for (int32_t y = 0; y < image.view.height; ++y) {
        for (int32_t x = 0; x < image.view.width; ++x) {
            pixel(image, x, y) = static_cast<std::byte>(random.next_byte());
        }
    }
}

/// Current(x, y) = previous(x - dx, y - dy): the content moves by (+dx, +dy).
/// Out-of-source pixels take `fill` (border artifacts cannot reach the central
/// comparison window of the searches used below).
GrayImage shifted_copy(const GrayImage& source, int32_t dx, int32_t dy, uint8_t fill) {
    GrayImage image = make_gray(source.view.width, source.view.height, source.view.row_stride_bytes, fill);
    for (int32_t y = 0; y < image.view.height; ++y) {
        for (int32_t x = 0; x < image.view.width; ++x) {
            const int32_t sx = x - dx;
            const int32_t sy = y - dy;
            if (sx >= 0 && sx < source.view.width && sy >= 0 && sy < source.view.height) {
                pixel(image, x, y) = pixel(source, sx, sy);
            }
        }
    }
    return image;
}

/// Bit-identical comparison of two estimates (the frozen determinism claim):
/// every field compared with exact equality, so the records must carry the
/// very same bits.
void expect_bit_identical(const ShiftEstimate& first, const ShiftEstimate& second, const char* what) {
    EXPECT_EQ(first.thumbnail_dx, second.thumbnail_dx) << what;
    EXPECT_EQ(first.thumbnail_dy, second.thumbnail_dy) << what;
    EXPECT_EQ(first.dx, second.dx) << what;
    EXPECT_EQ(first.dy, second.dy) << what;
    EXPECT_EQ(first.confidence, second.confidence) << what;
}

/// Expects a kInvalidArgument result (guarding the status() precondition).
void expect_invalid(const Result<ShiftEstimate>& result, const char* what) {
    if (result.ok()) {
        ADD_FAILURE() << what << ": expected kInvalidArgument, got an estimate";
        return;
    }
    EXPECT_EQ(result.status().code(), ErrorCode::kInvalidArgument) << what;
}

/// Expects a successful estimate and returns it.
ShiftEstimate expect_ok(const Result<ShiftEstimate>& result, const char* what) {
    EXPECT_TRUE(result.ok()) << what << ": " << (result.ok() ? "" : result.status().message());
    return result.ok() ? result.value() : ShiftEstimate{};
}

ChangeSignature signature_of(const GrayImage& frame, int32_t thumbnail_size) {
    ChangeDetectionParams params;
    params.thumbnail_size = thumbnail_size;
    auto signature = make_change_signature(frame.view, params);
    EXPECT_TRUE(signature.ok());
    return signature.ok() ? signature.take_value() : ChangeSignature{};
}

ChangeSignature manual_signature(ImageBuffer thumbnail, int32_t frame_width, int32_t frame_height) {
    ChangeSignature signature;
    signature.frame_width = frame_width;
    signature.frame_height = frame_height;
    signature.fingerprint = 0;
    signature.thumbnail = std::move(thumbnail);
    return signature;
}

/// A 64x64 kGray8 buffer with one bright pixel at (x, y).
ImageBuffer thumbnail_with_blob(int32_t size, int32_t x, int32_t y) {
    auto buffer = ImageBuffer::create(PixelFormat::kGray8, size, size, kBudget);
    EXPECT_TRUE(buffer.ok());
    if (!buffer.ok()) {
        return ImageBuffer{};
    }
    ImageBuffer thumb = buffer.take_value();
    thumb.data()[static_cast<size_t>(y) * static_cast<size_t>(thumb.row_stride_bytes()) + static_cast<size_t>(x)] =
        static_cast<std::byte>(255);
    return thumb;
}

// --- frozen semantics: zero shift, confidence boundaries -------------------------

TEST(ShiftEstimation, IdenticalFeaturefulFramesGiveZeroShiftAndFullConfidence) {
    GrayImage frame = make_gray(128, 128, 128, 0);
    fill_random(frame, 0x12345678U);  // aperiodic: only (0, 0) can score SAD 0

    const ShiftEstimationParams params;
    const ShiftEstimate estimate = expect_ok(estimate_global_shift(frame.view, frame.view, params), "identical");
    EXPECT_EQ(estimate.thumbnail_dx, 0);
    EXPECT_EQ(estimate.thumbnail_dy, 0);
    EXPECT_EQ(estimate.dx, 0.0F);
    EXPECT_EQ(estimate.dy, 0.0F);
    // SAD_best == 0 with all other candidates > 0: the frozen rule gives
    // exactly 1, not a tolerance-checked approximation.
    EXPECT_EQ(estimate.confidence, 1.0F);
}

TEST(ShiftEstimation, FeaturelessFramesTieToZeroShiftAndZeroConfidence) {
    // Uniform frames: every candidate ties at SAD 0, the winner order falls
    // through to the smallest (radius, dy, dx) = (0, 0), and the confidence
    // denominator vanishes exactly, which the contract defines as 0.
    const GrayImage frame = make_gray(128, 128, 128, 90);

    const ShiftEstimationParams params;
    const ShiftEstimate estimate = expect_ok(estimate_global_shift(frame.view, frame.view, params), "uniform");
    EXPECT_EQ(estimate.thumbnail_dx, 0);
    EXPECT_EQ(estimate.thumbnail_dy, 0);
    EXPECT_EQ(estimate.confidence, 0.0F);
}

TEST(ShiftEstimation, SingleCandidateMaxShiftZeroHasZeroConfidence) {
    // max_shift == 0 leaves one candidate; the estimate succeeds even for
    // completely different frames and reports confidence 0 by definition.
    GrayImage previous = make_gray(64, 64, 64, 0);
    fill_random(previous, 42U);
    GrayImage current = make_gray(64, 64, 64, 0);
    fill_random(current, 0xDEADBEEFU);

    ShiftEstimationParams params;
    params.max_shift = 0;
    const ShiftEstimate estimate = expect_ok(estimate_global_shift(previous.view, current.view, params), "m0");
    EXPECT_EQ(estimate.thumbnail_dx, 0);
    EXPECT_EQ(estimate.thumbnail_dy, 0);
    EXPECT_EQ(estimate.dx, 0.0F);
    EXPECT_EQ(estimate.dy, 0.0F);
    EXPECT_EQ(estimate.confidence, 0.0F);
}

// --- frozen semantics: known-shift recovery and the coordinate chain -------------

TEST(ShiftEstimation, KnownIntegerShiftIsRecoveredExactly) {
    // 128x128 frames at thumbnail 64: the exact 2x area resample commutes with
    // the 2-pixel frame translation, so the (+8, -4) frame shift is recovered
    // exactly as thumbnail (4, -2) and frame (8.0, -4.0).
    GrayImage previous = make_gray(128, 128, 128, 0);
    fill_random(previous, 0xCAFEBABEU);
    const GrayImage current = shifted_copy(previous, 8, -4, 0);

    const ShiftEstimationParams params;
    const ShiftEstimate estimate = expect_ok(estimate_global_shift(previous.view, current.view, params), "recovery");
    EXPECT_EQ(estimate.thumbnail_dx, 4);
    EXPECT_EQ(estimate.thumbnail_dy, -2);
    EXPECT_EQ(estimate.dx, 8.0F);
    EXPECT_EQ(estimate.dy, -4.0F);
    EXPECT_EQ(estimate.confidence, 1.0F);

    // RULE-05: the reported shift enters the coordinate chain as a translation
    // from previous-frame to current-frame coordinates (p_current = p_previous
    // + (dx, dy)) and round-trips through compose/inverse.
    const auto translation =
        mirador::make_translation(estimate.dx, estimate.dy, CoordinateSpaceId::kFrame, CoordinateSpaceId::kOriented);
    const auto back = mirador::inverse(translation);
    ASSERT_TRUE(back.ok());
    const auto round_trip = mirador::compose(translation, back.value());
    ASSERT_TRUE(round_trip.ok());
    const PointF point{10.0F, 20.0F};
    const PointF recovered = mirador::transform_point(round_trip.value(), point);
    EXPECT_NEAR(recovered.x, point.x, 1e-6F);
    EXPECT_NEAR(recovered.y, point.y, 1e-6F);
    // Direction: the previous-frame point lands on the same content in the
    // current frame.
    const PointF moved = mirador::transform_point(translation, point);
    EXPECT_FLOAT_EQ(moved.x, 18.0F);
    EXPECT_FLOAT_EQ(moved.y, 16.0F);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest macro expansion dominates the metric
TEST(ShiftEstimation, NonSquareFrameMappingIsExactPerAxis) {
    // 256x128 frame at thumbnail 32: scales 8 (x) and 4 (y). The pattern is
    // constant on the resample blocks, so the (24, -8) frame shift is an exact
    // (3, -2) thumbnail shift and the per-axis mapping is the exact rational
    // 3 * 256/32 = 24 and -2 * 128/32 = -8.
    GrayImage previous = make_gray(256, 128, 256, 0);
    DeterministicRandom random(7U);
    for (int32_t y = 0; y < 128; y += 4) {  // 8x4 frame-pixel blocks = resample cells
        for (int32_t x = 0; x < 256; x += 8) {
            const auto value = static_cast<std::byte>(random.next_byte());
            for (int32_t by = 0; by < 4; ++by) {
                for (int32_t bx = 0; bx < 8; ++bx) {
                    pixel(previous, x + bx, y + by) = value;
                }
            }
        }
    }
    const GrayImage current = shifted_copy(previous, 24, -8, 0);

    ShiftEstimationParams params;
    params.thumbnail_size = 32;
    params.max_shift = 8;
    const ShiftEstimate estimate = expect_ok(estimate_global_shift(previous.view, current.view, params), "nonsquare");
    EXPECT_EQ(estimate.thumbnail_dx, 3);
    EXPECT_EQ(estimate.thumbnail_dy, -2);
    EXPECT_EQ(estimate.dx, 24.0F);
    EXPECT_EQ(estimate.dy, -8.0F);
    EXPECT_EQ(estimate.confidence, 1.0F);
}

TEST(ShiftEstimation, RationalFrameMappingIsExactInFloat) {
    // Manual signatures decouple the mapping check from resampling: frame
    // 100x50, thumbnail shift (1, 1). The frozen precision rule evaluates the
    // exact rational shift * frame_dim / thumbnail_size, so dx = 100/64 =
    // 1.5625 and dy = 50/64 = 0.78125 must hold with exact float equality.
    auto previous_thumb = thumbnail_with_blob(64, 10, 20);
    auto current_thumb = thumbnail_with_blob(64, 11, 21);
    ASSERT_FALSE(previous_thumb.empty());
    ASSERT_FALSE(current_thumb.empty());
    const ChangeSignature previous = manual_signature(std::move(previous_thumb), 100, 50);
    const ChangeSignature current = manual_signature(std::move(current_thumb), 100, 50);

    ShiftEstimationParams params;
    params.max_shift = 4;
    const ShiftEstimate estimate = expect_ok(estimate_global_shift(previous, current, params), "rational");
    EXPECT_EQ(estimate.thumbnail_dx, 1);
    EXPECT_EQ(estimate.thumbnail_dy, 1);
    EXPECT_EQ(estimate.dx, 1.5625F);
    EXPECT_EQ(estimate.dy, 0.78125F);
    EXPECT_EQ(estimate.confidence, 1.0F);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest macro expansion dominates the metric
TEST(ShiftEstimation, TiedPeriodicPatternPrefersSmallestChebyshevRadius) {
    // Vertical stripes with a 32-frame-pixel (16-thumbnail-pixel) period: the
    // candidates (dx, dy) with dx in {-16, 0, +16} and arbitrary dy all score
    // SAD 0 (the pattern is constant along y). The frozen winner order must
    // resolve the tie to radius 0, i.e. exactly (0, 0).
    GrayImage frame = make_gray(128, 128, 128, 0);
    for (int32_t y = 0; y < 128; ++y) {
        for (int32_t x = 0; x < 128; ++x) {
            pixel(frame, x, y) = static_cast<std::byte>((x / 16) % 2 == 0 ? 0 : 255);
        }
    }

    const ShiftEstimationParams params;  // max_shift 16: the tied shifts are extreme candidates
    const ShiftEstimate estimate = expect_ok(estimate_global_shift(frame.view, frame.view, params), "periodic");
    EXPECT_EQ(estimate.thumbnail_dx, 0);
    EXPECT_EQ(estimate.thumbnail_dy, 0);
    EXPECT_EQ(estimate.dx, 0.0F);
    EXPECT_EQ(estimate.dy, 0.0F);
    EXPECT_EQ(estimate.confidence, 1.0F);
}

TEST(ShiftEstimation, ExtremeWindowEdgeShiftsAreRecovered) {
    // 8x8 gray frames pass through the thumbnail pipeline unchanged (identity
    // resample, gray stays gray), so thumbnail coordinates are frame
    // coordinates. max_shift 3 is the inclusive bound for size 8 (window 1x1):
    // single-pixel blobs force the extreme candidates (+3, +3) and (-3, -3),
    // which read exactly at the window edges (ASAN guards the accesses).
    ShiftEstimationParams params;
    params.thumbnail_size = 8;
    params.max_shift = 3;

    GrayImage previous = make_gray(8, 8, 8, 0);
    GrayImage current = make_gray(8, 8, 8, 0);
    pixel(previous, 3, 3) = static_cast<std::byte>(255);
    pixel(current, 6, 6) = static_cast<std::byte>(255);
    ShiftEstimate estimate = expect_ok(estimate_global_shift(previous.view, current.view, params), "corner+");
    EXPECT_EQ(estimate.thumbnail_dx, 3);
    EXPECT_EQ(estimate.thumbnail_dy, 3);
    EXPECT_EQ(estimate.dx, 3.0F);
    EXPECT_EQ(estimate.dy, 3.0F);
    EXPECT_EQ(estimate.confidence, 1.0F);

    GrayImage previous_low = make_gray(8, 8, 8, 0);
    GrayImage current_low = make_gray(8, 8, 8, 0);
    pixel(previous_low, 3, 3) = static_cast<std::byte>(255);
    pixel(current_low, 0, 0) = static_cast<std::byte>(255);
    estimate = expect_ok(estimate_global_shift(previous_low.view, current_low.view, params), "corner-");
    EXPECT_EQ(estimate.thumbnail_dx, -3);
    EXPECT_EQ(estimate.thumbnail_dy, -3);
    EXPECT_EQ(estimate.dx, -3.0F);
    EXPECT_EQ(estimate.dy, -3.0F);
}

// --- determinism -----------------------------------------------------------------

TEST(ShiftEstimation, RepeatedCallsAreBitIdentical) {
    GrayImage previous = make_gray(128, 128, 128, 0);
    fill_random(previous, 99U);
    const GrayImage current = shifted_copy(previous, -5, 7, 0);

    const ShiftEstimationParams params;
    const ShiftEstimate first = expect_ok(estimate_global_shift(previous.view, current.view, params), "first");
    const ShiftEstimationParams params_again;  // a fresh, value-identical params instance
    const ShiftEstimate second = expect_ok(estimate_global_shift(previous.view, current.view, params_again), "second");
    expect_bit_identical(first, second, "repeated calls");
}

TEST(ShiftEstimation, IndependentFrameCopiesAreBitIdentical) {
    // Double-instance determinism: byte-identical frames in independent
    // allocations must produce bit-identical estimates (no hidden state). The
    // exact 2x scale keeps a known shift recoverable, so the determinism is
    // over a nontrivial value.
    GrayImage previous = make_gray(128, 128, 128, 0);
    fill_random(previous, 7U);
    const GrayImage current = shifted_copy(previous, 6, -2, 0);

    GrayImage previous_copy = make_gray(128, 128, 128, 0);
    GrayImage current_copy = make_gray(128, 128, 128, 0);
    std::copy(previous.bytes.begin(), previous.bytes.end(), previous_copy.bytes.begin());
    std::copy(current.bytes.begin(), current.bytes.end(), current_copy.bytes.begin());
    ASSERT_EQ(memcmp(previous.bytes.data(), previous_copy.bytes.data(), previous.bytes.size()), 0);

    const ShiftEstimationParams params;
    const ShiftEstimate direct = expect_ok(estimate_global_shift(previous.view, current.view, params), "direct");
    const ShiftEstimate copied =
        expect_ok(estimate_global_shift(previous_copy.view, current_copy.view, params), "copied");
    expect_bit_identical(direct, copied, "independent copies");

    EXPECT_EQ(direct.thumbnail_dx, 3);
    EXPECT_EQ(direct.thumbnail_dy, -1);
    EXPECT_EQ(direct.dx, 6.0F);
    EXPECT_EQ(direct.dy, -2.0F);
}

TEST(ShiftEstimation, PaddedStrideDoesNotChangeTheEstimate) {
    // Odd presented size and non-tight rows (stride 40 vs 33): the thumbnail
    // pipeline must make the estimate stride-independent.
    GrayImage tight = make_gray(33, 21, 33, 0);
    fill_random(tight, 555U);
    GrayImage padded_current = make_gray(33, 21, 40, 0);
    fill_random(padded_current, 777U);
    // Same seed: identical pixel content in a tight layout, padding untouched.
    GrayImage tight_current = make_gray(33, 21, 33, 0);
    fill_random(tight_current, 777U);

    ShiftEstimationParams params;
    params.thumbnail_size = 16;
    params.max_shift = 4;
    const ShiftEstimate padded = expect_ok(estimate_global_shift(tight.view, padded_current.view, params), "padded");
    const ShiftEstimate padded_tight =
        expect_ok(estimate_global_shift(tight.view, tight_current.view, params), "tight");
    expect_bit_identical(padded, padded_tight, "stride independence");
}

// --- signature overload ----------------------------------------------------------

TEST(ShiftEstimation, SignatureOverloadMatchesViewOverloadBitwise) {
    GrayImage previous = make_gray(128, 128, 128, 0);
    fill_random(previous, 2024U);
    const GrayImage current = shifted_copy(previous, 10, 6, 0);

    const ChangeSignature previous_signature = signature_of(previous, 64);
    const ChangeSignature current_signature = signature_of(current, 64);
    ASSERT_EQ(previous_signature.thumbnail.width(), 64);

    const ShiftEstimationParams defaults;
    const ShiftEstimate via_views = expect_ok(estimate_global_shift(previous.view, current.view, defaults), "views");
    const ShiftEstimate via_signatures =
        expect_ok(estimate_global_shift(previous_signature, current_signature, defaults), "signatures");
    expect_bit_identical(via_views, via_signatures, "signature vs view");

    // The stored thumbnails define the search: unused params fields stay
    // range-checked but must not influence the result.
    ShiftEstimationParams odd_params;
    odd_params.thumbnail_size = 32;    // differs from the stored 64, still well-formed
    odd_params.max_shift = 8;          // must fit params.thumbnail_size even here
    odd_params.work_budget_bytes = 1;  // the signature search allocates nothing
    const ShiftEstimate via_odd =
        expect_ok(estimate_global_shift(previous_signature, current_signature, odd_params), "odd params");
    expect_bit_identical(via_views, via_odd, "unused params fields");
}

TEST(ShiftEstimation, SignatureOverloadRejectsBadStoredState) {
    const ChangeSignature valid = manual_signature(thumbnail_with_blob(8, 3, 3), 64, 64);
    ASSERT_EQ(valid.thumbnail.width(), 8);

    expect_invalid(estimate_global_shift(ChangeSignature{}, manual_signature(thumbnail_with_blob(8, 0, 0), 64, 64),
                                         ShiftEstimationParams{}),
                   "empty previous thumbnail");

    auto rgba = ImageBuffer::create(PixelFormat::kRgba8, 64, 64, kBudget);
    ASSERT_TRUE(rgba.ok());
    expect_invalid(
        estimate_global_shift(manual_signature(rgba.take_value(), 256, 256),
                              manual_signature(thumbnail_with_blob(64, 0, 0), 256, 256), ShiftEstimationParams{}),
        "non-gray thumbnail");

    auto rectangle = ImageBuffer::create(PixelFormat::kGray8, 64, 32, kBudget);
    ASSERT_TRUE(rectangle.ok());
    expect_invalid(
        estimate_global_shift(manual_signature(rectangle.take_value(), 256, 128),
                              manual_signature(thumbnail_with_blob(64, 0, 0), 256, 128), ShiftEstimationParams{}),
        "non-square thumbnail");

    auto tiny = ImageBuffer::create(PixelFormat::kGray8, 4, 4, kBudget);
    ASSERT_TRUE(tiny.ok());
    expect_invalid(
        estimate_global_shift(manual_signature(tiny.take_value(), 16, 16),
                              manual_signature(thumbnail_with_blob(8, 0, 0), 16, 16), ShiftEstimationParams{}),
        "thumbnail below 8");

    auto huge = ImageBuffer::create(PixelFormat::kGray8, 300, 300, kBudget);
    ASSERT_TRUE(huge.ok());
    expect_invalid(
        estimate_global_shift(manual_signature(huge.take_value(), 1024, 1024),
                              manual_signature(thumbnail_with_blob(8, 0, 0), 1024, 1024), ShiftEstimationParams{}),
        "thumbnail above 256");

    expect_invalid(
        estimate_global_shift(manual_signature(thumbnail_with_blob(8, 0, 0), 0, 64),
                              manual_signature(thumbnail_with_blob(8, 0, 0), 0, 64), ShiftEstimationParams{}),
        "zero frame width");

    expect_invalid(
        estimate_global_shift(manual_signature(thumbnail_with_blob(8, 0, 0), 70000, 64),
                              manual_signature(thumbnail_with_blob(8, 0, 0), 70000, 64), ShiftEstimationParams{}),
        "frame width above kMaxImageDimension");

    expect_invalid(
        estimate_global_shift(manual_signature(thumbnail_with_blob(8, 0, 0), 64, 64),
                              manual_signature(thumbnail_with_blob(8, 0, 0), 65, 64), ShiftEstimationParams{}),
        "mismatched frame dimensions");

    // max_shift valid for params.thumbnail_size but beyond the stored 8x8
    // thumbnails.
    ShiftEstimationParams params;
    params.max_shift = 16;  // fits the default thumbnail_size 64, exceeds the stored 8
    expect_invalid(estimate_global_shift(manual_signature(thumbnail_with_blob(8, 0, 0), 64, 64),
                                         manual_signature(thumbnail_with_blob(8, 0, 0), 64, 64), params),
                   "max_shift beyond stored thumbnails");
}

// --- DOD-03 coordinate matrix ----------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest macro expansion dominates the metric
TEST(ShiftEstimation, EstimateIsIndependentOfRotationMetadataDOD03Matrix) {
    // Odd presented size (33x21), padded non-tight rows (stride 40), odd
    // thumbnail edge 9 with max_shift at its inclusive bound 4. The comparison
    // always runs on presented pixels, so every rotation metadata value must
    // yield a bit-identical, in-bounds estimate.
    GrayImage previous = make_gray(33, 21, 40, 0);
    fill_random(previous, 313U);
    const GrayImage current = shifted_copy(previous, 2, 1, 0);

    ShiftEstimationParams params;
    params.thumbnail_size = 9;
    params.max_shift = 4;  // == (9 - 1) / 2, the inclusive bound

    bool first = true;
    ShiftEstimate baseline;
    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        ImageView previous_view = previous.view;
        previous_view.rotation = rotation;
        ImageView current_view = current.view;
        current_view.rotation = rotation;
        const ShiftEstimate estimate =
            expect_ok(estimate_global_shift(previous_view, current_view, params), "rotation matrix");
        EXPECT_GE(estimate.thumbnail_dx, -4);
        EXPECT_LE(estimate.thumbnail_dx, 4);
        EXPECT_GE(estimate.thumbnail_dy, -4);
        EXPECT_LE(estimate.thumbnail_dy, 4);
        if (first) {
            baseline = estimate;
            first = false;
        } else {
            expect_bit_identical(baseline, estimate, "rotation metadata");
        }
    }
}

// --- budget protection (RULE-06) -------------------------------------------------

TEST(ShiftEstimation, BudgetBoundaryIsExplicit) {
    // Gray 128x128 at thumbnail 64 makes exactly one internal allocation
    // request: the 64x64 gray intermediate (4096 bytes; the format is already
    // gray, so no conversion follows). Per-request budget semantics: 4096
    // succeeds, one byte less fails with kBudgetExceeded.
    GrayImage previous = make_gray(128, 128, 128, 0);
    fill_random(previous, 11U);
    const GrayImage current = shifted_copy(previous, 3, 3, 0);

    ShiftEstimationParams params;
    params.work_budget_bytes = 4096;
    expect_ok(estimate_global_shift(previous.view, current.view, params), "budget boundary");

    params.work_budget_bytes = 4095;
    const auto denied = estimate_global_shift(previous.view, current.view, params);
    ASSERT_FALSE(denied.ok());
    EXPECT_EQ(denied.status().code(), ErrorCode::kBudgetExceeded);

    params.work_budget_bytes = 1;
    const auto one_byte = estimate_global_shift(previous.view, current.view, params);
    ASSERT_FALSE(one_byte.ok());
    EXPECT_EQ(one_byte.status().code(), ErrorCode::kBudgetExceeded);

    // RGBA input resamples into a 64x64 RGBA intermediate (16384 bytes) before
    // the gray conversion (4096): the per-request budget must reject 8192 even
    // though both individual artifacts after the first would fit, and accept
    // 16384.
    GrayImage rgba_previous = make_gray(128, 128, 128, 0);  // content reused via an RGBA wrapper below
    fill_random(rgba_previous, 11U);
    const std::vector<std::byte> rgba_bytes = [&] {
        std::vector<std::byte> bytes(static_cast<size_t>(128) * 128 * 4);
        for (int32_t y = 0; y < 128; ++y) {
            for (int32_t x = 0; x < 128; ++x) {
                const auto value = pixel(rgba_previous, x, y);
                const size_t index = (static_cast<size_t>(y) * 128U + static_cast<size_t>(x)) * 4U;
                bytes[index + 0] = value;
                bytes[index + 1] = value;
                bytes[index + 2] = value;
                bytes[index + 3] = static_cast<std::byte>(255);
            }
        }
        return bytes;
    }();
    ImageView rgba_view;
    rgba_view.data = rgba_bytes.data();
    rgba_view.width = 128;
    rgba_view.height = 128;
    rgba_view.row_stride_bytes = int64_t{128} * 4;
    rgba_view.format = PixelFormat::kRgba8;

    ShiftEstimationParams rgba_params;
    rgba_params.work_budget_bytes = 8192;
    const auto rgba_denied = estimate_global_shift(rgba_view, rgba_view, rgba_params);
    ASSERT_FALSE(rgba_denied.ok());
    EXPECT_EQ(rgba_denied.status().code(), ErrorCode::kBudgetExceeded);

    rgba_params.work_budget_bytes = 16384;
    expect_ok(estimate_global_shift(rgba_view, rgba_view, rgba_params), "rgba budget boundary");
}

// --- error model (RULE-08) -------------------------------------------------------

TEST(ShiftEstimation, RejectsInvalidViewsAndParameters) {
    const GrayImage frame = make_gray(64, 64, 64, 0);
    const ShiftEstimationParams defaults;

    expect_invalid(estimate_global_shift(ImageView{}, frame.view, defaults), "null previous view");
    expect_invalid(estimate_global_shift(frame.view, ImageView{}, defaults), "null current view");

    const GrayImage other_size = make_gray(64, 48, 64, 0);
    expect_invalid(estimate_global_shift(frame.view, other_size.view, defaults), "mismatched dimensions");

    ShiftEstimationParams params = defaults;
    params.thumbnail_size = 7;
    expect_invalid(estimate_global_shift(frame.view, frame.view, params), "thumbnail_size below 8");

    params.thumbnail_size = 257;
    expect_invalid(estimate_global_shift(frame.view, frame.view, params), "thumbnail_size above 256");

    params = defaults;
    params.max_shift = -1;
    expect_invalid(estimate_global_shift(frame.view, frame.view, params), "negative max_shift");

    params.max_shift = 32;  // 2 * 32 == 64 > 64 - 1
    expect_invalid(estimate_global_shift(frame.view, frame.view, params), "max_shift at the exclusive bound");

    params.max_shift = 31;  // the inclusive bound is valid
    expect_ok(estimate_global_shift(frame.view, frame.view, params), "max_shift at the inclusive bound");

    params = defaults;
    params.work_budget_bytes = 0;
    expect_invalid(estimate_global_shift(frame.view, frame.view, params), "zero budget");

    params.work_budget_bytes = -5;
    expect_invalid(estimate_global_shift(frame.view, frame.view, params), "negative budget");
}

// --- cancellation and deadline conversion ----------------------------------------

TEST(ShiftEstimation, CancelledContextYieldsExplicitKCancelled) {
    GrayImage previous = make_gray(64, 64, 64, 0);
    fill_random(previous, 5U);
    const GrayImage current = shifted_copy(previous, 2, 2, 0);
    const auto signature_previous = signature_of(previous, 64);
    const auto signature_current = signature_of(current, 64);
    const ShiftEstimationParams params;

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const auto view_cancelled = estimate_global_shift(previous.view, current.view, params, cancelled);
    ASSERT_FALSE(view_cancelled.ok());
    EXPECT_EQ(view_cancelled.status().code(), ErrorCode::kCancelled);
    const auto signature_cancelled = estimate_global_shift(signature_previous, signature_current, params, cancelled);
    ASSERT_FALSE(signature_cancelled.ok());
    EXPECT_EQ(signature_cancelled.status().code(), ErrorCode::kCancelled);

    // Validation takes precedence over cancellation.
    ShiftEstimationParams invalid = params;
    invalid.thumbnail_size = 0;
    const auto invalid_first = estimate_global_shift(previous.view, current.view, invalid, cancelled);
    ASSERT_FALSE(invalid_first.ok());
    EXPECT_EQ(invalid_first.status().code(), ErrorCode::kInvalidArgument);
}

TEST(ShiftEstimation, MidSearchCancellationStopsWithinARowBoundary) {
    // The contract promises an entry check plus one check per search row: the
    // counting callback flips on its 5th observation (entry + four row
    // checks), so the call must return kCancelled after exactly five
    // observations and never a partial estimate.
    GrayImage previous = make_gray(64, 64, 64, 0);
    fill_random(previous, 6U);
    const GrayImage current = shifted_copy(previous, 1, 3, 0);

    int observations = 0;
    ExecutionContext late_cancel;
    late_cancel.is_cancelled = [&observations] { return ++observations >= 5; };

    const ShiftEstimationParams params;  // 33 rows of search: the flip lands mid-search
    const auto result = estimate_global_shift(previous.view, current.view, params, late_cancel);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kCancelled);
    EXPECT_EQ(observations, 5);
}

TEST(ShiftEstimation, PastDeadlineYieldsExplicitKTimeout) {
    GrayImage previous = make_gray(64, 64, 64, 0);
    fill_random(previous, 8U);
    const GrayImage current = shifted_copy(previous, -2, 1, 0);
    const auto signature_previous = signature_of(previous, 64);
    const auto signature_current = signature_of(current, 64);
    const ShiftEstimationParams params;

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    const auto view_timeout = estimate_global_shift(previous.view, current.view, params, expired);
    ASSERT_FALSE(view_timeout.ok());
    EXPECT_EQ(view_timeout.status().code(), ErrorCode::kTimeout);
    const auto signature_timeout = estimate_global_shift(signature_previous, signature_current, params, expired);
    ASSERT_FALSE(signature_timeout.ok());
    EXPECT_EQ(signature_timeout.status().code(), ErrorCode::kTimeout);
}

// --- format paths ----------------------------------------------------------------

TEST(ShiftEstimation, RgbaGrayValuedPathMatchesGrayPathBitwise) {
    // BT.601 luma of gray-valued RGBA pixels is the value itself, so the RGBA
    // path must be bit-identical to the gray path on the same content.
    GrayImage gray_previous = make_gray(128, 128, 128, 0);
    fill_random(gray_previous, 77U);
    const GrayImage gray_current = shifted_copy(gray_previous, 5, 9, 0);

    const auto to_rgba = [](const GrayImage& source) {
        std::vector<std::byte> bytes(static_cast<size_t>(source.view.width) * static_cast<size_t>(source.view.height) *
                                     4U);
        for (int32_t y = 0; y < source.view.height; ++y) {
            for (int32_t x = 0; x < source.view.width; ++x) {
                const auto value = pixel(source, x, y);
                const size_t index =
                    (static_cast<size_t>(y) * static_cast<size_t>(source.view.width) + static_cast<size_t>(x)) * 4U;
                bytes[index + 0] = value;
                bytes[index + 1] = value;
                bytes[index + 2] = value;
                bytes[index + 3] = static_cast<std::byte>(255);
            }
        }
        ImageView view;
        view.data = bytes.data();
        view.width = source.view.width;
        view.height = source.view.height;
        view.row_stride_bytes = int64_t{source.view.width} * 4;
        view.format = PixelFormat::kRgba8;
        return std::pair(std::move(bytes), view);
    };
    const auto [rgba_previous_bytes, rgba_previous_view] = to_rgba(gray_previous);
    const auto [rgba_current_bytes, rgba_current_view] = to_rgba(gray_current);

    const ShiftEstimationParams params;
    const ShiftEstimate via_gray =
        expect_ok(estimate_global_shift(gray_previous.view, gray_current.view, params), "gray");
    const ShiftEstimate via_rgba =
        expect_ok(estimate_global_shift(rgba_previous_view, rgba_current_view, params), "rgba");
    expect_bit_identical(via_gray, via_rgba, "gray vs rgba");
}

TEST(ShiftEstimation, Nv12LumaPathMatchesGrayPathBitwise) {
    // NV12 thumbnails resample the luma plane only, so the NV12 path must be
    // bit-identical to the gray path built from the same luma bytes.
    GrayImage gray_previous = make_gray(128, 128, 128, 0);
    fill_random(gray_previous, 88U);
    const GrayImage gray_current = shifted_copy(gray_previous, -7, 4, 0);

    const auto to_nv12 = [](const GrayImage& source) {
        auto buffer = ImageBuffer::create(PixelFormat::kNv12, source.view.width, source.view.height, kBudget);
        EXPECT_TRUE(buffer.ok());
        if (!buffer.ok()) {
            return ImageBuffer{};
        }
        ImageBuffer nv12 = buffer.take_value();
        const ImageView view = nv12.view();
        for (int32_t y = 0; y < source.view.height; ++y) {
            std::copy_n(source.bytes.data() + static_cast<int64_t>(y) * source.view.row_stride_bytes, source.view.width,
                        nv12.data() + static_cast<int64_t>(y) * view.row_stride_bytes);
        }
        // Neutral chroma: the comparison only ever touches luma.
        const int64_t chroma_offset = static_cast<int64_t>(source.view.height) * view.row_stride_bytes;
        std::fill(nv12.data() + chroma_offset, nv12.data() + nv12.byte_size(), static_cast<std::byte>(128));
        return nv12;
    };
    const ImageBuffer nv12_previous = to_nv12(gray_previous);
    const ImageBuffer nv12_current = to_nv12(gray_current);
    ASSERT_FALSE(nv12_previous.empty());

    const ShiftEstimationParams params;
    const ShiftEstimate via_gray =
        expect_ok(estimate_global_shift(gray_previous.view, gray_current.view, params), "gray");
    const ShiftEstimate via_nv12 =
        expect_ok(estimate_global_shift(nv12_previous.view(), nv12_current.view(), params), "nv12");
    expect_bit_identical(via_gray, via_nv12, "gray vs nv12");
}

// --- privacy default (RULE-10) ---------------------------------------------------

std::map<std::string, std::filesystem::file_type> snapshot_directory(const std::filesystem::path& directory) {
    std::map<std::string, std::filesystem::file_type> entries;
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        ADD_FAILURE() << "cannot list " << directory << ": " << ec.message();
        return entries;
    }
    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code status_ec;
        entries[it->path().filename().string()] = it->status(status_ec).type();
        it.increment(ec);
        if (ec) {
            ADD_FAILURE() << "cannot iterate " << directory << ": " << ec.message();
            break;
        }
    }
    return entries;
}

TEST(ShiftEstimation, EstimationWritesNoFiles) {
    namespace fs = std::filesystem;
    GrayImage previous = make_gray(128, 128, 128, 0);
    fill_random(previous, 12U);
    const GrayImage current = shifted_copy(previous, 4, 4, 0);
    const auto signature_previous = signature_of(previous, 64);
    const auto signature_current = signature_of(current, 64);

    const fs::path cwd = fs::current_path();
    const fs::path temp = fs::temp_directory_path();
    const auto cwd_before = snapshot_directory(cwd);
    const auto temp_before = snapshot_directory(temp);
    ASSERT_FALSE(cwd_before.empty());

    const ShiftEstimationParams params;
    expect_ok(estimate_global_shift(previous.view, current.view, params), "ok call");
    expect_ok(estimate_global_shift(signature_previous, signature_current, params), "ok signature call");
    ShiftEstimationParams denied = params;
    denied.work_budget_bytes = 1;  // failure paths must not write either
    const auto budget_failure = estimate_global_shift(previous.view, current.view, denied);
    ASSERT_FALSE(budget_failure.ok());
    ShiftEstimationParams invalid_params = params;
    invalid_params.thumbnail_size = 0;
    expect_invalid(estimate_global_shift(previous.view, current.view, invalid_params), "invalid call");

    const auto cwd_after = snapshot_directory(cwd);
    const auto temp_after = snapshot_directory(temp);
    for (const auto& [name, type] : cwd_after) {
        (void)type;
        EXPECT_NE(cwd_before.find(name), cwd_before.end()) << "unexpected new file in cwd: " << name;
    }
    for (const auto& [name, type] : temp_after) {
        (void)type;
        EXPECT_NE(temp_before.find(name), temp_before.end()) << "unexpected new file in temp: " << name;
    }
}

}  // namespace
