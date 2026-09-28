// M7-11 TrackerBackend SPI contract freeze (DEC-020): independent verification
// suite for include/mirador/tracker_backend.hpp and the fusion-side
// orchestration pattern it freezes (object-tracking design section 2 row D and
// section 6.2). The header ships only the pure interface — mirador-core holds
// no backend code, so the frozen contract is made executable through a Fake
// TrackerBackend (the fixed-trajectory injection double of DEC-020's
// verification method) that rehearses the M7-13 fusion-side wiring with the
// already-frozen ObjectTracker primitives only (zero new ObjectTracker API):
//
//   * Lifecycle (contract block 1): initialize -> update x N -> destruction;
//     initialize with an injected backend failure yields kBackendFailure and
//     no handle.
//   * Validation matrix (contract blocks 4/6 and the initialize/update error
//     lists): invalid views, non-finite/empty/out-of-image bounds ->
//     kInvalidArgument, undeclared formats -> kUnsupportedFormat; the session
//     state is untouched after every error.
//   * The FROZEN ordering: structural validation takes precedence over
//     cancellation (the M7-05/M7-06 pool precedent, the deliberate contrast to
//     the M7-02 adopt_track cancel-first entry) — a malformed request reports
//     kInvalidArgument even with an already-cancelled and expired context.
//   * Cancellation/deadline conversion (RULE-03): kCancelled/kTimeout are
//     explicit, leave the session fully usable and advance nothing
//     (all-or-nothing, contract block 3 — the fake commits its per-frame state
//     only on the success path).
//   * Failure is visible, never stale (contract block 3): kBackendFailure
//     returns no value, freezes the trajectory cursor, and recovery is destroy
//     + fresh initialize. The fusion-side degradation runs through the frozen
//     commit_track_evidence placeholder grade (kTracking -> kUncertain) and
//     the rebuilt session serves the next frame (the RISK-2026-18 hook).
//   * Handle ownership (contract block 2): the harness maps track terminate,
//     pool eviction (the explicit evicted_track_ids report) and reset to
//     synchronous session destruction, and a session rebuild replaces the
//     handle in place; the SPI adds zero pool bytes until the M7-13 slot
//     constant lands (byte_size checkpoints equal a backend-free run).
//   * Cache exemption (contract block 5, the negative): identical update calls
//     on one session return different results — the RULE-07 "same input same
//     result" invariant genuinely fails for update — and a RULE-07 key built
//     from the call's inputs is identical for both calls, so a capability-
//     result cache intercepting update would serve the stale first result
//     (demonstrated with the real CapabilityResultCache).
//   * Coordinates (DOD-03): 0/90/180/270 rotation metadata x odd presented
//     size x non-contiguous stride x flush-edge priors — identical presented
//     content yields bit-identical results, the backend observes presented
//     dimensions only, and the input bytes are never modified.
//   * Per-sequence determinism (contract block 8): identical initialize inputs
//     plus identical frame sequences produce bit-identical result sequences
//     across sessions and backend instances (single-call "same input same
//     result" is explicitly not a requirement for update).
//   * Privacy (RULE-10/DOD-06): the whole orchestration writes no files and
//     every produced Status message stays free of a pixel-borne marker.
//
// The fake is the executable specification of the frozen contract for the
// M7-12 reference backend and the M7-13 injection wiring; where this suite
// pins behavior, the header's contract blocks quoted in the comments govern.

#include <mirador/tracker_backend.hpp>

#include <mirador/backend_info.hpp>
#include <mirador/capability_cache.hpp>
#include <mirador/detector_backend.hpp>
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
using mirador::CachedCapabilityResult;
using mirador::CacheKeyDigest;
using mirador::CapabilityKeyFields;
using mirador::CapabilityKind;
using mirador::CapabilityResultCache;
using mirador::deadline_reached;
using mirador::DetectionRegion;
using mirador::ErrorCode;
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
using mirador::TrackState;
using mirador::TrackVerification;
using mirador::validate;
using mirador::VisualRegion;

// --- image helpers (the sibling fusion suites' gray-buffer shape) --------------

/// Owning single-channel gray8 buffer with an explicit row stride.
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

/// Noise-like per-pixel pattern (uint32 hashing, deterministic, no UB).
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

/// Structurally valid RGBA8 view (stride 4x width) over an owned buffer —
/// used for the undeclared-format cases so the only violation is the format
/// gate itself and the frozen kInvalidArgument/kUnsupportedFormat boundary is
/// not blurred.
struct RgbaImage {
    std::vector<std::byte> pixels;
    int32_t width = 0;
    int32_t height = 0;
};

ImageView rgba_view_of(const RgbaImage& image) {
    ImageView view;
    view.data = image.pixels.data();
    view.width = image.width;
    view.height = image.height;
    view.row_stride_bytes = static_cast<int64_t>(image.width) * 4;
    view.format = PixelFormat::kRgba8;
    return view;
}

RgbaImage make_rgba_image(int32_t width, int32_t height) {
    RgbaImage image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<size_t>(width) * 4U * static_cast<size_t>(height), std::byte{0});
    return image;
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

// --- the fake fixed-trajectory tracker backend (DEC-020 verification double) ---

/// Session/destroy counters shared between the backend and every session, so
/// destruction observations survive any destruction order without the handle
/// ever holding a backend reference (contract block 1: the handle never
/// extends the backend's lifetime).
struct SessionCounters {
    int alive = 0;
    int created = 0;
    int destroyed = 0;
};

/// The frozen request validation as an executable specification (contract
/// blocks 4 and 6 plus the initialize/update error lists): the
/// validate(ImageView) rule set, then the accepted_formats gate, then
/// finite/positive/fully-inside bounds. Only validation-before-cancellation
/// is frozen; the relative order of kInvalidArgument and kUnsupportedFormat
/// for a doubly-bad request is deliberately not pinned by the header, and the
/// suite never exercises an overlap.
Result<void> validate_tracker_request(const ImageView& image, const RectF& bounds,
                                      const std::vector<PixelFormat>& accepted_formats) {
    if (Result<void> checked = validate(image); !checked.ok()) {
        return checked;
    }
    const bool declared = std::any_of(accepted_formats.begin(), accepted_formats.end(),
                                      [&](PixelFormat format) { return format == image.format; });
    if (!declared) {
        return Status(ErrorCode::kUnsupportedFormat, "prepared format not declared in accepted_formats");
    }
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || bounds.width <= 0.0F || bounds.height <= 0.0F || bounds.x < 0.0F ||
        bounds.y < 0.0F || bounds.x + bounds.width > static_cast<float>(image.width) ||
        bounds.y + bounds.height > static_cast<float>(image.height)) {
        return Status(ErrorCode::kInvalidArgument,
                      "request bounds must be finite, positive and fully inside the prepared image");
    }
    return {};
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): test double, knobs
// and observations are public by design (the fake_backends.hpp convention).
struct FakeTrackerSession final : TrackerSession {
    FakeTrackerSession(std::shared_ptr<SessionCounters> shared_counters, bool echo_pixels,
                       std::vector<TrackerUpdateResult> fixed_trajectory, std::vector<PixelFormat> formats)
        : pixel_echo(echo_pixels),
          trajectory(std::move(fixed_trajectory)),
          accepted_formats(std::move(formats)),
          counters(std::move(shared_counters)) {
        ++counters->created;
        ++counters->alive;
    }

