// M7-13 deep-enhanced channel conditional fusion: independent verification
// suite for the ObjectTracker injection point and the deep overload of
// `commit_track_evidence` (object-tracking design section 6.2 "enhancement
// channel (optional injection)", RISK-2026-18 gating, DEC-020/DEC-021,
// RULE-06/RULE-12). Written against the frozen header contracts only, with a
// fake TrackerBackend (the fixed-trajectory injection double of the M7-11
// pattern) — the pool never drives the backend, so the suite hands the pool
// exactly what a caller would:
//   * RULE-12 switch (negative zero-change contract): a deep-evidence commit
//     on a tracker with `deep_channel_enabled` off fails kInvalidArgument and
//     leaves the pool untouched; enabling the switch alone (no injection, no
//     deep evidence) is behaviorally and byte-identical to the traditional
//     pipeline.
//   * RISK-2026-18 combination matrix: co-located corroboration upgrades the
//     confidence to the never-lowering max and keeps the confirming level
//     (kConfirmed/kTentative, E2-only included); a conflict (position or
//     confidence) demotes every confirming grade to kPlaceholder so the
//     frozen state machine degrades kTracking -> kUncertain with no position,
//     confidence or template write; non-confirming grades keep their veto
//     semantics; the deep channel never confirms by itself (a corroborated
//     frame on placeholder evidence stays placeholder).
//   * Template protection (高置信模板更新仅取双通道一致帧): on a session-holding
//     track under an enabled switch the positive capture of a kConfirmed
//     commit requires a corroborated frame — a confirmed commit without deep
//     evidence still confirms identity and moves the position but withholds
//     and reports the capture (`template_withheld_by_deep_channel`); a
//     drifted deep channel (conflict) can never grow the template pool;
//     tracks without an injected session keep the frozen M7-06 policy; the
//     M7-08 redetection review commit (no deep evidence by its frozen shape)
//     falls under the protection on session-holding tracks.
//   * Failure disposition (tracker_backend.hpp contract block 3):
//     kBackendFailure -> detach (drop) -> placeholder-commit degradation to
//     kUncertain through the frozen primitives -> fresh initialize on the
//     current bounds re-attached in place -> corroboration recovers kTracking.
//   * Byte budget (RULE-06): `kTrackerHandleSlotOverheadBytes` is accounted
//     in `byte_size()`/`pool_budget_bytes` exactly, an attach that cannot fit
//     fails kBudgetExceeded with the pool untouched, an in-place rebuild is
//     byte-neutral and destroys the old session, and terminate / live-track
//     pool eviction / reset destroy the held sessions synchronously (the
//     evolved two-state form of the M7-11 sentinel
//     OrchestrationAddsZeroPoolBytesUntilTheM713Slot).
//   * DEC-021 untrusted output: non-finite / non-positive / out-of-view deep
//     bounds and a confidence outside [0, 1] are REJECTED with
//     kInvalidArgument, never clamped; validation precedes cancellation
//     (the frozen M7-05/M7-06 order); the [0, 1] and flush-edge boundaries
//     are inclusive.
//   * DOD-04: parameter invalidation is not relaxed by the combination rules
//     — NCC threshold differences still flip grades and a deep-agreement IoU
//     difference flips the disposition on identical inputs.
//   * Determinism (tracker_backend.hpp contract block 8, RULE-03): identical
//     pool input sequences on twin trackers/backends produce bit-identical
//     commits, dispositions and pool state.
//   * DOD-03 coordinate matrix: 0/90/180/270 rotation metadata x odd
//     presented size x non-contiguous stride x flush-edge priors — the pool
//     consumes tracker-space coordinates only, behaves bit-identically across
//     the matrix, the backend observes presented metadata only and the input
//     bytes are never modified (the caller owns the prepared-space recovery).
//   * Privacy (RULE-10/DOD-06): the deep-channel flow writes no files and no
//     Status message carries a pixel-borne marker.
//
// The suite runs the pool side only; backend-side contract execution (the
// validation matrix, cache exemption, per-sequence determinism of the SPI
// itself) stays with the M7-11 orchestration suite.

#include <mirador/tracker_backend.hpp>

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/object_tracker.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using mirador::AppearanceChannelOutcome;
using mirador::BackendInfo;
using mirador::deadline_reached;
using mirador::DeepChannelDisposition;
using mirador::DeepChannelEvidence;
using mirador::ErrorCode;
using mirador::EvidenceGrade;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::is_cancelled;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PixelFormat;
using mirador::PositionScenario;
using mirador::RectF;
using mirador::RectI;
using mirador::Result;
using mirador::Rotation;
using mirador::Status;
using mirador::StructureChannelOutcome;
using mirador::TargetTrack;
using mirador::TrackerBackend;
using mirador::TrackerInitRequest;
using mirador::TrackerSession;
using mirador::TrackerUpdateRequest;
using mirador::TrackerUpdateResult;
using mirador::TrackEvidenceCommit;
using mirador::TrackPositionEvidence;
using mirador::TrackSemantics;
using mirador::TrackState;
using mirador::TrackVerification;
using mirador::VisualRegion;

// --- image helpers (the sibling fusion suites' gray-buffer shape) --------------

struct GrayImage {
    std::vector<std::byte> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int64_t stride = 0;
};

GrayImage make_gray_image(int32_t width, int32_t height, std::byte fill, int64_t stride_padding_bytes = 0) {
    GrayImage image;
    image.width = width;
    image.height = height;
    image.stride = static_cast<int64_t>(width) + stride_padding_bytes;
    image.pixels.assign(static_cast<size_t>(image.stride) * static_cast<size_t>(height), fill);
    return image;
}

uint8_t noise_pixel(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393U + static_cast<uint32_t>(y) * 668265263U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    return static_cast<uint8_t>((h ^ (h >> 16U)) % 256U);
}

GrayImage make_noise_image(int32_t width, int32_t height, int64_t stride_padding_bytes = 0) {
    GrayImage image = make_gray_image(width, height, std::byte{0}, stride_padding_bytes);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.stride) + static_cast<size_t>(x)] =
                std::byte{noise_pixel(x, y)};
        }
    }
    return image;
}

ImageView view_of(const GrayImage& image, Rotation rotation = Rotation::k0) {
    ImageView view;
    view.data = image.pixels.data();
    view.width = image.width;
    view.height = image.height;
    view.row_stride_bytes = image.stride;
    view.format = PixelFormat::kGray8;
    view.rotation = rotation;
    return view;
}

/// FNV-1a over the whole buffer (content and stride padding) for the
/// input-immutability negatives.
uint64_t buffer_hash(const GrayImage& image) {
    uint64_t hash = 14695981039346656037ULL;
    for (const std::byte byte : image.pixels) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

ExecutionContext clean_context() {
    return {};
}

ExecutionContext cancelled_context() {
    ExecutionContext context;
    context.is_cancelled = [] { return true; };
    return context;
}

ExecutionContext expired_deadline_context() {
    ExecutionContext context;
    context.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    return context;
}

// --- the fake tracker backend (M7-11 verification-double pattern) ---------------

/// Shared destruction observability: the counters survive any destruction
/// order without the handle holding a backend reference (contract block 1).
struct SessionCounters {
    int alive = 0;
    int created = 0;
    int destroyed = 0;
};

struct FakeTrackerSession final : TrackerSession {
    FakeTrackerSession(std::shared_ptr<SessionCounters> shared_counters, std::vector<TrackerUpdateResult> fixed,
                       bool echo_presented_bytes)
        : trajectory(std::move(fixed)), pixel_echo(echo_presented_bytes), counters(std::move(shared_counters)) {
        ++counters->created;
        ++counters->alive;
    }

    ~FakeTrackerSession() override {
        --counters->alive;
        ++counters->destroyed;
    }

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes): test double.
    /// Knobs: an injected status (the kBackendFailure shape) wins over the
    /// trajectory; a fixed trajectory replays one result per successful
    /// update and reports kBackendFailure once exhausted; pixel-echo mode
    /// derives the result from the presented bytes (stride-honoring, see
    /// compute_result).
    Status injected_status;
    std::vector<TrackerUpdateResult> trajectory;
    bool pixel_echo = false;
    std::shared_ptr<SessionCounters> counters;

    // Observations.
    int update_count = 0;
    int success_count = 0;
    RectF last_prior;
    Rotation last_rotation = Rotation::k0;
    int32_t last_width = 0;
    int32_t last_height = 0;
    int64_t last_stride = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    Result<TrackerUpdateResult> update(const ImageView& prepared_image, const TrackerUpdateRequest& request,
                                       const ExecutionContext& context) override {
        ++update_count;
        last_prior = request.prior_bounds;
        last_rotation = prepared_image.rotation;
        last_width = prepared_image.width;
        last_height = prepared_image.height;
        last_stride = prepared_image.row_stride_bytes;
        if (!injected_status.ok()) {
            return injected_status;
        }
        if (is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "fake-tracker observed cancellation");
        }
        if (deadline_reached(context)) {
            return Status(ErrorCode::kTimeout, "fake-tracker observed deadline");
        }
        const TrackerUpdateResult result = compute_result(prepared_image, request.prior_bounds);
        ++success_count;
        return result;
    }

