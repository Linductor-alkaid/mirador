// M7-10 go/no-go verdict pin (Independent-Verification-Agent). The M7-10
// verdict record (docs/plans/m7-cross-frame-object-tracking.md,
// "Go/No-Go 判定记录", 2026-09-28) ruled GO with the library default
// `min_compensation_confidence` kept at 0.0 — the unfiltered, no-prior gate —
// with 0.7 published as the harness caller's reference strategy (DEC-022 open
// item 1; a library-side raise is deferred to the real-data review). The
// default *value* is pinned by the M7-09 calibration suite; this suite pins
// the ruling's behavioral face on the confidence bands the published
// calibration row measured (docs/benchmarks/linux-x64-object-tracking-2026-09.md,
// `min_compensation_confidence` row: true scroll [0.93, 0.95] vs local-change
// pseudo-shift [0.46, 0.55]):
//   * the local-change band passes the 0.0 library default gate — the measured
//     exposure the ruling records verbatim ("0.0 门下 C/D 曾对局部变化帧平移
//     整池"): the whole pool moves for local-change frames unless the caller
//     configures the knob;
//   * the same band is explicitly refused (estimate echoed, pool untouched)
//     at the 0.7 caller policy — the reference strategy the ruling keeps;
//   * the true-scroll band applies at BOTH configurations (0.7 must not block
//     genuine scrolls — "0.7 后真滚动照常应用");
//   * an explicitly configured 0.0 gate is behaviorally identical to the
//     library default — "维持 0.0" means the default IS the unfiltered gate,
//     with no implicit special-casing of the default value.
// Threshold behavior, boundary semantics, coordinate/rotation/odd-size/stride
// matrices and determinism live in the M7-05/06/07/08 suites; the default
// values themselves live in the M7-09 calibration suite. Nothing here
// duplicates those paths.

#include <mirador/object_tracker.hpp>

#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/shift_estimation.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::ImageView;
using mirador::MotionCompensationResult;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::RectF;
using mirador::ShiftEstimate;
using mirador::TargetTrack;
using mirador::VisualRegion;

// --- fixtures and helpers -----------------------------------------------------

/// Owning single-channel gray8 buffer with an explicit row stride.
struct GrayImage {
    std::vector<std::byte> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int64_t stride = 0;
};

/// Noise-like per-pixel pattern (uint32 hashing, deterministic, no UB) — the
/// high-frequency texture keeps the adoption template capture well-formed.
uint8_t noise_pixel(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393U + static_cast<uint32_t>(y) * 668265263U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    return static_cast<uint8_t>((h ^ (h >> 16U)) % 256U);
}

/// Odd x odd extents with +5 bytes of stride padding: the adoption capture
/// exercises the non-contiguous-stride read path while the suite stays off the
/// DOD-03 matrix (owned by the M7-07 suite).
GrayImage noise_image(int32_t width, int32_t height) {
    GrayImage image;
    image.width = width;
    image.height = height;
    image.stride = static_cast<int64_t>(width) + 5;
    image.pixels.assign(static_cast<size_t>(image.stride) * static_cast<size_t>(height), std::byte{0});
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x)] =
                std::byte{noise_pixel(x, y)};
        }
    }
    return image;
}

ImageView view_of(const GrayImage& image) {
    ImageView view;
    view.data = image.pixels.data();
    view.width = image.width;
    view.height = image.height;
    view.row_stride_bytes = image.stride;
    view.format = PixelFormat::kGray8;
    return view;
}

VisualRegion make_region(uint64_t stable_id, RectF bounds, float confidence = 0.9F) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.anchor = PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    region.confidence = confidence;
    return region;
}

ObjectTracker make_tracker(const ObjectTrackerOptions& options = {}) {
    auto created = ObjectTracker::create(options);
    if (!created.ok()) {
        ADD_FAILURE() << "make_tracker: " << created.status().message();
        return ObjectTracker{};
    }
    return created.take_value();
}

/// Hand-built caller evidence (the tracker consumes estimates; it never runs
/// the estimator itself — the evidence-trust boundary).
ShiftEstimate shift_of(float dx, float dy, float confidence) {
    ShiftEstimate shift;
    shift.thumbnail_dx = static_cast<int32_t>(dx);
    shift.thumbnail_dy = static_cast<int32_t>(dy);
    shift.dx = dx;
    shift.dy = dy;
    shift.confidence = confidence;
    return shift;
}