    ~FakeTrackerSession() override {
        --counters->alive;
        ++counters->destroyed;
    }

    // Knobs: the injected failure maps onto the frozen kBackendFailure path
    // (contract block 3); a fixed trajectory replays one pre-decided result
    // per successful update and reports kBackendFailure once exhausted.
    Status next_update_status;
    bool pixel_echo = false;
    std::vector<TrackerUpdateResult> trajectory;
    std::vector<PixelFormat> accepted_formats;
    std::shared_ptr<SessionCounters> counters;

    // Observations.
    int update_count = 0;
    int success_count = 0;
    /// All-or-nothing probe (contract block 3): advanced only on success.
    int64_t cursor = 0;
    RectF last_prior;
    PixelFormat last_format = PixelFormat::kGray8;
    Rotation last_rotation = Rotation::k0;
    int32_t last_width = 0;
    int32_t last_height = 0;
    int64_t last_stride = 0;

    Result<TrackerUpdateResult> update(const ImageView& prepared_image, const TrackerUpdateRequest& request,
                                       const ExecutionContext& context) override {
        ++update_count;
        last_prior = request.prior_bounds;
        last_format = prepared_image.format;
        last_rotation = prepared_image.rotation;
        last_width = prepared_image.width;
        last_height = prepared_image.height;
        last_stride = prepared_image.row_stride_bytes;
        if (const Result<void> checked =
                validate_tracker_request(prepared_image, request.prior_bounds, accepted_formats);
            !checked.ok()) {
            return checked.status();
        }
        if (is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "fake-tracker observed cancellation");
        }
        if (deadline_reached(context)) {
            return Status(ErrorCode::kTimeout, "fake-tracker observed deadline");
        }
        if (!next_update_status.ok()) {
            return next_update_status;
        }
        if (!pixel_echo && static_cast<size_t>(cursor) >= trajectory.size()) {
            return Status(ErrorCode::kBackendFailure, "fake-tracker fixed trajectory exhausted");
        }
        const TrackerUpdateResult result = compute_result(prepared_image, request.prior_bounds);
        ++cursor;  // commit-on-success: only the fully successful path advances state
        ++success_count;
        return result;
    }

private:
    /// Fixed mode replays `trajectory[cursor]`; pixel-echo mode derives the
    /// result deterministically from the presented bytes (stride-honoring,
    /// covering integer window of the prior) so the coordinate matrix actually
    /// exercises the view reads. Both are integer-arithmetic plus one float
    /// division — bit-identical on every replay (contract block 8).
    [[nodiscard]] TrackerUpdateResult compute_result(const ImageView& image, const RectF& prior) const {
        TrackerUpdateResult result;
        if (pixel_echo) {
            const auto x0 = std::max(0, static_cast<int32_t>(std::floor(prior.x)));
            const auto y0 = std::max(0, static_cast<int32_t>(std::floor(prior.y)));
            const auto x1 = std::min(image.width, static_cast<int32_t>(std::ceil(prior.x + prior.width)));
            const auto y1 = std::min(image.height, static_cast<int32_t>(std::ceil(prior.y + prior.height)));
            uint64_t sum = 0;
            for (int32_t y = y0; y < y1; ++y) {
                const std::byte* row = image.data + static_cast<int64_t>(y) * image.row_stride_bytes;
                for (int32_t x = x0; x < x1; ++x) {
                    sum += static_cast<uint8_t>(row[x]);
                }
            }
            // The translation clamps to the image so even a flush-edge prior
            // yields fully inside results (contract block 6: implementations
            // report bounds inside the prepared image).
            const float dx = std::min(static_cast<float>(sum % 3U) + 1.0F,
                                      static_cast<float>(image.width) - (prior.x + prior.width));
            const float dy = std::min(static_cast<float>(sum % 2U) + 1.0F,
                                      static_cast<float>(image.height) - (prior.y + prior.height));
            result.bounds = RectF{prior.x + dx, prior.y + dy, prior.width, prior.height};
            result.confidence = 0.5F + static_cast<float>(sum % 100U) / 250.0F;
        } else {
            result = trajectory[static_cast<size_t>(cursor)];
        }
        return result;
    }
};

struct FakeTrackerBackend final : TrackerBackend {
    BackendInfo info_value;
    /// Injected initialize failure (kBackendFailure: model not loaded etc.).
    Status next_init_status;
    bool pixel_echo = false;
    /// Template copied into every session the backend creates.
    std::vector<TrackerUpdateResult> trajectory;
    std::shared_ptr<SessionCounters> counters = std::make_shared<SessionCounters>();

    int init_count = 0;

    FakeTrackerBackend() {
        info_value.name = "fake-tracker";
        info_value.implementation_version = "1.0.0";
        info_value.model_id = "fake-track-model";
        info_value.model_revision = "r1";
        info_value.accepted_formats = {PixelFormat::kGray8};
        info_value.thread_safe = false;
        trajectory = {{RectF{2.0F, 1.0F, 8.0F, 6.0F}, 0.90F},
                      {RectF{3.0F, 2.0F, 8.0F, 6.0F}, 0.85F},
                      {RectF{4.0F, 3.0F, 8.0F, 6.0F}, 0.80F}};
    }

    [[nodiscard]] BackendInfo info() const override { return info_value; }

    Result<std::unique_ptr<TrackerSession>> initialize(const ImageView& prepared_image,
                                                       const TrackerInitRequest& request,
                                                       const ExecutionContext& context) override {
        ++init_count;
        last_init_bounds = request.initial_bounds;
        if (const Result<void> checked =
                validate_tracker_request(prepared_image, request.initial_bounds, info_value.accepted_formats);
            !checked.ok()) {
            return checked.status();
        }
        if (is_cancelled(context)) {
            return Status(ErrorCode::kCancelled, "fake-tracker observed cancellation");
        }
        if (deadline_reached(context)) {
            return Status(ErrorCode::kTimeout, "fake-tracker observed deadline");
        }
        if (!next_init_status.ok()) {
            return next_init_status;
        }
        auto session =
            std::make_unique<FakeTrackerSession>(counters, pixel_echo, trajectory, info_value.accepted_formats);
        return {std::move(session)};
    }