private:
    /// Fixed mode replays `trajectory[success_count]` (kBackendFailure once
    /// exhausted — never a stale repeat, contract block 3); pixel-echo mode
    /// derives the result deterministically from the presented bytes with
    /// integer arithmetic (bit-identical on every replay, contract block 8).
    [[nodiscard]] TrackerUpdateResult compute_result(const ImageView& image, const RectF& prior) const {
        TrackerUpdateResult result;
        if (pixel_echo) {
            const auto x0 = std::max(0, static_cast<int32_t>(prior.x));
            const auto y0 = std::max(0, static_cast<int32_t>(prior.y));
            const auto x1 = std::min(image.width, static_cast<int32_t>(prior.x + prior.width));
            const auto y1 = std::min(image.height, static_cast<int32_t>(prior.y + prior.height));
            uint64_t sum = 0;
            for (int32_t y = y0; y < y1; ++y) {
                const std::byte* row = image.data + static_cast<int64_t>(y) * image.row_stride_bytes;
                for (int32_t x = x0; x < x1; ++x) {
                    sum += static_cast<uint8_t>(row[x]);
                }
            }
            // A small content-driven displacement, clamped back inside the
            // image so even a flush-edge prior yields fully inside bounds
            // (contract block 6); the reported confidence stays in [0, 1].
            const float dx = std::min(static_cast<float>(sum % 5U) - 2.0F,
                                      static_cast<float>(image.width) - (prior.x + prior.width));
            const float dy =
                std::min(static_cast<float>(sum % 2U), static_cast<float>(image.height) - (prior.y + prior.height));
            result.bounds = RectF{prior.x + dx, prior.y + dy, prior.width, prior.height};
            result.confidence = 0.5F + static_cast<float>(sum % 40U) / 100.0F;
        } else {
            result = trajectory[static_cast<size_t>(success_count) % trajectory.size()];
        }
        return result;
    }
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): test double,
// knobs and observations are public by design (the fake_backends.hpp convention).
struct FakeTrackerBackend final : TrackerBackend {
    BackendInfo info_value;
    std::vector<TrackerUpdateResult> trajectory;
    bool pixel_echo = false;
    std::shared_ptr<SessionCounters> counters = std::make_shared<SessionCounters>();
    int init_count = 0;

    FakeTrackerBackend() {
        info_value.name = "fake-deep-tracker";
        info_value.implementation_version = "1.0.0";
        info_value.model_id = "fake-deep-model";
        info_value.model_revision = "r1";
        info_value.accepted_formats = {PixelFormat::kGray8};
        info_value.thread_safe = false;
        trajectory = {{RectF{2.0F, 1.0F, 8.0F, 6.0F}, 0.90F}, {RectF{3.0F, 2.0F, 8.0F, 6.0F}, 0.85F}};
    }

    [[nodiscard]] BackendInfo info() const override { return info_value; }

