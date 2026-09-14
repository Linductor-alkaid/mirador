#ifndef MIRADOR_PERCEPTION_SESSION_HPP
#define MIRADOR_PERCEPTION_SESSION_HPP

#include <mirador/capability_cache.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/frame_cache.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>

#include <cstdint>
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

    /// Runs object/UI detection; identical pipeline and error contract as
    /// `run_ocr` (DEC-012).
    [[nodiscard]] Result<std::vector<DetectionRegion>> run_detector(const Frame& frame, DetectorBackend* backend,
                                                                    const DetectionRequest& request,
                                                                    const ExecutionContext& context = {}) noexcept;

private:
    explicit PerceptionSession(PerceptionSessionOptions options, FrameCache frame_cache,
                               CapabilityResultCache result_cache) noexcept;

    PerceptionSessionOptions options_;
    FrameCache frame_cache_;
    CapabilityResultCache result_cache_;
    /// Previous frame signature of this source; nullopt until the first
    /// `analyze_change` (design section 18: compact state, no full frames).
    std::optional<ChangeSignature> previous_signature_;
};

}  // namespace mirador

#endif  // MIRADOR_PERCEPTION_SESSION_HPP