    // Observation of the last initialize request.
    RectF last_init_bounds;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

/// Downcast helper: the suite only ever hands out fakes it created through
/// `FakeTrackerBackend::initialize`.
FakeTrackerSession& as_fake(TrackerSession& session) {
    return *static_cast<FakeTrackerSession*>(&session);
}

// --- the fusion-side harness (the M7-11-frozen ownership decision, rehearsed) --

/// One parallel session slot per track — the pool-side single-slot storage
/// pattern of M7-05/M7-06/M7-08 that contract block 2 freezes for the handle.
/// The slot's byte constant itself lands with the M7-13 wiring; this harness
/// proves the frozen ownership decision is executable as written: a rebuild
/// replaces the handle in place (destroy old, store new) and the slots are
/// destroyed synchronously on track termination, pool eviction and reset.
class FusionSideSessionSlots {
public:
    void attach(uint64_t track_id, std::unique_ptr<TrackerSession> session) { slots_[track_id] = std::move(session); }

    void destroy(uint64_t track_id) { slots_.erase(track_id); }

    void destroy_all() { slots_.clear(); }

    [[nodiscard]] TrackerSession* find(uint64_t track_id) const {
        const auto slot = slots_.find(track_id);
        return slot == slots_.end() ? nullptr : slot->second.get();
    }

    [[nodiscard]] size_t size() const { return slots_.size(); }

private:
    std::map<uint64_t, std::unique_ptr<TrackerSession>> slots_;
};

// --- pool helpers (the frozen ObjectTracker primitives, as in the M7-06 suite) -

VisualRegion make_region(uint64_t stable_id, RectF bounds, float confidence = 0.9F) {
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    region.anchor = {bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
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

TrackVerification placeholder_verification(uint64_t track_id) {
    // Hand-built caller evidence (the commit is contractually forbidden from
    // re-running the scan): no appearance channel outcome and no structure
    // channel — the frozen decision table's row 5, the placeholder grade.
    TrackVerification verification;
    verification.track_id = track_id;
    verification.state = TrackState::kTracking;
    verification.verification_roi = RectI{0, 0, 1, 1};
    verification.appearance.outcome = AppearanceChannelOutcome::kNone;
    verification.appearance.peak_ncc = 0.0;
    verification.appearance.peak_sidelobe_ratio = 0.0;
    verification.structure.outcome = StructureChannelOutcome::kNotSupplied;
    return verification;
}

/// Snapshot of a directory's entry names (the privacy suite's method:
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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- lifecycle -----------------------------------------------------------------

TEST(TrackerBackendOrchestration, InfoReturnsDeclaredIdentityAndValidates) {
    const FakeTrackerBackend backend;
    const BackendInfo info = backend.info();
    EXPECT_EQ(info.name, "fake-tracker");
    EXPECT_EQ(info.implementation_version, "1.0.0");
    EXPECT_EQ(info.model_id, "fake-track-model");
    EXPECT_EQ(info.model_revision, "r1");
    ASSERT_EQ(info.accepted_formats.size(), 1U);
    EXPECT_EQ(info.accepted_formats[0], PixelFormat::kGray8);
    EXPECT_FALSE(info.thread_safe);
    const Result<void> checked = mirador::validate(info);
    ASSERT_TRUE(checked.ok()) << checked.status().message();
}

TEST(TrackerBackendOrchestration, InitializeCreatesUsableSessionAndDestructionDiscardsState) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);

    TrackerInitRequest request;
    request.initial_bounds = RectF{4.0F, 3.0F, 12.0F, 8.0F};
    auto initialized = backend.initialize(view, request, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    ASSERT_NE(initialized.value().get(), nullptr);
    EXPECT_EQ(backend.init_count, 1);
    EXPECT_EQ(backend.counters->created, 1);
    EXPECT_EQ(backend.counters->alive, 1);
    EXPECT_EQ(backend.last_init_bounds, request.initial_bounds);

    TrackerSession& session = *initialized.value();
    TrackerUpdateRequest update_request;
    update_request.prior_bounds = RectF{4.0F, 3.0F, 12.0F, 8.0F};
    const Result<TrackerUpdateResult> updated = session.update(view, update_request, clean_context());
    ASSERT_TRUE(updated.ok()) << updated.status().message();
    EXPECT_EQ(updated.value(), backend.trajectory[0]);

    // Stage 3: destroying the handle is the explicit discard of all state
    // (contract block 2).
    std::unique_ptr<TrackerSession> handle = initialized.take_value();
    handle.reset();
    EXPECT_EQ(backend.counters->alive, 0);
    EXPECT_EQ(backend.counters->destroyed, 1);
}

TEST(TrackerBackendOrchestration, InitializeBackendFailureYieldsNoHandle) {
    FakeTrackerBackend backend;
    backend.next_init_status = Status(ErrorCode::kBackendFailure, "fake-tracker model not loaded");
    const GrayImage image = make_noise_image(33, 21);

    TrackerInitRequest request;
    request.initial_bounds = RectF{4.0F, 3.0F, 12.0F, 8.0F};
    const Result<std::unique_ptr<TrackerSession>> initialized =
        backend.initialize(view_of(image), request, clean_context());

    // Failure is visible and no handle exists in that case (contract block 3
    // and the initialize error list).
    ASSERT_FALSE(initialized.ok());
    EXPECT_EQ(initialized.status().code(), ErrorCode::kBackendFailure);
    EXPECT_EQ(backend.counters->created, 0);
    EXPECT_EQ(backend.counters->alive, 0);

    // The backend itself stays usable after a failed initialize.
    backend.next_init_status = Status::success();
    auto retried = backend.initialize(view_of(image), request, clean_context());
    ASSERT_TRUE(retried.ok()) << retried.status().message();
    EXPECT_EQ(backend.counters->alive, 1);
}

// --- validation matrix ---------------------------------------------------------

// One EXPECT per validation rule (the sibling contract suites' precedent).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): gtest TEST body
TEST(TrackerBackendOrchestration, InitializeValidationMatrix) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);

    struct Case {
        std::string name;
        ImageView view;
        RectF bounds;
        ErrorCode expected;
    };

    ImageView null_view = view_of(image);
    null_view.data = nullptr;
    ImageView empty_view = view_of(image);
    empty_view.width = 0;