    Result<std::unique_ptr<TrackerSession>> initialize(const ImageView& /*prepared_image*/,
                                                       const TrackerInitRequest& /*request*/,
                                                       const ExecutionContext& /*context*/) override {
        ++init_count;
        return {std::make_unique<FakeTrackerSession>(counters, trajectory, pixel_echo)};
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

FakeTrackerSession& as_fake(TrackerSession& session) {
    return *static_cast<FakeTrackerSession*>(&session);
}

// --- pool helpers (the frozen ObjectTracker primitives, as in the M7-06 suite) --

VisualRegion make_region(uint64_t stable_id, RectF bounds, float confidence = 0.9F, std::string label = {}) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.confidence = confidence;
    region.label = std::move(label);
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

void adopt_or_fail(ObjectTracker& tracker, const ImageView& view, uint64_t id, const RectF& bounds,
                   uint64_t frame_sequence = 1, std::string label = {}) {
    const auto adopted = tracker.adopt_track(make_region(id, bounds, 0.9F, std::move(label)), view, frame_sequence);
    ASSERT_TRUE(adopted.ok()) << "adopt " << id << ": " << adopted.status().message();
}

/// Attaches a fresh fake session to `track_id`, initializing it on the
/// caller's backend with the track's current bounds (the M7-13 caller-side
/// wiring shape).
void attach_or_fail(ObjectTracker& tracker, FakeTrackerBackend& backend, const ImageView& view, uint64_t track_id) {
    const TargetTrack* track = tracker.find_track(track_id);
    ASSERT_NE(track, nullptr);
    auto initialized = backend.initialize(view, TrackerInitRequest{track->last_bounds, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    const auto attached = tracker.attach_tracker_session(track_id, initialized.take_value());
    ASSERT_TRUE(attached.ok()) << attached.status().message();
}

/// Hand-built caller evidence (the commit is contractually forbidden from
/// re-running the scan), as in the M7-06 suite.
TrackVerification verification_of(uint64_t track_id, AppearanceChannelOutcome appearance, int32_t offset_dx = 0,
                                  int32_t offset_dy = 0, double peak_ncc = 0.9,
                                  StructureChannelOutcome structure = StructureChannelOutcome::kNotSupplied) {
    TrackVerification verification;
    verification.track_id = track_id;
    verification.state = TrackState::kTracking;
    verification.verification_roi = RectI{0, 0, 1, 1};
    verification.appearance.outcome = appearance;
    verification.appearance.peak_ncc = peak_ncc;
    verification.appearance.peak_sidelobe_ratio = 10.0;
    verification.appearance.best_template_index = 0U;
    verification.appearance.best_offset_dx = offset_dx;
    verification.appearance.best_offset_dy = offset_dy;
    verification.structure.outcome = structure;
    return verification;
}

TrackPositionEvidence position_of(PositionScenario scenario, bool inside_gate) {
    TrackPositionEvidence position;
    position.scenario = scenario;
    position.inside_gate = inside_gate;
    return position;
}

DeepChannelEvidence deep_of(RectF bounds, float confidence) {
    DeepChannelEvidence evidence;
    evidence.bounds = bounds;
    evidence.confidence = confidence;
    return evidence;
}

/// Observable pool state for atomicity/zero-change comparisons.
struct PoolSnapshot {
    size_t track_count = 0;
    int64_t used_bytes = 0;
    std::vector<uint64_t> ids;
    std::vector<TrackState> states;
    std::vector<RectF> bounds;
    std::vector<float> confidences;
    std::vector<size_t> template_counts;
    std::vector<uint64_t> last_verified;
};

PoolSnapshot snapshot_of(const ObjectTracker& tracker) {
    PoolSnapshot snapshot;
    snapshot.track_count = tracker.track_count();
    snapshot.used_bytes = tracker.byte_size();
    for (const uint64_t id : tracker.track_ids()) {
        const TargetTrack* track = tracker.find_track(id);
        snapshot.ids.push_back(track->track_id);
        snapshot.states.push_back(track->state);
        snapshot.bounds.push_back(track->last_bounds);
        snapshot.confidences.push_back(track->confidence);
        snapshot.template_counts.push_back(track->templates.size());
        snapshot.last_verified.push_back(track->last_verified_sequence);
    }
    return snapshot;
}

bool identical(const PoolSnapshot& lhs, const PoolSnapshot& rhs) {
    return lhs.track_count == rhs.track_count && lhs.used_bytes == rhs.used_bytes && lhs.ids == rhs.ids &&
           lhs.states == rhs.states && lhs.bounds == rhs.bounds && lhs.confidences == rhs.confidences &&
           lhs.template_counts == rhs.template_counts && lhs.last_verified == rhs.last_verified;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

RectF offset_bounds(const RectF& bounds, int32_t dx, int32_t dy) {
    return RectF{bounds.x + static_cast<float>(dx), bounds.y + static_cast<float>(dy), bounds.width, bounds.height};
}

// --- RULE-12 switch and the zero-change contract --------------------------------

TEST(ObjectTrackerDeepChannelTest, DeepCommitRequiresTheCallerSwitch) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTracker tracker = make_tracker();  // deep_channel_enabled defaults to false
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    const PoolSnapshot before = snapshot_of(tracker);

    // A deep-evidence commit with the switch off is a caller programming
    // error: explicit kInvalidArgument, never silently ignored (the
    // zero-change contract), pool completely untouched.
    const auto rejected =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                      deep_of(offset_bounds(bounds, 2, 1), 0.9F));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_TRUE(identical(before, snapshot_of(tracker)));

    // The traditional overload keeps working unchanged on the same tracker.
    const auto traditional =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(traditional.ok()) << traditional.status().message();
    EXPECT_EQ(traditional.value().deep_disposition, DeepChannelDisposition::kNotSupplied);
    EXPECT_FALSE(traditional.value().template_withheld_by_deep_channel);
    EXPECT_EQ(traditional.value().grade, EvidenceGrade::kConfirmed);
}

TEST(ObjectTrackerDeepChannelTest, EnablingTheSwitchAloneChangesNothing) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions enabled_options;
    enabled_options.deep_channel_enabled = true;

    ObjectTracker traditional = make_tracker();
    ObjectTracker enabled = make_tracker(enabled_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(traditional, view, 7U, bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(enabled, view, 7U, bounds));

    // An identical operation sequence through the traditional overload only:
    // every commit echo and every pool checkpoint is bit-identical whether
    // the switch is on or off (RULE-12: the core hardcodes no policy, and
    // enabling the combination rules without supplying evidence is a no-op).
    for (const uint64_t sequence : {2U, 3U, 4U}) {
        const auto traditional_commit = traditional.commit_track_evidence(
            7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
            position_of(PositionScenario::kStationary, true), std::nullopt, view, sequence);
        const auto enabled_commit = enabled.commit_track_evidence(
            7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
            position_of(PositionScenario::kStationary, true), std::nullopt, view, sequence);
        ASSERT_TRUE(traditional_commit.ok()) << traditional_commit.status().message();
        ASSERT_TRUE(enabled_commit.ok()) << enabled_commit.status().message();
        EXPECT_EQ(traditional_commit.value().grade, enabled_commit.value().grade);
        EXPECT_EQ(traditional_commit.value().state, enabled_commit.value().state);
        EXPECT_EQ(traditional_commit.value().deep_disposition, enabled_commit.value().deep_disposition);
        EXPECT_EQ(traditional_commit.value().template_captured, enabled_commit.value().template_captured);
        EXPECT_EQ(traditional_commit.value().template_withheld_by_deep_channel,
                  enabled_commit.value().template_withheld_by_deep_channel);
        EXPECT_TRUE(identical(snapshot_of(traditional), snapshot_of(enabled)));
    }
}

// --- DEC-021 adoption-point validation (rejected, never clamped) ----------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one assert block per rejected shape
TEST(ObjectTrackerDeepChannelTest, DeepValidationRejectsUntrustedOutput) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto verification = verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85);
    const auto position = position_of(PositionScenario::kStationary, true);

    struct BadCase {
        std::string what;
        RectF deep_bounds;
        float confidence;
    };
    const std::vector<BadCase> bad_cases = {
        {"nan x", RectF{nan, 13.0F, 16.0F, 12.0F}, 0.9F},
        {"inf width", RectF{18.0F, 13.0F, inf, 12.0F}, 0.9F},
        {"zero width", RectF{18.0F, 13.0F, 0.0F, 12.0F}, 0.9F},
        {"negative height", RectF{18.0F, 13.0F, 16.0F, -1.0F}, 0.9F},
        {"negative x", RectF{-0.5F, 13.0F, 16.0F, 12.0F}, 0.9F},
        {"overflows right edge", RectF{49.0F, 13.0F, 16.0F, 12.0F}, 0.9F},
        {"overflows bottom edge", RectF{18.0F, 37.0F, 16.0F, 12.0F}, 0.9F},
        {"nan confidence", RectF{18.0F, 13.0F, 16.0F, 12.0F}, nan},
        {"confidence above one", RectF{18.0F, 13.0F, 16.0F, 12.0F}, 1.25F},
        {"negative confidence", RectF{18.0F, 13.0F, 16.0F, 12.0F}, -0.25F},
        {"infinite confidence", RectF{18.0F, 13.0F, 16.0F, 12.0F}, inf},
    };
    for (const BadCase& bad : bad_cases) {
        const PoolSnapshot before = snapshot_of(tracker);
        const auto rejected = tracker.commit_track_evidence(7U, verification, position, std::nullopt, view, 2,
                                                            deep_of(bad.deep_bounds, bad.confidence));
        EXPECT_FALSE(rejected.ok()) << bad.what << ": deep evidence must be rejected";
        if (!rejected.ok()) {
            EXPECT_EQ(rejected.status().code(), ErrorCode::kInvalidArgument) << bad.what;
        }
        EXPECT_TRUE(identical(before, snapshot_of(tracker))) << bad.what << ": pool must stay untouched";
    }

    // The inclusive boundaries are accepted: confidence exactly 0 and 1, and
    // bounds flush with every presented edge (DOD-03 flush edge).
    const std::vector<DeepChannelEvidence> edge_cases = {
        deep_of(offset_bounds(bounds, 2, 1), 0.0F),      deep_of(offset_bounds(bounds, 2, 1), 1.0F),
        deep_of(RectF{0.0F, 0.0F, 18.0F, 13.0F}, 0.9F),  deep_of(RectF{46.0F, 0.0F, 18.0F, 13.0F}, 0.9F),
        deep_of(RectF{0.0F, 35.0F, 18.0F, 13.0F}, 0.9F), deep_of(RectF{46.0F, 35.0F, 18.0F, 13.0F}, 0.9F),
    };
    uint64_t sequence = 2;
    for (const DeepChannelEvidence& edge : edge_cases) {
        const auto accepted =
            tracker.commit_track_evidence(7U, verification, position, std::nullopt, view, sequence, edge);
        ASSERT_TRUE(accepted.ok()) << accepted.status().message();
        ++sequence;
    }
}

TEST(ObjectTrackerDeepChannelTest, DeepValidationPrecedesCancellation) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    const PoolSnapshot before = snapshot_of(tracker);

    const auto verification = verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85);
    const auto position = position_of(PositionScenario::kStationary, true);
    const RectF conflicting{40.0F, 30.0F, 8.0F, 6.0F};

    // The frozen ordering (the M7-05/M7-06 pool precedent): a malformed deep
    // evidence reports kInvalidArgument even under an already-cancelled or
    // expired context; only fully valid calls report the cancellation.
    const auto invalid_cancelled =
        tracker.commit_track_evidence(7U, verification, position, std::nullopt, view, 2,
                                      deep_of(RectF{18.0F, 13.0F, 0.0F, 12.0F}, 0.9F), cancelled_context());
    EXPECT_FALSE(invalid_cancelled.ok());
    EXPECT_EQ(invalid_cancelled.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_TRUE(identical(before, snapshot_of(tracker)));

    const auto invalid_expired =
        tracker.commit_track_evidence(7U, verification, position, std::nullopt, view, 2,
                                      deep_of(RectF{18.0F, 13.0F, 16.0F, 12.0F}, 1.25F), expired_deadline_context());
    EXPECT_FALSE(invalid_expired.ok());
    EXPECT_EQ(invalid_expired.status().code(), ErrorCode::kInvalidArgument);

    const auto cancelled = tracker.commit_track_evidence(7U, verification, position, std::nullopt, view, 2,
                                                         deep_of(conflicting, 0.9F), cancelled_context());
    EXPECT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.status().code(), ErrorCode::kCancelled);
    EXPECT_TRUE(identical(before, snapshot_of(tracker)));

    const auto timed_out = tracker.commit_track_evidence(7U, verification, position, std::nullopt, view, 2,
                                                         deep_of(conflicting, 0.9F), expired_deadline_context());
    EXPECT_FALSE(timed_out.ok());
    EXPECT_EQ(timed_out.status().code(), ErrorCode::kTimeout);
    EXPECT_TRUE(identical(before, snapshot_of(tracker)));
}