/// The pool state this suite compares: bounds, center invariant and accounted
/// bytes of the single adopted track.
struct TrackPin {
    RectF last_bounds = {};
    PointF predicted_center = {};
    int64_t pool_bytes = 0;
};

TrackPin pin_of(const ObjectTracker& tracker, uint64_t track_id) {
    const TargetTrack* track = tracker.find_track(track_id);
    if (track == nullptr) {
        ADD_FAILURE() << "pin_of: track " << track_id << " missing";
        return TrackPin{};
    }
    return TrackPin{track->last_bounds, track->predicted_center, tracker.byte_size()};
}

/// The frozen center invariant: predicted_center is exactly the center of
/// last_bounds (M7-07 contract, consumed by the position gate downstream).
void expect_center_invariant(const TrackPin& pin) {
    const PointF expected{pin.last_bounds.x + pin.last_bounds.width / 2.0F,
                          pin.last_bounds.y + pin.last_bounds.height / 2.0F};
    ASSERT_EQ(pin.predicted_center, expected);
}

// --- the M7-10 ruling on the published confidence bands ------------------------

/// The measured bands decide oppositely at the two configurations the ruling
/// separates: the local-change pseudo-shift band [0.46, 0.55] moves the pool
/// at the library default 0.0 (the recorded exposure of unconfigured callers)
/// and is explicitly refused at the 0.7 caller policy, while the true-scroll
/// band [0.93, 0.95] applies at both. A library-side default raise without a
/// conscious re-recording of the verdict flips the first two rows.
TEST(ObjectTrackerVerdictRulingTest, PublishedConfidenceBandsSeparateExactlyAtTheCallerPolicy) {
    struct BandCase {
        float confidence;
        bool applied_at_caller07;  // the default (0.0) applies every case
    };
    // Band ends as published by the M7-09 calibration row.
    const BandCase cases[] = {
        {0.46F, false},  // local-change pseudo-shift band, lower end
        {0.55F, false},  // local-change pseudo-shift band, upper end
        {0.93F, true},   // true-scroll band, lower end
        {0.95F, true},   // true-scroll band, upper end
    };
    constexpr float kShiftDx = 4.0F;
    constexpr float kShiftDy = 2.0F;

    for (const BandCase& band : cases) {
        // Library default: the gate is 0.0 — the estimate applies and the
        // whole (single-track) pool translates by the evaluated shift.
        ObjectTracker default_tracker = make_tracker();
        const GrayImage image = noise_image(65, 63);
        const ImageView view = view_of(image);
        const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
        const auto adopted = default_tracker.adopt_track(make_region(9U, bounds), view, 1U);
        ASSERT_TRUE(adopted.ok()) << "confidence " << band.confidence << ": " << adopted.status().message();

        const auto applied = default_tracker.compensate_global_motion(shift_of(kShiftDx, kShiftDy, band.confidence));
        ASSERT_TRUE(applied.ok()) << "confidence " << band.confidence << ": " << applied.status().message();
        EXPECT_TRUE(applied.value().applied) << "confidence " << band.confidence << ": the 0.0 library default "
                                             << "gate must not filter any well-formed estimate";
        EXPECT_EQ(applied.value().dx, kShiftDx);
        EXPECT_EQ(applied.value().dy, kShiftDy);
        ASSERT_EQ(applied.value().tracks.size(), 1U) << "confidence " << band.confidence;
        EXPECT_EQ(applied.value().tracks[0].track_id, 9U) << "confidence " << band.confidence;

        const TrackPin moved = pin_of(default_tracker, 9U);
        EXPECT_EQ(moved.last_bounds, (RectF{bounds.x + kShiftDx, bounds.y + kShiftDy, bounds.width, bounds.height}))
            << "confidence " << band.confidence;
        ASSERT_NO_FATAL_FAILURE(expect_center_invariant(moved));

        // Harness caller policy (0.7): the local-change band is an explicit,
        // pool-untouched refusal echoing the evaluated estimate; the
        // true-scroll band still applies ("0.7 后真滚动照常应用").
        ObjectTrackerOptions caller_options;
        caller_options.min_compensation_confidence = 0.7;
        ObjectTracker caller_tracker = make_tracker(caller_options);
        const GrayImage caller_image = noise_image(65, 63);
        const auto caller_adopted = caller_tracker.adopt_track(make_region(9U, bounds), view_of(caller_image), 1U);
        ASSERT_TRUE(caller_adopted.ok()) << "confidence " << band.confidence << ": "
                                         << caller_adopted.status().message();
        const TrackPin before = pin_of(caller_tracker, 9U);
        ASSERT_NO_FATAL_FAILURE(expect_center_invariant(before));

        const auto gated = caller_tracker.compensate_global_motion(shift_of(kShiftDx, kShiftDy, band.confidence));
        ASSERT_TRUE(gated.ok()) << "confidence " << band.confidence << ": " << gated.status().message();
        EXPECT_EQ(gated.value().applied, band.applied_at_caller07) << "confidence " << band.confidence;
        EXPECT_EQ(gated.value().dx, kShiftDx) << "confidence " << band.confidence;
        EXPECT_EQ(gated.value().dy, kShiftDy) << "confidence " << band.confidence;

        if (!band.applied_at_caller07) {
            EXPECT_TRUE(gated.value().tracks.empty()) << "confidence " << band.confidence;
            const TrackPin after = pin_of(caller_tracker, 9U);
            EXPECT_EQ(after.last_bounds, before.last_bounds) << "confidence " << band.confidence;
            EXPECT_EQ(after.predicted_center, before.predicted_center) << "confidence " << band.confidence;
            EXPECT_EQ(after.pool_bytes, before.pool_bytes)
                << "confidence " << band.confidence << ": a refusal must leave the pool untouched (RULE-06)";
        } else {
            ASSERT_EQ(gated.value().tracks.size(), 1U) << "confidence " << band.confidence;
            const TrackPin after = pin_of(caller_tracker, 9U);
            EXPECT_EQ(after.last_bounds, (RectF{bounds.x + kShiftDx, bounds.y + kShiftDy, bounds.width, bounds.height}))
                << "confidence " << band.confidence;
            ASSERT_NO_FATAL_FAILURE(expect_center_invariant(after));
        }
    }
}