    const std::vector<Case> cases = {
        {"null plane data", null_view, RectF{4.0F, 3.0F, 12.0F, 8.0F}, ErrorCode::kInvalidArgument},
        {"zero width view", empty_view, RectF{4.0F, 3.0F, 12.0F, 8.0F}, ErrorCode::kInvalidArgument},
        {"nan x", view_of(image), RectF{std::nanf(""), 3.0F, 12.0F, 8.0F}, ErrorCode::kInvalidArgument},
        {"nan width", view_of(image), RectF{4.0F, 3.0F, std::nanf(""), 8.0F}, ErrorCode::kInvalidArgument},
        {"inf height", view_of(image), RectF{4.0F, 3.0F, 12.0F, std::numeric_limits<float>::infinity()},
         ErrorCode::kInvalidArgument},
        {"zero area", view_of(image), RectF{4.0F, 3.0F, 0.0F, 8.0F}, ErrorCode::kInvalidArgument},
        {"negative origin", view_of(image), RectF{-0.5F, 3.0F, 12.0F, 8.0F}, ErrorCode::kInvalidArgument},
        {"past right edge", view_of(image), RectF{21.5F, 3.0F, 12.0F, 8.0F}, ErrorCode::kInvalidArgument},
        {"past bottom edge", view_of(image), RectF{4.0F, 13.5F, 12.0F, 8.0F}, ErrorCode::kInvalidArgument},
    };
    for (const Case& entry : cases) {
        TrackerInitRequest request;
        request.initial_bounds = entry.bounds;
        const Result<std::unique_ptr<TrackerSession>> initialized =
            backend.initialize(entry.view, request, clean_context());
        EXPECT_FALSE(initialized.ok()) << entry.name;
        EXPECT_EQ(initialized.status().code(), entry.expected) << entry.name;
        EXPECT_EQ(backend.counters->created, 0) << entry.name;
    }

    // Undeclared format: the prepared format must come from
    // info().accepted_formats — never silently converted (contract block 6).
    // The view is structurally valid, so the format gate is the only
    // violated rule.
    const RgbaImage rgba_image = make_rgba_image(33, 21);
    TrackerInitRequest request;
    request.initial_bounds = RectF{4.0F, 3.0F, 12.0F, 8.0F};
    const Result<std::unique_ptr<TrackerSession>> format_rejected =
        backend.initialize(rgba_view_of(rgba_image), request, clean_context());
    EXPECT_FALSE(format_rejected.ok());
    EXPECT_EQ(format_rejected.status().code(), ErrorCode::kUnsupportedFormat);
    EXPECT_EQ(backend.counters->created, 0);

    // A flush-edge bound (the full view) is fully inside and valid (the
    // M7-01 adopt_track precedent for flush edges).
    request.initial_bounds = RectF{0.0F, 0.0F, 33.0F, 21.0F};
    auto flush = backend.initialize(view_of(image), request, clean_context());
    EXPECT_TRUE(flush.ok()) << flush.status().message();
}

TEST(TrackerBackendOrchestration, UpdateValidationMatrixLeavesSessionAtTrajectoryStart) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);

    TrackerInitRequest init_request;
    init_request.initial_bounds = RectF{4.0F, 3.0F, 12.0F, 8.0F};
    auto initialized = backend.initialize(view, init_request, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    TrackerSession& session = *initialized.value();

    const RectF valid_prior{4.0F, 3.0F, 12.0F, 8.0F};
    const std::vector<std::pair<std::string, TrackerUpdateRequest>> bad_requests = {
        {"nan prior", {RectF{std::nanf(""), 3.0F, 12.0F, 8.0F}, {}}},
        {"zero area prior", {RectF{4.0F, 3.0F, 12.0F, 0.0F}, {}}},
        {"outside prior", {RectF{30.0F, 18.0F, 12.0F, 8.0F}, {}}},
    };
    for (const auto& [name, bad_request] : bad_requests) {
        const Result<TrackerUpdateResult> updated = session.update(view, bad_request, clean_context());
        EXPECT_FALSE(updated.ok()) << name;
        EXPECT_EQ(updated.status().code(), ErrorCode::kInvalidArgument) << name;
    }

    const RgbaImage rgba_image = make_rgba_image(33, 21);
    const Result<TrackerUpdateResult> format_rejected =
        session.update(rgba_view_of(rgba_image), TrackerUpdateRequest{valid_prior, {}}, clean_context());
    EXPECT_FALSE(format_rejected.ok());
    EXPECT_EQ(format_rejected.status().code(), ErrorCode::kUnsupportedFormat);

    // Every error above consumed nothing: the next valid call replays the
    // first trajectory point (all-or-nothing, contract block 3).
    const FakeTrackerSession& fake = as_fake(session);
    EXPECT_EQ(fake.cursor, 0);
    const Result<TrackerUpdateResult> next =
        session.update(view, TrackerUpdateRequest{valid_prior, {}}, clean_context());
    ASSERT_TRUE(next.ok()) << next.status().message();
    EXPECT_EQ(next.value(), backend.trajectory[0]);
    EXPECT_EQ(fake.cursor, 1);
}