// --- RISK-2026-18 combination matrix --------------------------------------------

TEST(ObjectTrackerDeepChannelTest, CoLocatedCorroborationUpgradesConfidenceKeepsLevel) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));

    // E1 strong at offset (2, 1): the candidate window is the track bounds
    // translated by the E1 best offset (the frozen M7-06 candidate rule).
    const RectF candidate = offset_bounds(bounds, 2, 1);
    const auto commit = tracker.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(candidate, 0.9F));
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(commit.value().state, TrackState::kTracking);
    // Confidence upgraded to the never-lowering max of this frame's
    // traditional source (the clamped E1 peak) and the deep confidence.
    EXPECT_FLOAT_EQ(tracker.find_track(7U)->confidence, 0.9F);
    // Position still follows the E1 candidate window — the deep channel never
    // moves the estimate itself.
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, candidate);
    EXPECT_TRUE(commit.value().template_captured);
    EXPECT_FALSE(commit.value().template_withheld_by_deep_channel);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 2U);

    // A deep confidence below the traditional source never lowers the frame's
    // upgrade below it: max(E1 peak, deep) — here 0.8 stays 0.8.
    const auto weaker =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 1, 0, 0.8),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3,
                                      deep_of(offset_bounds(candidate, 1, 0), 0.6F));
    ASSERT_TRUE(weaker.ok()) << weaker.status().message();
    EXPECT_EQ(weaker.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_FLOAT_EQ(tracker.find_track(7U)->confidence, 0.8F);
}

TEST(ObjectTrackerDeepChannelTest, CorroborationKeepsTentativeLevelAndUpgradesConfidence) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));

    // E1 weak inside the gate -> kTentative; corroboration keeps the level
    // and upgrades the confidence (a confirming grade, never lifted or
    // lowered by the deep side).
    const RectF candidate = offset_bounds(bounds, 1, 0);
    const auto commit = tracker.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kWeak, 1, 0, 0.65),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(candidate, 0.88F));
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kTentative);
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);
    EXPECT_FLOAT_EQ(tracker.find_track(7U)->confidence, 0.88F);
    // kTentative never writes templates, so there is nothing to withhold.
    EXPECT_FALSE(commit.value().template_captured);
    EXPECT_FALSE(commit.value().template_withheld_by_deep_channel);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 1U);
}

TEST(ObjectTrackerDeepChannelTest, E2OnlyConfirmationCorroborationUpgradesPriorConfidence) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));

    // E2-only confirmation: no appearance channel outcome, so the candidate
    // window is the unchanged bounds — corroboration compares against it and
    // upgrades the kept prior confidence the same never-lowering way.
    const auto commit = tracker.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kNone, 0, 0, 0.0, StructureChannelOutcome::kConsistent),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(bounds, 0.95F));
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_FLOAT_EQ(tracker.find_track(7U)->confidence, 0.95F);
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, bounds);
    EXPECT_TRUE(commit.value().template_captured);
    EXPECT_FALSE(commit.value().template_withheld_by_deep_channel);
}

TEST(ObjectTrackerDeepChannelTest, ConflictDemotesConfirmedGradeToPlaceholder) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));

    // Drifted deep tracker: a far-away box with HIGH confidence still
    // conflicts (position side of the frozen table) — the conservative side
    // demotes the confirming grade to kPlaceholder, and the frozen state
    // machine degrades kTracking -> kUncertain with nothing else moving.
    const PoolSnapshot before = snapshot_of(tracker);
    const auto commit =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                      deep_of(RectF{40.0F, 30.0F, 8.0F, 6.0F}, 0.99F));
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kConflict);
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(commit.value().state, TrackState::kUncertain);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kUncertain);
    // No position move, no confidence write, no capture — the placeholder
    // semantics of the frozen M7-06 machine.
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, bounds);
    EXPECT_FLOAT_EQ(tracker.find_track(7U)->confidence, before.confidences[0]);
    EXPECT_FALSE(commit.value().template_captured);
    EXPECT_FALSE(commit.value().template_withheld_by_deep_channel);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 1U);
    EXPECT_EQ(tracker.find_track(7U)->last_verified_sequence, before.last_verified[0]);

    // The same demotion for a kTentative confirming grade.
    ObjectTrackerOptions tentative_options;
    tentative_options.deep_channel_enabled = true;
    ObjectTracker tentative_tracker = make_tracker(tentative_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tentative_tracker, view, 9U, bounds));
    const auto tentative =
        tentative_tracker.commit_track_evidence(9U, verification_of(9U, AppearanceChannelOutcome::kWeak, 1, 0, 0.65),
                                                position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                                deep_of(RectF{40.0F, 30.0F, 8.0F, 6.0F}, 0.99F));
    ASSERT_TRUE(tentative.ok()) << tentative.status().message();
    EXPECT_EQ(tentative.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(tentative.value().deep_disposition, DeepChannelDisposition::kConflict);
    EXPECT_EQ(tentative_tracker.find_track(9U)->state, TrackState::kUncertain);
}

TEST(ObjectTrackerDeepChannelTest, ConflictByLowConfidenceDemotesToo) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    // Perfect co-location but the deep confidence sits below the floor: the
    // confidence side of the frozen conflict rule fires.
    const auto commit =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                      deep_of(offset_bounds(bounds, 2, 1), 0.49F));
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kConflict);
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kUncertain);
}

TEST(ObjectTrackerDeepChannelTest, DeepChannelNeverConfirmsByItself) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    // Placeholder evidence + perfect corroboration (IoU 1.0, confidence 1.0):
    // the deep channel cannot lift the grade — no lift, no confidence write,
    // the frozen machine still degrades the track on the placeholder grade.
    const PoolSnapshot before = snapshot_of(tracker);
    const auto commit = tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                                      position_of(PositionScenario::kStationary, true), std::nullopt,
                                                      view, 2, deep_of(bounds, 1.0F));
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kUncertain);
    EXPECT_FLOAT_EQ(tracker.find_track(7U)->confidence, before.confidences[0]);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 1U);
    EXPECT_FALSE(commit.value().template_captured);
}

TEST(ObjectTrackerDeepChannelTest, VetoedGradeKeepsSemanticsUnderDeepEvidence) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds, 1, "button"));

    // Semantic conflict -> kVetoed; corroborated deep evidence must not
    // touch the veto semantics (non-confirming grades stay untouched): the
    // negative-template collection still runs, no positive capture happens.
    TrackSemantics candidate;
    candidate.label = "icon";
    const auto commit = tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                                      position_of(PositionScenario::kStationary, true), candidate, view,
                                                      2, deep_of(bounds, 0.95F));
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kVetoed);
    EXPECT_TRUE(commit.value().semantics_conflict);
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_TRUE(commit.value().negative_template_captured);
    EXPECT_FALSE(commit.value().template_captured);
    EXPECT_EQ(tracker.find_track(7U)->negative_templates.size(), 1U);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kUncertain);
}

// --- template protection (高置信模板更新仅取双通道一致帧) -------------------------

TEST(ObjectTrackerDeepChannelTest, TemplateWithheldOnSessionTrackWithoutDeepEvidence) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));

    // A confirmed commit whose deep evidence is missing this frame (caller
    // skip here; backend failure/cancel in the field) still confirms
    // identity — the graceful-degradation rule — but the capture is withheld
    // and reported (RULE-06: never silent).
    const auto commit =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(commit.value().state, TrackState::kTracking);
    EXPECT_EQ(commit.value().deep_disposition, DeepChannelDisposition::kNotSupplied);
    EXPECT_TRUE(commit.value().template_withheld_by_deep_channel);
    EXPECT_FALSE(commit.value().template_captured);
    // Identity still progresses: the position follows the candidate window
    // and the verification sequence advances.
    EXPECT_EQ(tracker.find_track(7U)->last_bounds, offset_bounds(bounds, 2, 1));
    EXPECT_EQ(tracker.find_track(7U)->last_verified_sequence, 2U);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 1U);
}

