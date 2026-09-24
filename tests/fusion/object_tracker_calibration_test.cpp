// M7-09 calibration-freeze suite (DEC-019 section 5, Independent-Verification-
// Agent). The M7-09 harness run calibrated every `ObjectTrackerOptions` and
// `ShiftEstimationParams` default against the synthetic A/B/C/D matrix and
// published the per-field verdicts in
// docs/benchmarks/linux-x64-object-tracking-2026-09.md — all library defaults
// were maintained on measured grounds. These tests mechanically freeze that
// calibration surface:
//   * every default must keep the exact frozen value — any change has to be a
//     conscious recalibration that updates this suite together with the report
//     and the DEC-019 record ("任一初值在 M7-09 校准后变更需记录理由");
//   * the frozen verification-ROI reach at the defaults keeps the two scene
//     mechanics the published matrix rests on: the 48 px scroll step stays
//     beyond the E1 reach (the B/C separation structure, RISK-2026-17) while
//     the +24 px popup displacement stays inside it (the swap-pressure
//     structure, RISK-2026-16);
//   * the harness caller policy for the compensation gate (0.7, the measured
//     separation between true-scroll confidence [0.93, 0.95] and local-change
//     frames [0.46, 0.55]) behaves per the frozen inclusive gate contract —
//     the library default 0.0 itself is pinned by the first test.
//
// The threshold *behavior* (grade table, gate boundary semantics, redetection
// zero-static-trigger anchor, compensation atomicity) is covered by the
// M7-05/M7-06/M7-07/M7-08 suites; nothing here duplicates those paths.

#include <mirador/object_tracker.hpp>

#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/shift_estimation.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using mirador::ImageView;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::RectF;
using mirador::ShiftEstimate;
using mirador::ShiftEstimationParams;
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
/// high-frequency texture keeps the adoption template capture well-formed for
/// any thumbnail side (the M7-07 suite's shared pattern).
uint8_t noise_pixel(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393U + static_cast<uint32_t>(y) * 668265263U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    return static_cast<uint8_t>((h ^ (h >> 16U)) % 256U);
}