/// "维持 0.0" pins the default to BE the unfiltered gate: an explicitly
/// configured 0.0 gate and the library default produce bitwise-equal
/// compensation results and equal post-state. If a future change makes the
/// default diverge from the explicit 0.0 (special-casing, silent default
/// raise), this equivalence breaks before any published number does.
TEST(ObjectTrackerVerdictRulingTest, ExplicitGate00ConfigurationMatchesLibraryDefaultBitwise) {
    ObjectTracker default_tracker = make_tracker();
    ObjectTrackerOptions explicit_options;
    explicit_options.min_compensation_confidence = 0.0;
    ObjectTracker explicit_tracker = make_tracker(explicit_options);

    const GrayImage image = noise_image(65, 63);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    ASSERT_TRUE(default_tracker.adopt_track(make_region(9U, bounds), view, 1U).ok());
    ASSERT_TRUE(explicit_tracker.adopt_track(make_region(9U, bounds), view, 1U).ok());

    // A local-change-band pseudo-shift (the ruling's documented exposure).
    const auto from_default = default_tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 0.55F));
    const auto from_explicit = explicit_tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 0.55F));
    ASSERT_TRUE(from_default.ok()) << from_default.status().message();
    ASSERT_TRUE(from_explicit.ok()) << from_explicit.status().message();

    EXPECT_EQ(from_default.value().applied, from_explicit.value().applied);
    EXPECT_EQ(from_default.value().dx, from_explicit.value().dx);
    EXPECT_EQ(from_default.value().dy, from_explicit.value().dy);
    ASSERT_EQ(from_default.value().tracks.size(), from_explicit.value().tracks.size());
    for (size_t i = 0; i < from_default.value().tracks.size(); ++i) {
        EXPECT_EQ(from_default.value().tracks[i].track_id, from_explicit.value().tracks[i].track_id);
        EXPECT_EQ(from_default.value().tracks[i].previous_bounds, from_explicit.value().tracks[i].previous_bounds);
        EXPECT_EQ(from_default.value().tracks[i].compensated_bounds,
                  from_explicit.value().tracks[i].compensated_bounds);
    }

    const TrackPin default_pin = pin_of(default_tracker, 9U);
    const TrackPin explicit_pin = pin_of(explicit_tracker, 9U);
    EXPECT_EQ(default_pin.last_bounds, explicit_pin.last_bounds);
    EXPECT_EQ(default_pin.predicted_center, explicit_pin.predicted_center);
    EXPECT_EQ(default_pin.pool_bytes, explicit_pin.pool_bytes);
    ASSERT_NO_FATAL_FAILURE(expect_center_invariant(default_pin));
}

}  // namespace