TEST(ObjectTrackerDeepChannelTest, CorroboratedFramesCaptureWithheldFramesNeverPollute) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));

    // Frame 2: corroborated confirm -> the dual-channel-consistent frame is
    // the only one allowed to update the template pool.
    const auto corroborated =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                      deep_of(offset_bounds(bounds, 2, 1), 0.9F));
    ASSERT_TRUE(corroborated.ok()) << corroborated.status().message();
    EXPECT_TRUE(corroborated.value().template_captured);
    EXPECT_FALSE(corroborated.value().template_withheld_by_deep_channel);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 2U);

    // Frames 3-5: confirmed but no deep evidence -> withheld every time; the
    // template pool never grows (the drift guard: a deep channel that keeps
    // failing cannot pollute the appearance store).
    for (const uint64_t sequence : {3U, 4U, 5U}) {
        const auto withheld = tracker.commit_track_evidence(
            7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 0, 0, 0.85),
            position_of(PositionScenario::kStationary, true), std::nullopt, view, sequence);
        ASSERT_TRUE(withheld.ok()) << withheld.status().message();
        EXPECT_EQ(withheld.value().grade, EvidenceGrade::kConfirmed);
        EXPECT_TRUE(withheld.value().template_withheld_by_deep_channel) << "sequence " << sequence;
        EXPECT_FALSE(withheld.value().template_captured);
        EXPECT_EQ(tracker.find_track(7U)->templates.size(), 2U) << "sequence " << sequence;
    }
    // And a drifted (conflicting) confirmed-in frame demotes before the
    // capture question even arises: still no capture, no withholding echo.
    const auto conflicted =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 0, 0, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 6,
                                      deep_of(RectF{48.0F, 36.0F, 8.0F, 6.0F}, 0.99F));
    ASSERT_TRUE(conflicted.ok()) << conflicted.status().message();
    EXPECT_EQ(conflicted.value().grade, EvidenceGrade::kPlaceholder);
    EXPECT_FALSE(conflicted.value().template_withheld_by_deep_channel);
    EXPECT_FALSE(conflicted.value().template_captured);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 2U);
}

TEST(ObjectTrackerDeepChannelTest, TemplateProtectionNeedsAnInjectedSession) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    // Switch on but no session attached: the frozen M7-06 capture policy
    // applies — a confirmed commit captures without any deep evidence.
    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));

    const auto commit =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_FALSE(commit.value().template_withheld_by_deep_channel);
    EXPECT_TRUE(commit.value().template_captured);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 2U);
}

TEST(ObjectTrackerDeepChannelTest, RedetectionReviewCommitFallsUnderTemplateProtection) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    // The M7-08 identity review keeps its frozen shape (its commits carry no
    // deep evidence); on a session-holding track under an enabled switch the
    // review's confirming capture falls under the protection (the frozen
    // scope note) while the recapture itself — the kLost -> kTracking edge —
    // still goes through.
    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    options.uncertain_frame_limit = 1;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));

    // One placeholder commit with the limit 1 -> kLost.
    const auto loss =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(loss.ok()) << loss.status().message();
    ASSERT_EQ(tracker.find_track(7U)->state, TrackState::kLost);

    // The caller's redetection identity review: strong appearance, the stale
    // prior does not gate a kLost track (frozen M7-06), no deep evidence.
    const auto review =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 0, 0, 0.9),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3);
    ASSERT_TRUE(review.ok()) << review.status().message();
    EXPECT_EQ(review.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(review.value().state, TrackState::kTracking);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);
    EXPECT_TRUE(review.value().template_withheld_by_deep_channel);
    EXPECT_FALSE(review.value().template_captured);
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 1U);
}

// --- failure disposition (tracker_backend.hpp contract block 3) ------------------

TEST(ObjectTrackerDeepChannelTest, BackendFailureDisposalDegradesAndRebuildRecovers) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));
    TrackerSession& session = *tracker.tracker_session(7U);

    // kBackendFailure: the session must be treated as unusable — the caller
    // destroys it via detach (the slot releases), and the track degrades
    // through the existing frozen placeholder-commit path (no new API).
    as_fake(session).injected_status = Status(ErrorCode::kBackendFailure, "fake-tracker corrupted");
    const auto failed = session.update(view, TrackerUpdateRequest{bounds, {}}, clean_context());
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.status().code(), ErrorCode::kBackendFailure);
    const int64_t attached_bytes = tracker.byte_size();
    std::unique_ptr<TrackerSession> discarded = tracker.detach_tracker_session(7U);
    ASSERT_NE(discarded, nullptr);
    discarded.reset();  // the explicit discard
    EXPECT_EQ(backend.counters->alive, 0);
    EXPECT_EQ(tracker.byte_size(), attached_bytes - ObjectTracker::kTrackerHandleSlotOverheadBytes);
    EXPECT_EQ(tracker.tracker_session(7U), nullptr);

    const auto degraded =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(degraded.ok()) << degraded.status().message();
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kUncertain);
    // The degradation allocates the M7-06 state-machine slot (the frozen
    // bookkeeping of the insufficient streak).
    const int64_t degraded_bytes =
        attached_bytes - ObjectTracker::kTrackerHandleSlotOverheadBytes + ObjectTracker::kStateSlotOverheadBytes;
    EXPECT_EQ(tracker.byte_size(), degraded_bytes);

    // Recovery: a fresh initialize on the current frame's bounds, re-attached
    // (a new slot), and the next corroborated commit restores kTracking.
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));
    ASSERT_NE(tracker.tracker_session(7U), nullptr);
    EXPECT_EQ(tracker.byte_size(), degraded_bytes + ObjectTracker::kTrackerHandleSlotOverheadBytes);
    const auto recovered =
        tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                      position_of(PositionScenario::kStationary, true), std::nullopt, view, 3,
                                      deep_of(offset_bounds(bounds, 2, 1), 0.9F));
    ASSERT_TRUE(recovered.ok()) << recovered.status().message();
    EXPECT_EQ(recovered.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(recovered.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTracking);
    // The failure frame supplied no deep evidence and no capture happened on
    // it: the corruption window wrote no templates.
    EXPECT_EQ(tracker.find_track(7U)->templates.size(), 2U);
}

// --- byte budget (RULE-06): slot accounting, explicit rejection, lifecycle -------

TEST(ObjectTrackerDeepChannelTest, HandleSlotAccountingAndExplicitBudgetRejection) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    // Measure the not-injected baseline on a default tracker first.
    ObjectTracker baseline = make_tracker();
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(baseline, view, 7U, bounds));
    const int64_t base_bytes = baseline.byte_size();

    // One byte short: the attach fails explicitly and the pool is untouched.
    ObjectTrackerOptions tight_options;
    tight_options.deep_channel_enabled = true;
    tight_options.pool_budget_bytes = base_bytes + ObjectTracker::kTrackerHandleSlotOverheadBytes - 1;
    ObjectTracker tight = make_tracker(tight_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tight, view, 7U, bounds));
    FakeTrackerBackend tight_backend;
    const TargetTrack* tight_track = tight.find_track(7U);
    auto rejected_init =
        tight_backend.initialize(view, TrackerInitRequest{tight_track->last_bounds, {}}, clean_context());
    ASSERT_TRUE(rejected_init.ok()) << rejected_init.status().message();
    TrackerSession* rejected_session = rejected_init.value().get();
    const auto rejected = tight.attach_tracker_session(7U, rejected_init.take_value());
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(tight.byte_size(), base_bytes);
    EXPECT_EQ(tight.tracker_session(7U), nullptr);
    EXPECT_EQ(tight.detach_tracker_session(7U), nullptr);
    (void)rejected_session;  // fate probed separately; see the issue report

    // Exactly the slot constant: the attach fits.
    ObjectTrackerOptions fits_options = tight_options;
    fits_options.pool_budget_bytes = base_bytes + ObjectTracker::kTrackerHandleSlotOverheadBytes;
    ObjectTracker fits = make_tracker(fits_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(fits, view, 7U, bounds));
    FakeTrackerBackend fits_backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(fits, fits_backend, view, 7U));
    EXPECT_EQ(fits.byte_size(), base_bytes + ObjectTracker::kTrackerHandleSlotOverheadBytes);
    EXPECT_NE(fits.tracker_session(7U), nullptr);
    // Detach frees the slot bytes exactly.
    std::unique_ptr<TrackerSession> removed = fits.detach_tracker_session(7U);
    ASSERT_NE(removed, nullptr);
    removed.reset();
    EXPECT_EQ(fits.byte_size(), base_bytes);
    EXPECT_EQ(fits_backend.counters->alive, 0);
}