GrayImage noise_image(int32_t width, int32_t height) {
    GrayImage image;
    image.width = width;
    image.height = height;
    image.stride = width;
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
/// the estimator itself — the evidence-trust boundary under test).
ShiftEstimate shift_of(float dx, float dy, float confidence) {
    ShiftEstimate shift;
    shift.thumbnail_dx = static_cast<int32_t>(dx);
    shift.thumbnail_dy = static_cast<int32_t>(dy);
    shift.dx = dx;
    shift.dy = dy;
    shift.confidence = confidence;
    return shift;
}

// --- frozen values (docs/benchmarks/linux-x64-object-tracking-2026-09.md,
//     "门槛初值逐项校准" table; every row decided "维持") -------------------------

constexpr int32_t kFrozenMaxTargets = 64;
constexpr int32_t kFrozenMaxPositionHistory = 32;
constexpr int32_t kFrozenMaxTemplates = 4;
constexpr int32_t kFrozenMaxNegativeTemplates = 4;
constexpr int32_t kFrozenTemplateThumbSide = 32;
constexpr int64_t kFrozenPoolBudgetBytes = int64_t{1} * 1024 * 1024;
constexpr int32_t kFrozenUncertainFrameLimit = 5;
constexpr int32_t kFrozenMaxGenerationLag = 1;
constexpr double kFrozenNccStrongThreshold = 0.8;
constexpr double kFrozenNccWeakThreshold = 0.6;
constexpr double kFrozenPeakSidelobeRatioMin = 5.0;
constexpr double kFrozenStructureDeviationTolerance = 0.2;
constexpr double kFrozenVerificationRoiDiagonalRatio = 1.0;
constexpr int64_t kFrozenVerificationWorkBudgetBytes = int64_t{256} * 1024 * 1024;
constexpr double kFrozenImpostorMatchThreshold = 0.8;
constexpr double kFrozenMinCompensationConfidence = 0.0;
constexpr int32_t kFrozenRedetectBackoffBaseFrames = 1;
constexpr int32_t kFrozenRedetectBackoffMaxFrames = 60;
constexpr int32_t kFrozenRedetectMaxAttempts = 8;
constexpr int32_t kFrozenMaxRedetectionRecords = 64;

constexpr int32_t kFrozenShiftThumbnailSize = 64;
constexpr int32_t kFrozenShiftMaxShift = 16;
constexpr int64_t kFrozenShiftWorkBudgetBytes = int64_t{512} * 1024;

// The published scene mechanics (same report, 场景清单 / 校准表): the harness
// object is 64x32 px, the scroll step is 48 px per frame, and the similar-icons
// popup displacement over the covered widget is +24 px.
constexpr int32_t kHarnessObjectWidth = 64;
constexpr int32_t kHarnessObjectHeight = 32;
constexpr int32_t kHarnessScrollStepPx = 48;
constexpr int32_t kHarnessPopupDisplacementPx = 24;

// --- option defaults ------------------------------------------------------------

/// The whole ObjectTrackerOptions calibration surface, echoed through
/// `create` (so the freeze also proves the defaults stay inside their
/// documented validation ranges). One EXPECT per field: a drift names the
/// exact knob that must be re-calibrated and re-recorded.
TEST(ObjectTrackerCalibrationTest, ObjectTrackerDefaultsMatchM709CalibrationFreeze) {
    const auto created = ObjectTracker::create({});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const ObjectTrackerOptions& defaults = created.value().options();

    // Pool bounds (RULE-06).
    EXPECT_EQ(defaults.max_targets, kFrozenMaxTargets);
    EXPECT_EQ(defaults.max_position_history, kFrozenMaxPositionHistory);
    EXPECT_EQ(defaults.max_templates, kFrozenMaxTemplates);
    EXPECT_EQ(defaults.max_negative_templates, kFrozenMaxNegativeTemplates);
    EXPECT_EQ(defaults.template_thumb_side, kFrozenTemplateThumbSide);
    EXPECT_EQ(defaults.pool_budget_bytes, kFrozenPoolBudgetBytes);
    // State-machine timing.
    EXPECT_EQ(defaults.uncertain_frame_limit, kFrozenUncertainFrameLimit);
    EXPECT_EQ(defaults.max_generation_lag, kFrozenMaxGenerationLag);
    // Neighborhood verification (E1/E2).
    EXPECT_EQ(defaults.ncc_strong_threshold, kFrozenNccStrongThreshold);
    EXPECT_EQ(defaults.ncc_weak_threshold, kFrozenNccWeakThreshold);
    EXPECT_EQ(defaults.peak_sidelobe_ratio_min, kFrozenPeakSidelobeRatioMin);
    EXPECT_EQ(defaults.structure_deviation_tolerance, kFrozenStructureDeviationTolerance);
    EXPECT_EQ(defaults.verification_roi_diagonal_ratio, kFrozenVerificationRoiDiagonalRatio);
    EXPECT_EQ(defaults.verification_work_budget_bytes, kFrozenVerificationWorkBudgetBytes);
    // Evidence fusion.
    EXPECT_EQ(defaults.impostor_match_threshold, kFrozenImpostorMatchThreshold);
    // Global motion compensation: the LIBRARY default stays 0.0 — the 0.7
    // gate is the harness caller's configuration, not a library value (the
    // M7-09 verification record; library-side raise is M7-10's decision).
    EXPECT_EQ(defaults.min_compensation_confidence, kFrozenMinCompensationConfidence);
    // Cascade redetection.
    EXPECT_EQ(defaults.redetect_backoff_base_frames, kFrozenRedetectBackoffBaseFrames);
    EXPECT_EQ(defaults.redetect_backoff_max_frames, kFrozenRedetectBackoffMaxFrames);
    EXPECT_EQ(defaults.redetect_max_attempts, kFrozenRedetectMaxAttempts);
    EXPECT_EQ(defaults.max_redetection_records, kFrozenMaxRedetectionRecords);
}

/// The shift-estimation calibration surface (the report's final calibration
/// row): thumbnail 64 / max_shift 16 / 512 KiB — the 48 px scroll step is
/// 4.27 thumbnail pixels at the default, inside the +-16 search bound.
TEST(ObjectTrackerCalibrationTest, ShiftEstimationDefaultsMatchM709CalibrationFreeze) {
    const ShiftEstimationParams defaults;
    EXPECT_EQ(defaults.thumbnail_size, kFrozenShiftThumbnailSize);
    EXPECT_EQ(defaults.max_shift, kFrozenShiftMaxShift);
    EXPECT_EQ(defaults.work_budget_bytes, kFrozenShiftWorkBudgetBytes);
}

/// The frozen structural fact the published matrix rests on: at the default
/// `verification_roi_diagonal_ratio` 1.0 the E1 reach of a 64x32 track (the
/// harness object size) is +-36 px — strictly narrower than the 48 px scroll
/// step (that gap IS the B/C separation, RISK-2026-17) while still covering
/// the +24 px popup displacement (the swap-pressure mechanism,
/// RISK-2026-16). A recalibration of the ratio that moves the reach across
/// either bound invalidates the published scroll/swap comparison and must be
/// re-recorded.
TEST(ObjectTrackerCalibrationTest, FrozenRoiReachSeparatesScrollStepFromPopupDisplacement) {
    ObjectTracker tracker = make_tracker();
    const GrayImage image = noise_image(320, 320);
    const ImageView view = view_of(image);
    const RectF bounds{100.0F, 100.0F, static_cast<float>(kHarnessObjectWidth),
                       static_cast<float>(kHarnessObjectHeight)};
    const auto adopted = tracker.adopt_track(make_region(7U, bounds), view, 1U);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();

    const auto roi = tracker.verification_roi(7U, view);
    ASSERT_TRUE(roi.ok()) << roi.status().message();

    // Maximum integer translation the E1 channel can recover: the window must
    // stay fully inside the clamped ROI, so the per-axis reach is
    // (roi.extent - bounds.extent) / 2. The frozen metering (the header's
    // precision note — M7-09 calibration owns it) yields exactly +-36 px for
    // the 64x32 object at the defaults.
    const int32_t reach_x = (roi.value().width - kHarnessObjectWidth) / 2;
    const int32_t reach_y = (roi.value().height - kHarnessObjectHeight) / 2;
    EXPECT_EQ(reach_x, 36);
    EXPECT_EQ(reach_y, 36);

    // The two published scene mechanics hold against the frozen reach.
    EXPECT_LT(reach_x, kHarnessScrollStepPx) << "scroll step must stay beyond the E1 reach (B/C separation)";
    EXPECT_LT(reach_y, kHarnessScrollStepPx);
    EXPECT_LE(kHarnessPopupDisplacementPx, reach_x)
        << "popup displacement must stay inside the E1 reach (swap pressure)";
    EXPECT_LE(kHarnessPopupDisplacementPx, reach_y);
}

/// The harness caller policy (min_compensation_confidence 0.7, the measured
/// true-scroll vs local-change separation) obeys the frozen gate contract:
/// the local-change confidence band published by the calibration ([0.46,
/// 0.55]) is an explicit, pool-untouched refusal echoing the evaluated
/// estimate, while the true-scroll band ([0.93, 0.95]) applies and keeps the
/// center invariant. The exact inclusive boundary itself is pinned by the
/// M7-07 suite at an exactly-representable threshold (0.5); a float
/// confidence equal to the decimal 0.7 sits below the double option value
/// (0.7f -> 0.699999988... < 0.7), which is representation, not calibration
/// semantics, so this test pins the measured bands instead.
TEST(ObjectTrackerCalibrationTest, HarnessCallerCompensationGate07SeparatesMeasuredBands) {
    ObjectTrackerOptions options;
    options.min_compensation_confidence = 0.7;
    ObjectTracker tracker = make_tracker(options);
    const GrayImage image = noise_image(64, 64);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 16.0F, 8.0F, 8.0F};
    const auto adopted = tracker.adopt_track(make_region(9U, bounds), view, 1U);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    const TargetTrack before = *tracker.find_track(9U);

    // Local-change band (popups, animations): refused, estimate echoed, pool
    // untouched (RULE-06 — nothing drops silently). 0.55 is the band's
    // published upper end.
    const auto refused = tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 0.55F));
    ASSERT_TRUE(refused.ok()) << refused.status().message();
    EXPECT_FALSE(refused.value().applied);
    EXPECT_EQ(refused.value().dx, 4.0F);
    EXPECT_EQ(refused.value().dy, 2.0F);
    EXPECT_TRUE(refused.value().tracks.empty());
    ASSERT_EQ(tracker.find_track(9U)->last_bounds, before.last_bounds);
    ASSERT_EQ(tracker.find_track(9U)->predicted_center, before.predicted_center);

    // True-scroll band: applied. 0.93 is the band's published lower end.
    const auto applied = tracker.compensate_global_motion(shift_of(4.0F, 2.0F, 0.93F));
    ASSERT_TRUE(applied.ok()) << applied.status().message();
    EXPECT_TRUE(applied.value().applied);
    ASSERT_EQ(applied.value().tracks.size(), 1U);
    EXPECT_EQ(applied.value().tracks[0].track_id, 9U);
    const RectF compensated{20.0F, 18.0F, 8.0F, 8.0F};
    EXPECT_EQ(tracker.find_track(9U)->last_bounds, compensated);
    // The frozen center invariant: predicted_center is exactly the new
    // last_bounds center after the translation.
    const PointF expected_center{compensated.x + compensated.width / 2.0F, compensated.y + compensated.height / 2.0F};
    EXPECT_EQ(tracker.find_track(9U)->predicted_center, expected_center);
}

}  // namespace