// --- the frozen ordering: validation precedes cancellation ---------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per frozen-ordering cell
TEST(TrackerBackendOrchestration, FrozenOrderingValidationPrecedesCancellation) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);
    // Already cancelled AND past the deadline: the malformed request must
    // still surface as kInvalidArgument (frozen ordering, contract block 4 —
    // the M7-05/M7-06 pool precedent, not the M7-02 cancel-first entry).
    const ExecutionContext hostile = [&] {
        ExecutionContext context;
        context.is_cancelled = [] { return true; };
        context.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        return context;
    }();

    TrackerInitRequest bad_init;
    bad_init.initial_bounds = RectF{40.0F, 3.0F, 12.0F, 8.0F};  // outside the 33x21 view
    const Result<std::unique_ptr<TrackerSession>> init_rejected = backend.initialize(view, bad_init, hostile);
    EXPECT_FALSE(init_rejected.ok());
    EXPECT_EQ(init_rejected.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(backend.counters->created, 0);

    TrackerInitRequest good_init;
    good_init.initial_bounds = RectF{4.0F, 3.0F, 12.0F, 8.0F};
    auto initialized = backend.initialize(view, good_init, hostile);
    EXPECT_FALSE(initialized.ok());
    EXPECT_EQ(initialized.status().code(), ErrorCode::kCancelled);

    initialized = backend.initialize(view, good_init, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    TrackerSession& session = *initialized.value();

    TrackerUpdateRequest bad_update;
    bad_update.prior_bounds = RectF{std::nanf(""), 3.0F, 12.0F, 8.0F};
    const Result<TrackerUpdateResult> update_rejected = session.update(view, bad_update, hostile);
    EXPECT_FALSE(update_rejected.ok());
    EXPECT_EQ(update_rejected.status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(as_fake(session).cursor, 0);

    const Result<TrackerUpdateResult> cancelled =
        session.update(view, TrackerUpdateRequest{good_init.initial_bounds, {}}, cancelled_context());
    EXPECT_EQ(cancelled.status().code(), ErrorCode::kCancelled);
    const Result<TrackerUpdateResult> timed_out =
        session.update(view, TrackerUpdateRequest{good_init.initial_bounds, {}}, expired_deadline_context());
    EXPECT_EQ(timed_out.status().code(), ErrorCode::kTimeout);
    EXPECT_EQ(as_fake(session).cursor, 0);
}

// --- cancellation/deadline conversion with all-or-nothing state ----------------

TEST(TrackerBackendOrchestration, CancelledAndTimeoutUpdatesLeaveSessionFullyUsable) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);

    auto initialized =
        backend.initialize(view, TrackerInitRequest{RectF{4.0F, 3.0F, 12.0F, 8.0F}, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    TrackerSession& session = *initialized.value();
    const TrackerUpdateRequest request{RectF{4.0F, 3.0F, 12.0F, 8.0F}, {}};

    const Result<TrackerUpdateResult> cancelled = session.update(view, request, cancelled_context());
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.status().code(), ErrorCode::kCancelled);

    const Result<TrackerUpdateResult> timed_out = session.update(view, request, expired_deadline_context());
    ASSERT_FALSE(timed_out.ok());
    EXPECT_EQ(timed_out.status().code(), ErrorCode::kTimeout);

    // Retry the same handle on a later frame: the session stays fully usable
    // and the per-frame state was never advanced (contract blocks 3/4).
    EXPECT_EQ(as_fake(session).cursor, 0);
    const Result<TrackerUpdateResult> retried = session.update(view, request, clean_context());
    ASSERT_TRUE(retried.ok()) << retried.status().message();
    EXPECT_EQ(retried.value(), backend.trajectory[0]);
    EXPECT_EQ(as_fake(session).cursor, 1);
}

// --- fixed-trajectory injection and failure visibility -------------------------

TEST(TrackerBackendOrchestration, FixedTrajectoryInjectionReplaysPointPerSuccessfulUpdate) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);
    const RectF prior{4.0F, 3.0F, 12.0F, 8.0F};

    auto initialized = backend.initialize(view, TrackerInitRequest{prior, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    TrackerSession& session = *initialized.value();

    for (size_t index = 0; index < backend.trajectory.size(); ++index) {
        const Result<TrackerUpdateResult> updated =
            session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
        ASSERT_TRUE(updated.ok()) << "trajectory point " << index;
        EXPECT_EQ(updated.value(), backend.trajectory[index]) << "trajectory point " << index;
        EXPECT_EQ(as_fake(session).cursor, static_cast<int64_t>(index) + 1);
    }

    // Exhausted fixed trajectory: an explicit backend failure, never a stale
    // replay of an earlier point (contract block 3).
    const Result<TrackerUpdateResult> exhausted =
        session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    EXPECT_FALSE(exhausted.ok());
    EXPECT_EQ(exhausted.status().code(), ErrorCode::kBackendFailure);
}

TEST(TrackerBackendOrchestration, BackendFailureIsVisibleNeverStaleAndSessionRebuildRecovers) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);
    const RectF prior{4.0F, 3.0F, 12.0F, 8.0F};

    auto initialized = backend.initialize(view, TrackerInitRequest{prior, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    TrackerSession& session = *initialized.value();

    const Result<TrackerUpdateResult> first = session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    ASSERT_TRUE(first.ok()) << first.status().message();

    as_fake(session).next_update_status = Status(ErrorCode::kBackendFailure, "fake-tracker state corrupted");
    const Result<TrackerUpdateResult> failed = session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    // Failure is visible: no value is returned, so no stale/cached answer can
    // leak to the caller (contract block 3). The failed call advanced nothing.
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.status().code(), ErrorCode::kBackendFailure);
    EXPECT_EQ(as_fake(session).cursor, 1);

    // The session must be treated as unusable: destroy it and recover through
    // a fresh initialize (contract block 3).
    std::unique_ptr<TrackerSession> unusable = initialized.take_value();
    unusable.reset();
    EXPECT_EQ(backend.counters->destroyed, 1);
    auto rebuilt = backend.initialize(view, TrackerInitRequest{prior, {}}, clean_context());
    ASSERT_TRUE(rebuilt.ok()) << rebuilt.status().message();
    TrackerSession& new_session = *rebuilt.value();
    EXPECT_EQ(as_fake(new_session).cursor, 0);
    const Result<TrackerUpdateResult> next = new_session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    ASSERT_TRUE(next.ok()) << next.status().message();
    EXPECT_EQ(next.value(), backend.trajectory[0]);
}

TEST(TrackerBackendOrchestration, SessionsOfOneBackendAreIndependent) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);
    const RectF prior{4.0F, 3.0F, 12.0F, 8.0F};

    auto first = backend.initialize(view, TrackerInitRequest{prior, {}}, clean_context());
    auto second = backend.initialize(view, TrackerInitRequest{prior, {}}, clean_context());
    ASSERT_TRUE(first.ok() && second.ok());
    EXPECT_EQ(backend.counters->alive, 2);

    // One session's failure never touches another session's state (contract
    // block 1).
    as_fake(*second.value()).next_update_status =
        Status(ErrorCode::kBackendFailure, "fake-tracker session two corrupted");
    const Result<TrackerUpdateResult> second_failed =
        second.value()->update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    EXPECT_EQ(second_failed.status().code(), ErrorCode::kBackendFailure);

    const Result<TrackerUpdateResult> first_ok =
        first.value()->update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    ASSERT_TRUE(first_ok.ok()) << first_ok.status().message();
    EXPECT_EQ(first_ok.value(), backend.trajectory[0]);

    // Teardown independence: destroying the failed session does not disturb
    // the survivor.
    std::unique_ptr<TrackerSession> failed_session = second.take_value();
    failed_session.reset();
    EXPECT_EQ(backend.counters->alive, 1);
    const Result<TrackerUpdateResult> first_again =
        first.value()->update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    ASSERT_TRUE(first_again.ok()) << first_again.status().message();
    EXPECT_EQ(first_again.value(), backend.trajectory[1]);
}

// --- fusion-side orchestration over the frozen ObjectTracker primitives --------