TEST(ObjectTrackerDeepChannelTest, InPlaceRebuildIsByteNeutralAndDestroysTheOldSession) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));
    TrackerSession* first = tracker.tracker_session(7U);
    const int64_t attached_bytes = tracker.byte_size();

    // A session rebuild replaces the handle in place (destroy old, store
    // new): byte-neutral, exactly one session alive afterwards, and the pool
    // hands out the new handle.
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));
    EXPECT_EQ(tracker.byte_size(), attached_bytes);
    TrackerSession* second = tracker.tracker_session(7U);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second, first);
    EXPECT_EQ(backend.counters->alive, 1);
    EXPECT_EQ(backend.counters->destroyed, 1);
    EXPECT_EQ(backend.counters->created, 2);
    EXPECT_EQ(backend.init_count, 2);
}

TEST(ObjectTrackerDeepChannelTest, HandleDestroyedOnTerminateEvictionAndReset) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};
    const RectF small{4.0F, 4.0F, 8.0F, 6.0F};

    // Termination path.
    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker terminator = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(terminator, view, 7U, bounds));
    const int64_t adopt_bytes = terminator.byte_size();
    FakeTrackerBackend backend_a;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(terminator, backend_a, view, 7U));
    const int64_t attached_bytes = terminator.byte_size();
    const auto ended = terminator.terminate(7U, 2);
    ASSERT_TRUE(ended.ok()) << ended.status().message();
    EXPECT_EQ(backend_a.counters->alive, 0);
    // Termination releases the handle slot AND the archived track's evidence
    // stores (templates, history) — measure the archive-only residue on a
    // twin to pin the byte account exactly.
    ObjectTracker archive_twin = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(archive_twin, view, 7U, bounds));
    ASSERT_TRUE(archive_twin.terminate(7U, 2).ok());
    EXPECT_EQ(terminator.byte_size(), attached_bytes - ObjectTracker::kTrackerHandleSlotOverheadBytes -
                                          (adopt_bytes - archive_twin.byte_size()));
    EXPECT_EQ(terminator.tracker_session(7U), nullptr);
    EXPECT_EQ(terminator.detach_tracker_session(7U), nullptr);

    // Track-bytes meter for the eviction arithmetic below: an identical
    // region on a slot-free tracker.
    ObjectTracker measurer = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(measurer, view, 1U, small));
    const int64_t track_bytes = measurer.byte_size();

    // Live-track eviction path: max_targets 2, adopting a third track evicts
    // the oldest live track (equal verification sequences -> lower id first)
    // and destroys its session synchronously.
    ObjectTrackerOptions evict_options = options;
    evict_options.max_targets = 2;
    ObjectTracker evictor = make_tracker(evict_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(evictor, view, 1U, small));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(evictor, view, 2U, RectF{40.0F, 4.0F, 8.0F, 6.0F}));
    FakeTrackerBackend backend_b;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(evictor, backend_b, view, 1U));
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(evictor, backend_b, view, 2U));
    ASSERT_EQ(backend_b.counters->alive, 2);
    const int64_t both_attached = evictor.byte_size();
    const auto adopted = evictor.adopt_track(make_region(3U, RectF{20.0F, 40.0F, 8.0F, 6.0F}), view, 2);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    ASSERT_EQ(adopted.value().evicted_track_ids.size(), 1U);
    EXPECT_EQ(adopted.value().evicted_track_ids[0], 1U);
    EXPECT_EQ(backend_b.counters->alive, 1);
    EXPECT_EQ(evictor.tracker_session(1U), nullptr);
    EXPECT_NE(evictor.tracker_session(2U), nullptr);
    // Evicted: t1's track bytes + its handle slot; inserted: t3 (identical
    // shape to t1, so an identical byte account).
    EXPECT_EQ(evictor.byte_size(),
              both_attached - track_bytes - ObjectTracker::kTrackerHandleSlotOverheadBytes + track_bytes);

    // Reset path: every remaining session is destroyed synchronously.
    evictor.reset();
    EXPECT_EQ(backend_b.counters->alive, 0);
    EXPECT_EQ(evictor.byte_size(), 0);
}

TEST(ObjectTrackerDeepChannelTest, AttachValidationMatrix) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker, view, 7U, bounds));
    FakeTrackerBackend backend;

    // Null session.
    EXPECT_EQ(tracker.attach_tracker_session(7U, nullptr).status().code(), ErrorCode::kInvalidArgument);
    // Unknown track id.
    auto unknown = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(unknown.ok());
    EXPECT_EQ(tracker.attach_tracker_session(99U, unknown.take_value()).status().code(), ErrorCode::kInvalidArgument);
    // A terminated track cannot hold a session (identity closed).
    auto for_terminated = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(for_terminated.ok());
    ObjectTracker archive = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(archive, view, 7U, bounds));
    const auto ended = archive.terminate(7U, 2);
    ASSERT_TRUE(ended.ok()) << ended.status().message();
    EXPECT_EQ(archive.attach_tracker_session(7U, for_terminated.take_value()).status().code(),
              ErrorCode::kInvalidArgument);
    // Absent handles read as nullptr, const and non-const alike.
    EXPECT_EQ(tracker.tracker_session(99U), nullptr);
    const ObjectTracker& const_tracker = tracker;
    EXPECT_EQ(const_tracker.tracker_session(99U), nullptr);
    EXPECT_EQ(tracker.detach_tracker_session(99U), nullptr);
}

TEST(ObjectTrackerDeepChannelTest, SentinelNotInjectedZeroBytesInjectedSlotBytes) {
    // The evolved two-state form of the M7-11 sentinel
    // OrchestrationAddsZeroPoolBytesUntilTheM713Slot: (a) not injected — the
    // pool accounts zero handle bytes and behaves bit-identically whether the
    // switch is off or on; (b) injected — the pool accounts exactly
    // kTrackerHandleSlotOverheadBytes while the handle is stored and drops
    // back on detach, at every checkpoint of an identical operation sequence.
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions enabled_options;
    enabled_options.deep_channel_enabled = true;

    // (a) Not injected: switch off vs switch on, traditional commits only.
    ObjectTracker switch_off = make_tracker();
    ObjectTracker switch_on = make_tracker(enabled_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(switch_off, view, 7U, bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(switch_on, view, 7U, bounds));
    ASSERT_EQ(switch_off.byte_size(), switch_on.byte_size());
    const auto off_commit =
        switch_off.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                         position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    const auto on_commit =
        switch_on.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
                                        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2);
    ASSERT_TRUE(off_commit.ok());
    ASSERT_TRUE(on_commit.ok());
    EXPECT_EQ(off_commit.value().grade, on_commit.value().grade);
    EXPECT_TRUE(identical(snapshot_of(switch_off), snapshot_of(switch_on)));

    // (b) Injected vs not-injected under the same corroborated evidence: the
    // slot bytes are the ONLY difference while the handle is stored, and the
    // twins match bitwise again after the detach.
    ObjectTracker unattached = make_tracker(enabled_options);
    ObjectTracker injected = make_tracker(enabled_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(unattached, view, 7U, bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(injected, view, 7U, bounds));
    FakeTrackerBackend backend;
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(injected, backend, view, 7U));
    ASSERT_EQ(injected.byte_size(), unattached.byte_size() + ObjectTracker::kTrackerHandleSlotOverheadBytes);

    const RectF candidate = offset_bounds(bounds, 2, 1);
    const auto unattached_commit = unattached.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(candidate, 0.9F));
    const auto injected_commit = injected.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 2, 1, 0.85),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(candidate, 0.9F));
    ASSERT_TRUE(unattached_commit.ok()) << unattached_commit.status().message();
    ASSERT_TRUE(injected_commit.ok()) << injected_commit.status().message();
    EXPECT_EQ(injected_commit.value().deep_disposition, unattached_commit.value().deep_disposition);
    EXPECT_EQ(injected_commit.value().grade, unattached_commit.value().grade);
    EXPECT_EQ(injected.byte_size(), unattached.byte_size() + ObjectTracker::kTrackerHandleSlotOverheadBytes);
    std::unique_ptr<TrackerSession> removed = injected.detach_tracker_session(7U);
    ASSERT_NE(removed, nullptr);
    removed.reset();
    EXPECT_TRUE(identical(snapshot_of(injected), snapshot_of(unattached)));
}

