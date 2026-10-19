#ifndef MIRADOR_PERCEPTION_SESSION_HPP
#define MIRADOR_PERCEPTION_SESSION_HPP

#include <mirador/capability_cache.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/evidence.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/frame_cache.hpp>
#include <mirador/fusion.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/ocr_batch.hpp>
#include <mirador/result.hpp>
#include <mirador/stable_id_tracker.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mirador {

/// Version of the session's deterministic preprocessing chain (ROI crop in the
/// oriented view -> format conversion -> `max_side` area resize). Bump whenever
/// the chain could change prepared pixels or their coordinate mapping; it is a
/// mandatory capability-result cache key field (RULE-07).
inline constexpr uint32_t kPreprocessPipelineVersion = 1;

/// Session configuration for one image source (design section 25). Defaults
/// are the DEC-008 frozen budgets; explicit non-positive budgets are rejected
/// rather than falling back to the defaults.
struct PerceptionSessionOptions {
    std::string source_id;  ///< identity of the image source this session tracks
    int64_t frame_cache_bytes = int64_t{4} * 1024 * 1024;
    int64_t result_cache_bytes = int64_t{16} * 1024 * 1024;
};

/// Session-level budgeted OCR request (DEC-025, frozen at M8-02): pipeline
/// fields for the shared preprocessing chain plus the backend-owned budget
/// knobs. No cache policy field exists by design — the budgeted entry never
/// touches the capability cache in M8 (DEC-025 section 5).
struct BudgetedOcrRequest {
    /// Pipeline-owned: crop, downscale cap and coordinate recovery (same
    /// semantics as `OcrRequest`, DEC-012).
    std::optional<RectF> roi;
    CoordinateSpaceId roi_space = CoordinateSpaceId::kOriented;
    int32_t max_side = 0;
    CoordinateSpaceId output_space = CoordinateSpaceId::kOriented;
    /// Space the caller's priority ROIs are declared in (kOriented or kFrame);
    /// they are converted to prepared-image space before execution.
    CoordinateSpaceId priority_space = CoordinateSpaceId::kOriented;
    /// Caller priorities; entry count/byte caps are validated per request.
    std::vector<OcrPriorityEntry> priorities;

    /// Backend-owned pass-through fields (same semantics as `OcrRequest`).
    std::string language_hint;
    float min_confidence = 0.0F;
    std::string backend_params;

    /// Budget knobs, forwarded to the backend (DEC-025 section 3): K must be
    /// positive; both durations are optional and explicitly enabled.
    int32_t max_rec_lines = 0;
    OcrSelectionStrategy selection = OcrSelectionStrategy::kReadingOrder;
    std::optional<std::chrono::steady_clock::duration> soft_budget;
    std::optional<std::chrono::steady_clock::duration> finalize_reserve;
    /// Tightened-only validation caps (see the kOcrBatchMax* constants).
    int32_t max_priority_entries = kOcrBatchMaxPriorityEntries;
    int64_t max_priority_bytes = kOcrBatchMaxPriorityBytes;
    int64_t max_report_bytes = kOcrBatchMaxReportBytes;
};

/// Budgeted OCR report with request binding (DEC-025 section 2 field table):
/// the backend's execution report with all line geometry recovered to
/// `output_space`, plus the binding fields a caller needs to audit the run.
/// Partial reports are diagnostic results for query/display/scheduling — they
/// never authorize actions by themselves (DEC-021 protocol unchanged).
struct BudgetedOcrReport {
    /// Execution report; `lines[*].bounds` live in `geometry_space`.
    OcrBatchResult batch;
    CoordinateSpaceId geometry_space = CoordinateSpaceId::kOriented;
    /// Session/source binding of the executed request.
    std::string source_id;
    uint64_t frame_sequence = 0;
    /// Executed pixel ROI in the oriented view (after crop/clamp).
    RectI executed_roi{};
    CoordinateSpaceId output_space = CoordinateSpaceId::kOriented;
    /// Backend identity snapshot for diagnostics; feeds cache keys on the
    /// complete path but the budgeted path stores nothing.
    BackendInfo backend_info;
    /// Digest of the effective (priority-converted) backend request.
    uint64_t request_digest = 0;
};