TEST(TrackerBackendOrchestration, BackendFailureDegradesTrackToUncertainThroughFrozenPrimitives) {
    // The DEC-020 failure-degradation hook: kBackendFailure from the session ->
    // the fusion side degrades the track through the existing frozen
    // commit_track_evidence placeholder grade -> rebuild the session. Zero new
    // ObjectTracker API: only adopt_track, commit_track_evidence, find_track.
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTracker tracker = make_tracker();
    const auto adopted = tracker.adopt_track(make_region(7U, bounds), view, 1);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();

    FusionSideSessionSlots slots;
    auto initialized = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    slots.attach(7U, initialized.take_value());
    ASSERT_EQ(slots.size(), 1U);

    // The deep channel frame: the backend reports failure (state corruption).
    TrackerSession& session = *slots.find(7U);
    as_fake(session).next_update_status = Status(ErrorCode::kBackendFailure, "fake-tracker corrupted");
    const Result<TrackerUpdateResult> failed = session.update(view, TrackerUpdateRequest{bounds, {}}, clean_context());
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.status().code(), ErrorCode::kBackendFailure);

    // Degradation through the frozen state machine: the placeholder grade
    // (decision table row 5 — no appearance, no structure) moves the kTracking
    // track to kUncertain (first insufficient commit; the limit is 5).
    const Result<TrackEvidenceCommit> commit = tracker.commit_track_evidence(
        7U, placeholder_verification(7U), TrackPositionEvidence{PositionScenario::kStationary, false}, std::nullopt,
        view, 2);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(commit.value().grade, mirador::EvidenceGrade::kPlaceholder);
    EXPECT_EQ(commit.value().previous_state, TrackState::kTracking);
    EXPECT_EQ(commit.value().state, TrackState::kUncertain);
    const TargetTrack* track = tracker.find_track(7U);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->state, TrackState::kUncertain);

    // Recovery: destroy the unusable session, rebuild on the track's current
    // bounds (the M7-13 shape), and the next frame is servable again. The
    // rebuild replaces the handle in place (contract block 2).
    const int64_t destroyed_before = backend.counters->destroyed;
    const RectF rebuild_bounds = track->last_bounds;
    auto rebuilt = backend.initialize(view, TrackerInitRequest{rebuild_bounds, {}}, clean_context());
    ASSERT_TRUE(rebuilt.ok()) << rebuilt.status().message();
    slots.attach(7U, rebuilt.take_value());
    EXPECT_EQ(backend.counters->destroyed, destroyed_before + 1);
    EXPECT_EQ(slots.size(), 1U);

    TrackerSession& new_session = *slots.find(7U);
    as_fake(new_session).next_update_status = Status::success();
    const Result<TrackerUpdateResult> next =
        new_session.update(view, TrackerUpdateRequest{rebuild_bounds, {}}, clean_context());
    ASSERT_TRUE(next.ok()) << next.status().message();
    EXPECT_EQ(next.value(), backend.trajectory[0]);
}

TEST(TrackerBackendOrchestration, HandleDestroyedSynchronouslyOnTerminate) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTracker tracker = make_tracker();
    ASSERT_TRUE(tracker.adopt_track(make_region(7U, bounds), view, 1).ok());

    FusionSideSessionSlots slots;
    auto initialized = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    slots.attach(7U, initialized.take_value());
    EXPECT_EQ(backend.counters->alive, 1);

    // Caller-driven termination: the identity record stays visible, and the
    // frozen ownership decision destroys the handle with it (contract block
    // 2, mirrored by the pool's own slot release on terminate).
    const Result<void> terminated = tracker.terminate(7U, 2);
    ASSERT_TRUE(terminated.ok()) << terminated.status().message();
    ASSERT_NE(tracker.find_track(7U), nullptr);
    EXPECT_EQ(tracker.find_track(7U)->state, TrackState::kTerminated);

    slots.destroy(7U);
    EXPECT_EQ(backend.counters->alive, 0);
    EXPECT_EQ(backend.counters->destroyed, 1);
    EXPECT_EQ(slots.size(), 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per eviction observation
TEST(TrackerBackendOrchestration, HandleDestroyedSynchronouslyOnPoolEviction) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTrackerOptions options;
    options.max_targets = 2;
    ObjectTracker tracker = make_tracker(options);

    FusionSideSessionSlots slots;
    for (const uint64_t id : {1U, 2U}) {
        const auto adopted = tracker.adopt_track(make_region(id, bounds), view, id);
        ASSERT_TRUE(adopted.ok()) << adopted.status().message();
        auto initialized = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
        ASSERT_TRUE(initialized.ok()) << initialized.status().message();
        slots.attach(id, initialized.take_value());
    }
    EXPECT_EQ(backend.counters->alive, 2);

    // Pool eviction is explicit and reported (RULE-06): the third adoption
    // evicts the oldest live track, and the harness destroys the evicted
    // track's handle in the same step (contract block 2).
    const auto adopted = tracker.adopt_track(make_region(3U, bounds), view, 3);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    ASSERT_EQ(adopted.value().evicted_track_ids.size(), 1U);
    EXPECT_EQ(adopted.value().evicted_track_ids[0], 1U);

    slots.destroy(adopted.value().evicted_track_ids[0]);
    EXPECT_EQ(backend.counters->alive, 1);
    EXPECT_EQ(backend.counters->destroyed, 1);
    EXPECT_EQ(slots.size(), 1U);
    EXPECT_NE(slots.find(2U), nullptr);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per reset observation
TEST(TrackerBackendOrchestration, HandleDestroyedSynchronouslyOnReset) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    ObjectTracker tracker = make_tracker();
    FusionSideSessionSlots slots;
    for (const uint64_t id : {1U, 2U, 3U}) {
        ASSERT_TRUE(tracker.adopt_track(make_region(id, bounds), view, id).ok());
        auto initialized = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
        ASSERT_TRUE(initialized.ok()) << initialized.status().message();
        slots.attach(id, initialized.take_value());
    }
    EXPECT_EQ(backend.counters->alive, 3);

    tracker.reset();
    EXPECT_EQ(tracker.track_count(), 0U);
    slots.destroy_all();
    EXPECT_EQ(backend.counters->alive, 0);
    EXPECT_EQ(backend.counters->destroyed, 3);
}

TEST(TrackerBackendOrchestration, OrchestrationAddsZeroPoolBytesUntilTheM713Slot) {
    // The SPI adds zero pool bytes until the M7-13 slot constant lands
    // (contract block 2): identical pool operation sequences with and without
    // sessions attached must produce identical byte_size at every checkpoint.
    const GrayImage image = make_noise_image(64, 48);
    const ImageView view = view_of(image);
    const RectF bounds{16.0F, 12.0F, 16.0F, 12.0F};

    int64_t baseline_after_adopt = 0;
    int64_t baseline_after_commit = 0;
    {
        ObjectTracker baseline = make_tracker();
        ASSERT_TRUE(baseline.adopt_track(make_region(7U, bounds), view, 1).ok());
        baseline_after_adopt = baseline.byte_size();
        const auto commit = baseline.commit_track_evidence(7U, placeholder_verification(7U),
                                                           TrackPositionEvidence{PositionScenario::kStationary, false},
                                                           std::nullopt, view, 2);
        ASSERT_TRUE(commit.ok()) << commit.status().message();
        baseline_after_commit = baseline.byte_size();
    }

    FakeTrackerBackend backend;
    ObjectTracker tracker = make_tracker();
    FusionSideSessionSlots slots;
    ASSERT_TRUE(tracker.adopt_track(make_region(7U, bounds), view, 1).ok());
    auto initialized = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    slots.attach(7U, initialized.take_value());
    EXPECT_EQ(tracker.byte_size(), baseline_after_adopt);

    TrackerSession& session = *slots.find(7U);
    as_fake(session).next_update_status = Status(ErrorCode::kBackendFailure, "fake-tracker corrupted");
    (void)session.update(view, TrackerUpdateRequest{bounds, {}}, clean_context());
    EXPECT_EQ(tracker.byte_size(), baseline_after_adopt);

    const auto commit = tracker.commit_track_evidence(7U, placeholder_verification(7U),
                                                      TrackPositionEvidence{PositionScenario::kStationary, false},
                                                      std::nullopt, view, 2);
    ASSERT_TRUE(commit.ok()) << commit.status().message();
    EXPECT_EQ(tracker.byte_size(), baseline_after_commit);

    slots.destroy_all();
    tracker.reset();
    EXPECT_EQ(tracker.byte_size(), 0);
}