// --- DOD-04: combination rules do not relax parameter invalidation ---------------

TEST(ObjectTrackerDeepChannelTest, CombinationRulesDoNotRelaxInvalidation) {
    // Two trackers differing only in `ncc_strong_threshold` turn the same
    // frame pair (E1 in the weak band) into different grades; corroborated
    // deep evidence upgrades each side's confidence by its OWN rule — the
    // deep channel never normalizes the grades across parameter differences.
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};
    const RectF candidate = offset_bounds(bounds, 1, 0);

    ObjectTrackerOptions weak_options;
    weak_options.deep_channel_enabled = true;
    ObjectTracker weak_tracker = make_tracker(weak_options);  // strong at 0.8, peak 0.7 stays weak
    ObjectTrackerOptions strong_options = weak_options;
    strong_options.ncc_strong_threshold = 0.65;
    ObjectTracker strong_tracker = make_tracker(strong_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(weak_tracker, view, 7U, bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(strong_tracker, view, 7U, bounds));

    const auto weak_commit = weak_tracker.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kWeak, 1, 0, 0.7),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(candidate, 0.9F));
    const auto strong_commit = strong_tracker.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 1, 0, 0.7),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(candidate, 0.9F));
    ASSERT_TRUE(weak_commit.ok()) << weak_commit.status().message();
    ASSERT_TRUE(strong_commit.ok()) << strong_commit.status().message();
    EXPECT_EQ(weak_commit.value().grade, EvidenceGrade::kTentative);
    EXPECT_EQ(strong_commit.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_FLOAT_EQ(weak_tracker.find_track(7U)->confidence, 0.9F);
    EXPECT_FLOAT_EQ(strong_tracker.find_track(7U)->confidence, 0.9F);

    // Two trackers differing only in `deep_agreement_min_iou` flip the same
    // deep evidence between corroboration and conflict: the deep channel's
    // own parameter change still flips the disposition on identical inputs.
    const RectF drifted = offset_bounds(bounds, 4, 2);  // IoU ~= 0.49 against the unchanged bounds
    ObjectTrackerOptions loose_options;
    loose_options.deep_channel_enabled = true;
    loose_options.deep_agreement_min_iou = 0.4;
    ObjectTrackerOptions strict_options;
    strict_options.deep_channel_enabled = true;  // deep_agreement_min_iou stays at the 0.5 default
    ObjectTracker loose = make_tracker(loose_options);
    ObjectTracker strict = make_tracker(strict_options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(loose, view, 7U, bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(strict, view, 7U, bounds));

    const auto corroborated = loose.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 0, 0, 0.85),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(drifted, 0.9F));
    const auto conflicted = strict.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kStrong, 0, 0, 0.85),
        position_of(PositionScenario::kStationary, true), std::nullopt, view, 2, deep_of(drifted, 0.9F));
    ASSERT_TRUE(corroborated.ok()) << corroborated.status().message();
    ASSERT_TRUE(conflicted.ok()) << conflicted.status().message();
    EXPECT_EQ(corroborated.value().deep_disposition, DeepChannelDisposition::kCorroborated);
    EXPECT_EQ(corroborated.value().grade, EvidenceGrade::kConfirmed);
    EXPECT_EQ(conflicted.value().deep_disposition, DeepChannelDisposition::kConflict);
    EXPECT_EQ(conflicted.value().grade, EvidenceGrade::kPlaceholder);
}

// --- determinism (RULE-03, contract block 8 at the pool seam) --------------------

TEST(ObjectTrackerDeepChannelTest, DeepCommitsAreBitDeterministic) {
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;

    FakeTrackerBackend backend_a;
    FakeTrackerBackend backend_b;
    ObjectTracker tracker_a = make_tracker(options);
    ObjectTracker tracker_b = make_tracker(options);
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker_a, view, 7U, bounds));
    ASSERT_NO_FATAL_FAILURE(adopt_or_fail(tracker_b, view, 7U, bounds));
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker_a, backend_a, view, 7U));
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker_b, backend_b, view, 7U));

    // An identical commit sequence — corroborated confirm, withheld confirm,
    // conflict, corroborated tentative — must produce bit-identical echoes
    // and bit-identical pool state on both twins (including backend-driven
    // evidence: identical initialize inputs plus identical update sequences
    // yield identical deep evidence per contract block 8).
    struct Step {
        AppearanceChannelOutcome appearance;
        int32_t dx;
        int32_t dy;
        double peak;
        bool supply_deep;
        RectF deep_bounds;
        float deep_confidence;
    };
    const std::vector<Step> steps = {
        {AppearanceChannelOutcome::kStrong, 2, 1, 0.85, true, offset_bounds(bounds, 2, 1), 0.9F},
        {AppearanceChannelOutcome::kStrong, 0, 0, 0.85, false, RectF{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F},
        {AppearanceChannelOutcome::kStrong, 0, 0, 0.85, true, RectF{40.0F, 30.0F, 8.0F, 6.0F}, 0.95F},
        {AppearanceChannelOutcome::kWeak, 1, 0, 0.65, true, offset_bounds(bounds, 1, 0), 0.8F},
    };
    uint64_t sequence = 2;
    for (const Step& step : steps) {
        const TrackVerification verification = verification_of(7U, step.appearance, step.dx, step.dy, step.peak);
        const TrackPositionEvidence position = position_of(PositionScenario::kStationary, true);
        Result<TrackEvidenceCommit> commit_a{Status{ErrorCode::kInvalidArgument, "unset"}};
        Result<TrackEvidenceCommit> commit_b{Status{ErrorCode::kInvalidArgument, "unset"}};
        if (step.supply_deep) {
            commit_a = tracker_a.commit_track_evidence(7U, verification, position, std::nullopt, view, sequence,
                                                       deep_of(step.deep_bounds, step.deep_confidence));
            commit_b = tracker_b.commit_track_evidence(7U, verification, position, std::nullopt, view, sequence,
                                                       deep_of(step.deep_bounds, step.deep_confidence));
        } else {
            commit_a = tracker_a.commit_track_evidence(7U, verification, position, std::nullopt, view, sequence);
            commit_b = tracker_b.commit_track_evidence(7U, verification, position, std::nullopt, view, sequence);
        }
        ASSERT_TRUE(commit_a.ok()) << commit_a.status().message();
        ASSERT_TRUE(commit_b.ok()) << commit_b.status().message();
        EXPECT_EQ(commit_a.value().grade, commit_b.value().grade) << "sequence " << sequence;
        EXPECT_EQ(commit_a.value().state, commit_b.value().state) << "sequence " << sequence;
        EXPECT_EQ(commit_a.value().deep_disposition, commit_b.value().deep_disposition) << "sequence " << sequence;
        EXPECT_EQ(commit_a.value().template_captured, commit_b.value().template_captured) << "sequence " << sequence;
        EXPECT_EQ(commit_a.value().template_withheld_by_deep_channel,
                  commit_b.value().template_withheld_by_deep_channel)
            << "sequence " << sequence;
        EXPECT_TRUE(identical(snapshot_of(tracker_a), snapshot_of(tracker_b))) << "sequence " << sequence;
        ++sequence;
    }
    EXPECT_EQ(tracker_a.byte_size(), tracker_b.byte_size());
}

// --- DOD-03 coordinate matrix ----------------------------------------------------