/// Per-image-source perception entry point (design sections 10, 18, 25; M2
/// scope of DEC-013): layered change analysis over bounded per-frame
/// signatures, on-demand Backend execution with coordinate recovery, and
/// capability-result caching. Evidence fusion, stable IDs and SemanticSnapshot
/// arrive in M4 on this same module.
///
/// Concurrency: the session is deliberately not thread-safe — one session
/// belongs to one scheduling context, matching the sync API boundary
/// (AGENTS.md). Caches stay inside the session unless the caller explicitly
/// shares them. All methods never throw and poll the ExecutionContext at stage
/// boundaries (kCancelled/kTimeout, never silently swallowed).
class PerceptionSession {
public:
    PerceptionSession() noexcept = default;
    PerceptionSession(const PerceptionSession&) = delete;
    PerceptionSession& operator=(const PerceptionSession&) = delete;
    PerceptionSession(PerceptionSession&&) noexcept = default;
    PerceptionSession& operator=(PerceptionSession&&) noexcept = default;
    ~PerceptionSession() noexcept = default;

    /// Creates a session with the given options. Returns kInvalidArgument for
    /// non-positive cache budgets or failing cache construction. Never throws.
    [[nodiscard]] static Result<PerceptionSession> create(PerceptionSessionOptions options) noexcept;

    [[nodiscard]] const std::string& source_id() const noexcept { return options_.source_id; }
    /// Frame-level artifact cache owned by the session (fingerprints,
    /// thumbnails, caller-defined intermediates).
    [[nodiscard]] FrameCache& frame_cache() noexcept { return frame_cache_; }
    /// Capability-result cache owned by the session (design section 12,
    /// second layer).
    [[nodiscard]] CapabilityResultCache& result_cache() noexcept { return result_cache_; }

    /// Compares `frame` against the previous frame of this source using the
    /// layered detector and stores the frame's compact signature (never the
    /// full frame). The first submission reports kGlobal with reason
    /// kFirstFrame: every existing result should be treated as stale. Cached
    /// capability results are intentionally not evicted on change — keys carry
    /// the frame fingerprint, so identical content later re-hits while changed
    /// content misses naturally.
    ///
    /// Errors: those of `detect_change`/`make_change_signature`, plus
    /// kInvalidArgument when the frame belongs to another source,
    /// kCancelled/kTimeout from the context. Never throws.
    [[nodiscard]] Result<ChangeReport> analyze_change(const Frame& frame, const ChangeDetectionParams& params = {},
                                                      const ExecutionContext& context = {}) noexcept;

    /// Runs OCR for `frame` through `backend` (null -> kBackendUnavailable):
    /// crops the request ROI in the oriented view, converts to the first
    /// backend-accepted format, downscales to `max_side` (kPreprocessPipelineVersion
    /// chain), recovers regions to `request.output_space`, and serves/stores
    /// the result through the capability cache per `request.cache_policy`
    /// (DEC-012). Regions of a cache hit are identical to the stored ones.
    ///
    /// Errors: kInvalidArgument (source mismatch, invalid view, invalid ROI or
    /// unsupported spaces), kBackendUnavailable (null backend or invalid
    /// info()), kUnsupportedFormat (no accepted format reachable),
    /// kBackendFailure/kCancelled/kTimeout from the backend or context,
    /// kBudgetExceeded for work buffers or cache insertion. Never throws.
    [[nodiscard]] Result<std::vector<TextRegion>> run_ocr(const Frame& frame, OcrBackend* backend,
                                                          const OcrRequest& request,
                                                          const ExecutionContext& context = {}) noexcept;