// --- the cache exemption negative ----------------------------------------------

TEST(TrackerBackendOrchestration, SessionStateNeverEntersCapabilityResultCache) {
    FakeTrackerBackend backend;
    const GrayImage image = make_noise_image(33, 21);
    const ImageView view = view_of(image);
    const RectF prior{4.0F, 3.0F, 12.0F, 8.0F};

    auto initialized = backend.initialize(view, TrackerInitRequest{prior, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    TrackerSession& session = *initialized.value();

    // The RULE-07 key inputs — image content, ROI, backend identity, request
    // parameters — are identical for both calls. (The cache cannot even name
    // the capability: CapabilityKind carries kOcr/kDetection only.)
    CapabilityKeyFields fields;
    fields.image_fingerprint = 123456789U;
    fields.source_id = "orchestration-test-source";
    fields.roi = RectI{4, 3, 12, 8};
    fields.preprocessing_version = 1;
    fields.kind = CapabilityKind::kDetection;
    fields.backend_name = backend.info_value.name;
    fields.implementation_version = backend.info_value.implementation_version;
    fields.model_id = backend.info_value.model_id;
    fields.model_revision = backend.info_value.model_revision;
    fields.request_params_digest = 42U;
    const CacheKeyDigest key = mirador::capability_cache_digest(fields);

    const Result<TrackerUpdateResult> first = session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    ASSERT_TRUE(first.ok()) << first.status().message();

    // A naive RULE-07-keyed cache layer stores the first result.
    auto created_cache = CapabilityResultCache::create(4096);
    ASSERT_TRUE(created_cache.ok()) << created_cache.status().message();
    CapabilityResultCache cache = created_cache.take_value();
    CachedCapabilityResult stored;
    stored.kind = CapabilityKind::kDetection;
    DetectionRegion region;
    region.bounds = first.value().bounds;
    region.class_id = 0;
    region.confidence = first.value().confidence;
    stored.detection_regions = {region};
    ASSERT_TRUE(cache.insert(key, stored).ok()) << "cache rejected the demo entry";

    // The identical second call: the sequence-dependent update contract means
    // the session moves on (same input, different result — the DEC-012
    // "same input same result" clause explicitly does not apply here), while
    // the cache layer would have served the stale first bounds.
    const Result<TrackerUpdateResult> second = session.update(view, TrackerUpdateRequest{prior, {}}, clean_context());
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value(), backend.trajectory[1]);
    EXPECT_NE(second.value(), first.value());

    const Result<mirador::CapabilityPayload> served = cache.lookup(key);
    ASSERT_TRUE(served.ok()) << served.status().message();
    ASSERT_NE(served.value(), nullptr);
    ASSERT_EQ(served.value()->detection_regions.size(), 1U);
    EXPECT_EQ(served.value()->detection_regions[0].bounds, first.value().bounds);
    EXPECT_NE(served.value()->detection_regions[0].bounds, second.value().bounds)
        << "a capability-result cache intercepting update would serve stale bounds";
}

// --- coordinates (DOD-03), determinism and input immutability ------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per matrix cell
TEST(TrackerBackendOrchestration, CoordinateMatrixRotationOddSizeStrideAndFlushEdges) {
    FakeTrackerBackend backend;
    backend.pixel_echo = true;

    const int32_t width = 33;   // odd
    const int32_t height = 21;  // odd
    TrackerInitRequest init_request;
    init_request.initial_bounds = RectF{4.5F, 3.5F, 12.25F, 8.75F};
    const TrackerUpdateRequest update_request{RectF{4.5F, 3.5F, 12.25F, 8.75F}, {}};
    const TrackerUpdateRequest flush_request{RectF{0.5F, 0.5F, 32.0F, 20.0F}, {}};
    const TrackerUpdateRequest full_view_request{RectF{0.0F, 0.0F, 33.0F, 21.0F}, {}};

    bool has_reference = false;
    TrackerUpdateResult reference;
    for (const Rotation rotation : {Rotation::k0, Rotation::k90, Rotation::k180, Rotation::k270}) {
        for (const int64_t padding : {int64_t{0}, int64_t{16}}) {
            const GrayImage image = make_noise_image(width, height, padding);
            const ImageView view = view_of(image, rotation);

            auto initialized = backend.initialize(view, init_request, clean_context());
            ASSERT_TRUE(initialized.ok()) << initialized.status().message();
            TrackerSession& session = *initialized.value();

            const Result<TrackerUpdateResult> updated = session.update(view, update_request, clean_context());
            ASSERT_TRUE(updated.ok()) << updated.status().message();
            if (!has_reference) {
                reference = updated.value();
                has_reference = true;
            } else {
                // Identical presented content: bit-identical results whatever
                // the rotation metadata or the stride (contract block 6).
                EXPECT_EQ(updated.value(), reference)
                    << "rotation " << static_cast<int>(rotation) << " padding " << padding;
            }
            // The backend observes presented dimensions only; rotation is
            // caller-side metadata it never interprets.
            EXPECT_EQ(as_fake(session).last_width, width);
            EXPECT_EQ(as_fake(session).last_height, height);
            EXPECT_EQ(as_fake(session).last_rotation, rotation);
            EXPECT_EQ(as_fake(session).last_stride, static_cast<int64_t>(width) + padding);
            EXPECT_EQ(as_fake(session).last_format, PixelFormat::kGray8);

            // Flush-edge and full-view priors are valid and yield fully
            // inside results in every cell.
            for (const TrackerUpdateRequest* edge_request : {&flush_request, &full_view_request}) {
                const Result<TrackerUpdateResult> edge = session.update(view, *edge_request, clean_context());
                ASSERT_TRUE(edge.ok()) << edge.status().message();
                EXPECT_GE(edge.value().bounds.x, 0.0F);
                EXPECT_GE(edge.value().bounds.y, 0.0F);
                EXPECT_LE(edge.value().bounds.x + edge.value().bounds.width, static_cast<float>(width));
                EXPECT_LE(edge.value().bounds.y + edge.value().bounds.height, static_cast<float>(height));
            }

            // The identity-space round trip: the caller's recovery of the
            // result through its Transform2D chain is out of scope here (the
            // SPI reports prepared-space bounds), so the prepared-space value
            // itself must come back bit-identical on the replay.
            const Result<TrackerUpdateResult> replay = session.update(view, update_request, clean_context());
            ASSERT_TRUE(replay.ok()) << replay.status().message();
            EXPECT_EQ(replay.value(), updated.value());
        }
    }
}

TEST(TrackerBackendOrchestration, InputPixelsNeverModified) {
    FakeTrackerBackend backend;
    backend.pixel_echo = true;
    const GrayImage image = make_noise_image(33, 21, 16);
    const uint64_t before = buffer_hash(image);
    const ImageView view = view_of(image);

    auto initialized =
        backend.initialize(view, TrackerInitRequest{RectF{4.0F, 3.0F, 12.0F, 8.0F}, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    TrackerSession& session = *initialized.value();

    (void)session.update(view, TrackerUpdateRequest{RectF{4.0F, 3.0F, 12.0F, 8.0F}, {}}, clean_context());
    (void)session.update(view, TrackerUpdateRequest{RectF{std::nanf(""), 0.0F, 1.0F, 1.0F}, {}}, clean_context());
    (void)session.update(view, TrackerUpdateRequest{RectF{4.0F, 3.0F, 12.0F, 8.0F}, {}}, cancelled_context());

    EXPECT_EQ(buffer_hash(image), before) << "the backend modified the input pixels";
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per sequence step
TEST(TrackerBackendOrchestration, PerSequenceDeterminismAcrossSessionsAndBackends) {
    const GrayImage frame_zero = make_noise_image(33, 21);
    const GrayImage frame_one = make_noise_image(34, 22);
    const GrayImage frame_two = make_noise_image(35, 23);
    const std::vector<GrayImage> frames = {frame_zero, frame_one, frame_two};
    const RectF prior{4.5F, 3.5F, 12.25F, 8.75F};
    const TrackerInitRequest init_request{prior, {}};

    // Identical initialize inputs plus identical frame sequences: bit-identical
    // result sequences across sessions on one backend and across backend
    // instances (contract block 8).
    std::vector<std::vector<TrackerUpdateResult>> sequences;
    for (int run = 0; run < 3; ++run) {
        FakeTrackerBackend backend;
        backend.pixel_echo = true;
        auto initialized = backend.initialize(view_of(frame_zero), init_request, clean_context());
        ASSERT_TRUE(initialized.ok()) << initialized.status().message();
        std::vector<TrackerUpdateResult> sequence;
        for (const GrayImage& frame : frames) {
            const Result<TrackerUpdateResult> updated =
                initialized.value()->update(view_of(frame), TrackerUpdateRequest{prior, {}}, clean_context());
            ASSERT_TRUE(updated.ok()) << updated.status().message();
            sequence.push_back(updated.value());
        }
        sequences.push_back(std::move(sequence));
    }
    for (size_t step = 0; step < frames.size(); ++step) {
        EXPECT_EQ(sequences[0][step], sequences[1][step]) << "step " << step;
        EXPECT_EQ(sequences[1][step], sequences[2][step]) << "step " << step;
    }
}

// --- privacy (RULE-10/DOD-06) --------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one EXPECT per privacy negative
TEST(TrackerBackendOrchestration, OrchestrationWritesNoFilesAndStatusCarriesNoPixelMarker) {
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path();
    const auto before = snapshot_directory(temp);

    // A marker flows through the tracked pixels; it must never surface in any
    // Status message the orchestration produces (diagnostics are ids, counts
    // and short Status messages — contract block 9), and the whole run must
    // write nothing to the filesystem.
    const std::string marker = "M7-11-PRIVACY-MARKER";
    GrayImage image = make_gray_image(33, 21, std::byte{0});
    for (size_t index = 0; index < marker.size(); ++index) {
        image.pixels[index + 1] = static_cast<std::byte>(marker[index]);
    }
    const ImageView view = view_of(image);
    const RectF bounds{2.0F, 2.0F, 16.0F, 12.0F};

    FakeTrackerBackend backend;
    backend.pixel_echo = true;
    std::vector<std::string> messages;
    const auto record = [&messages](const auto& result) {
        if (!result.ok()) {
            messages.push_back(result.status().message());
        }
    };

    ObjectTracker tracker = make_tracker();
    FusionSideSessionSlots slots;
    const auto adopted = tracker.adopt_track(make_region(7U, bounds), view, 1);
    ASSERT_TRUE(adopted.ok()) << adopted.status().message();
    record(adopted);

    auto initialized = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    slots.attach(7U, initialized.take_value());
    TrackerSession& session = *slots.find(7U);

    // Harvest every error message the orchestration can produce.
    record(session.update(view, TrackerUpdateRequest{bounds, {}}, cancelled_context()));
    record(session.update(view, TrackerUpdateRequest{bounds, {}}, expired_deadline_context()));
    as_fake(session).next_update_status = Status(ErrorCode::kBackendFailure, "fake-tracker corrupted");
    record(session.update(view, TrackerUpdateRequest{bounds, {}}, clean_context()));
    record(tracker.commit_track_evidence(7U, placeholder_verification(7U),
                                         TrackPositionEvidence{PositionScenario::kStationary, false}, std::nullopt,
                                         view, 2));
    record(tracker.terminate(7U, 3));
    slots.destroy_all();
    record(tracker.terminate(7U, 4));  // unknown id after terminate: an error message
    tracker.reset();

    // The successful pixel-echo updates round the marker-bearing bytes into a
    // scalar; assert the result stays a pure bounds/confidence pair (finite,
    // no content channel exists on the type).
    auto second = backend.initialize(view, TrackerInitRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(second.ok()) << second.status().message();
    const Result<TrackerUpdateResult> updated =
        second.value()->update(view, TrackerUpdateRequest{bounds, {}}, clean_context());
    ASSERT_TRUE(updated.ok()) << updated.status().message();
    EXPECT_TRUE(std::isfinite(updated.value().bounds.x) && std::isfinite(updated.value().bounds.width));
    EXPECT_GE(updated.value().confidence, 0.0F);
    EXPECT_LE(updated.value().confidence, 1.0F);

    for (const std::string& message : messages) {
        EXPECT_FALSE(contains(message, marker)) << "status message leaked the pixel marker: " << message;
    }

    const auto after = snapshot_directory(temp);
    EXPECT_EQ(after.size(), before.size()) << "the orchestration wrote files";
    for (const auto& [name, type] : after) {
        EXPECT_EQ(before.count(name), 1U) << "new file appeared: " << name << " (" << static_cast<int>(type) << ")";
    }
}

}  // namespace