namespace {

struct MatrixCellResult {
    RectF deep_bounds;
    float deep_confidence = 0.0F;
    EvidenceGrade grade = EvidenceGrade::kPlaceholder;
    DeepChannelDisposition disposition = DeepChannelDisposition::kNotSupplied;
    RectF track_bounds;
    float track_confidence = 0.0F;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one assert block per matrix-cell invariant
MatrixCellResult run_matrix_cell(uint8_t fill, const RectF& prior, Rotation rotation) {
    constexpr int32_t kWidth = 33;
    constexpr int32_t kHeight = 21;
    constexpr int64_t kPadding = 3;
    const GrayImage image = make_gray_image(kWidth, kHeight, std::byte{fill}, kPadding);
    const uint64_t before_hash = buffer_hash(image);
    const ImageView view = view_of(image, rotation);

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    adopt_or_fail(tracker, view, 7U, prior);
    FakeTrackerBackend backend;
    backend.pixel_echo = true;
    attach_or_fail(tracker, backend, view, 7U);
    TrackerSession& session = *tracker.tracker_session(7U);

    const auto updated = session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    EXPECT_TRUE(updated.ok()) << updated.status().message();
    // The backend observes the presented metadata verbatim (contract block
    // 6: rotation is caller-side metadata it never interprets) and never
    // modifies the input bytes.
    EXPECT_EQ(as_fake(session).last_rotation, rotation);
    EXPECT_EQ(as_fake(session).last_width, kWidth);
    EXPECT_EQ(as_fake(session).last_height, kHeight);
    EXPECT_EQ(as_fake(session).last_stride, kWidth + kPadding);
    EXPECT_EQ(as_fake(session).last_prior, prior);
    EXPECT_EQ(before_hash, buffer_hash(image)) << "input bytes were modified";
    if (!updated.ok()) {
        return MatrixCellResult{};
    }
    // The echo reports fully inside bounds (contract block 6).
    EXPECT_GE(updated.value().bounds.x, 0.0F);
    EXPECT_GE(updated.value().bounds.y, 0.0F);
    EXPECT_LE(updated.value().bounds.x + updated.value().bounds.width, static_cast<float>(kWidth));
    EXPECT_LE(updated.value().bounds.y + updated.value().bounds.height, static_cast<float>(kHeight));

    // The caller feeds the recovered evidence straight back (the echo is
    // already in this tracker's presented space here); the pool validates it
    // at the adoption point — flush-edge bounds are inclusive.
    const auto commit = tracker.commit_track_evidence(
        7U, verification_of(7U, AppearanceChannelOutcome::kNone), position_of(PositionScenario::kStationary, true),
        std::nullopt, view, 2, deep_of(updated.value().bounds, updated.value().confidence));
    EXPECT_TRUE(commit.ok()) << commit.status().message();
    MatrixCellResult result;
    if (commit.ok()) {
        result.deep_bounds = updated.value().bounds;
        result.deep_confidence = updated.value().confidence;
        result.grade = commit.value().grade;
        result.disposition = commit.value().deep_disposition;
        const TargetTrack* track = tracker.find_track(7U);
        result.track_bounds = track->last_bounds;
        result.track_confidence = track->confidence;
    }
    return result;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one comparison block per matrix cell
TEST(ObjectTrackerDeepChannelTest, CoordinateMatrixRotationOddSizeStrideFlushEdge) {
    // Odd presented size (33x21), non-contiguous stride (+3 padding), four
    // rotation metadata values, interior and flush-corner priors (x+w == 33,
    // y+h == 21), two fill levels (one exercises the echo's flush-edge
    // clamp). Identical presented content must yield bit-identical deep
    // evidence, commits and pool state across the whole matrix (the pool
    // consumes tracker-space coordinates only; the caller owns the
    // prepared-space recovery, RULE-05/DOD-03).
    const RectF interior{4.0F, 3.0F, 24.0F, 18.0F};
    const RectF flush_corner{9.0F, 3.0F, 24.0F, 18.0F};
    const std::vector<Rotation> rotations = {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270};

    for (const uint8_t fill : {uint8_t{40}, uint8_t{41}}) {
        for (const RectF* prior : {&interior, &flush_corner}) {
            std::vector<MatrixCellResult> per_rotation;
            per_rotation.reserve(rotations.size());
            for (const Rotation rotation : rotations) {
                per_rotation.push_back(run_matrix_cell(fill, *prior, rotation));
            }
            for (size_t index = 1; index < per_rotation.size(); ++index) {
                EXPECT_EQ(per_rotation[index].deep_bounds, per_rotation[0].deep_bounds)
                    << "fill " << static_cast<int>(fill) << " rotation index " << index;
                EXPECT_FLOAT_EQ(per_rotation[index].deep_confidence, per_rotation[0].deep_confidence);
                EXPECT_EQ(per_rotation[index].grade, per_rotation[0].grade);
                EXPECT_EQ(per_rotation[index].disposition, per_rotation[0].disposition);
                EXPECT_EQ(per_rotation[index].track_bounds, per_rotation[0].track_bounds);
                EXPECT_FLOAT_EQ(per_rotation[index].track_confidence, per_rotation[0].track_confidence);
            }
            // Sanity on the fixture itself: the echo keeps the prior size.
            EXPECT_EQ(per_rotation[0].deep_bounds.width, prior->width);
            EXPECT_EQ(per_rotation[0].deep_bounds.height, prior->height);
        }
    }
}

// --- privacy (RULE-10/DOD-06) ----------------------------------------------------

/// Snapshot of a directory's entry names (the M7-11 suite's method:
/// non-recursive, error-code overloads only — the test never throws).
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one record block per harvested message shape
TEST(ObjectTrackerDeepChannelTest, DeepChannelWritesNoFilesAndStatusCarriesNoPixelMarker) {
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path();
    const auto before = snapshot_directory(temp);

    // A marker flows through the tracked pixels; it must never surface in
    // any Status message the deep-channel flow produces, and the whole run
    // must write nothing to the filesystem.
    const std::string marker = "M7-13-DEEP-PRIVACY-MARKER";
    GrayImage image = make_gray_image(64, 48, std::byte{0});
    for (size_t index = 0; index < marker.size(); ++index) {
        image.pixels[index + 1] = static_cast<std::byte>(marker[index]);
    }
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    std::vector<std::string> messages;
    const auto record = [&messages](const auto& result) {
        if (!result.ok()) {
            messages.push_back(result.status().message());
        }
    };

    ObjectTrackerOptions options;
    options.deep_channel_enabled = true;
    ObjectTracker tracker = make_tracker(options);
    FakeTrackerBackend backend;
    record(tracker.adopt_track(make_region(7U, bounds), view, 1));
    ASSERT_NO_FATAL_FAILURE(attach_or_fail(tracker, backend, view, 7U));
    TrackerSession& session = *tracker.tracker_session(7U);

    // Harvest every error message the deep-channel flow can produce.
    record(session.update(view, TrackerUpdateRequest{bounds, {}}, cancelled_context()));
    record(session.update(view, TrackerUpdateRequest{bounds, {}}, expired_deadline_context()));
    as_fake(session).injected_status = Status(ErrorCode::kBackendFailure, "fake-tracker corrupted");
    record(session.update(view, TrackerUpdateRequest{bounds, {}}, clean_context()));
    record(tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                         position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                         deep_of(RectF{18.0F, 13.0F, 0.0F, 12.0F}, 0.9F)));
    record(tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                         position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                         deep_of(bounds, 2.0F)));
    record(tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                         position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                         deep_of(RectF{70.0F, 13.0F, 16.0F, 12.0F}, 0.9F)));
    ObjectTracker off_tracker = make_tracker();  // the switch-off rejection message
    record(off_tracker.adopt_track(make_region(7U, bounds), view, 1));
    record(off_tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                             position_of(PositionScenario::kStationary, true), std::nullopt, view, 2,
                                             deep_of(bounds, 0.9F)));
    record(tracker.commit_track_evidence(7U, verification_of(7U, AppearanceChannelOutcome::kNone),
                                         position_of(PositionScenario::kStationary, true), std::nullopt, view, 2));
    std::unique_ptr<TrackerSession> discarded = tracker.detach_tracker_session(7U);
    discarded.reset();
    record(tracker.terminate(7U, 3));

    for (const std::string& message : messages) {
        EXPECT_FALSE(contains(message, marker)) << "status message leaked the pixel marker: " << message;
    }

    const auto after = snapshot_directory(temp);
    EXPECT_EQ(after.size(), before.size()) << "the deep-channel flow wrote files";
    for (const auto& [name, type] : after) {
        EXPECT_EQ(before.count(name), 1U) << "new file appeared: " << name << " (" << static_cast<int>(type) << ")";
    }
}

}  // namespace