    /// Runs budgeted OCR for `frame` through `backend` (DEC-025, M8): the
    /// `run_ocr` preprocessing chain, then a bounded recognize with per-line
    /// completion states, caller priorities and soft budgets. The entry never
    /// reads or writes the capability result cache and never publishes state —
    /// a time-limited report must not be mistaken for the complete cached
    /// capability (AC8-08), so `source` binding below is diagnostic only.
    ///
    /// Request layering (DEC-012 one-consumer rule): `roi`/`roi_space`/
    /// `max_side`/`output_space`/`priority_space`/`priorities` are consumed by
    /// the pipeline (priority ROIs are converted to prepared-image space before
    /// the backend sees them); `language_hint`/`min_confidence`/
    /// `backend_params` and the budget knobs pass through to the backend.
    /// `finalize_reserve` requires a context deadline (kInvalidArgument
    /// otherwise). The returned report's line geometry is recovered to
    /// `output_space`; deferred lines keep queryable geometry.
    ///
    /// Errors: those of `run_ocr`'s pipeline stages, plus kInvalidArgument for
    /// an invalid budget request (see `validate_ocr_batch_request`) or a
    /// `finalize_reserve` without a deadline, kBackendFailure for a
    /// structurally inconsistent backend report, kBudgetExceeded when the
    /// report exceeds `max_report_bytes`. Hard timeout and cancellation fail
    /// the whole call — they never yield a partial report. Never throws.
    [[nodiscard]] Result<BudgetedOcrReport> run_ocr_budgeted(const Frame& frame, BudgetedOcrBackend* backend,
                                                             const BudgetedOcrRequest& request,
                                                             const ExecutionContext& context = {}) noexcept;

    /// Runs object/UI detection; identical pipeline and error contract as
    /// `run_ocr` (DEC-012).
    [[nodiscard]] Result<std::vector<DetectionRegion>> run_detector(const Frame& frame, DetectorBackend* backend,
                                                                    const DetectionRequest& request,
                                                                    const ExecutionContext& context = {}) noexcept;

    /// Fuses `evidence` into a `SemanticSnapshot` and publishes it (design
    /// sections 16 and 25, M4): converts evidence to
    /// `FusionOptions::target_space`, deterministically associates and merges
    /// it (FusionTrace in the returned FusionOutput is diagnostic only here),
    /// assigns stable ids through the session's tracker (DEC-010), bumps the
    /// snapshot generation per the tracker report and stores the immutable
    /// snapshot for `latest_snapshot`. The snapshot carries the most recent
    /// `ChangeReport` observed by `analyze_change` for this source.
    ///
    /// Errors: those of `fuse_evidence` and `StableIdTracker::advance`
    /// (kInvalidArgument, kCancelled/kTimeout, kBudgetExceeded), plus
    /// kInvalidArgument when the frame belongs to another source or is
    /// invalid. On error nothing is published and the tracker state is
    /// untouched. Never throws.
    [[nodiscard]] Result<SemanticSnapshot> fuse(const Frame& frame, const EvidenceSet& evidence,
                                                const FusionOptions& options = {},
                                                const ExecutionContext& context = {}) noexcept;

    /// The most recently published snapshot, or nullptr before the first
    /// `fuse`. The pointee is immutable; concurrent readers keep the snapshot
    /// alive across later fusions (design section 18).
    [[nodiscard]] std::shared_ptr<const SemanticSnapshot> latest_snapshot() const noexcept { return latest_snapshot_; }

    /// Largest stable id this session has assigned so far (diagnostics,
    /// RULE-09: ids are unique within this session only).
    [[nodiscard]] uint64_t last_stable_id() const noexcept { return tracker_.last_id(); }

private:
    explicit PerceptionSession(PerceptionSessionOptions options, FrameCache frame_cache,
                               CapabilityResultCache result_cache) noexcept;

    PerceptionSessionOptions options_;
    FrameCache frame_cache_;
    CapabilityResultCache result_cache_;
    /// Previous frame signature of this source; nullopt until the first
    /// `analyze_change` (design section 18: compact state, no full frames).
    std::optional<ChangeSignature> previous_signature_;
    /// Most recent change decision, embedded in every published snapshot.
    ChangeReport last_change_;
    /// Cross-snapshot stable-id tracker and generation counter (M4, DEC-010).
    StableIdTracker tracker_;
    uint64_t generation_ = 0;
    std::shared_ptr<const SemanticSnapshot> latest_snapshot_;
};

}  // namespace mirador

#endif  // MIRADOR_PERCEPTION_SESSION_HPP
