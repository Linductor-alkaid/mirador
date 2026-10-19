#ifndef MIRADOR_OCR_BATCH_HPP
#define MIRADOR_OCR_BATCH_HPP

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mirador {

/// Default validation caps of the budgeted OCR contract (DEC-025 section 2).
/// They are reference limits, not verified performance claims; callers may
/// tighten them per request through the request fields below. Loosening a cap
/// beyond these constants is rejected: the defaults bound the contract's
/// worst-case memory, and a caller needing more must build its own path.
inline constexpr int32_t kOcrBatchMaxPriorityEntries = 64;
inline constexpr int64_t kOcrBatchMaxPriorityBytes = int64_t{64} * 1024;
inline constexpr int64_t kOcrBatchMaxReportBytes = int64_t{16} * 1024 * 1024;

/// DEC-025 section 3 reference configuration (UNVALIDATED initial values): the
/// M8-01 harness measures the longest atomic unit and finalize cost, M8-06
/// freezes passable values. They are named so callers and benchmarks agree on
/// what "the reference configuration" means; nothing in the library applies
/// them implicitly — every budget knob stays explicitly caller-provided.
inline constexpr int32_t kOcrBatchReferenceMaxRecLines = 64;
inline constexpr std::chrono::steady_clock::duration kOcrBatchReferenceFinalizeReserve{
    std::chrono::milliseconds{500}};

/// Per-line completion state of one budgeted OCR run (DEC-025 section 2). The
/// state is the only source of truth: an empty `utf8_text` never encodes
/// "not recognized" — a recognized line may legitimately decode to empty text.
enum class OcrCompletionState : uint8_t {
    /// A recognition forward ran for this line. `utf8_text` carries the decoded
    /// text (possibly empty) and `rec_confidence` is set.
    kRecognized,
    /// The text was served from a per-line reuse source (DEC-025 section 5).
    /// Reserved for M9; the M8 contract never produces it and implementations
    /// must not emit it before that decision is frozen.
    kReused,
    /// No recognition ran for this line: geometry and det score are kept, text
    /// stays empty and `rec_confidence` stays unset. The line remains queryable.
    kDeferred,
};

/// Why a budgeted run stopped scheduling recognition work (DEC-025 section 2).
/// Hard-deadline timeouts and cancellation are NOT stop reasons: they fail the
/// whole call through `Result` (kTimeout/kCancelled) and never yield a report.
enum class OcrBatchStopReason : uint8_t {
    /// Every selected line was recognized; either all candidates were selected
    /// or the selection budget never bound. The only stop reason of an empty
    /// candidate set.
    kCompleted,
    /// The run recognized exactly `max_rec_lines` lines and deferred the rest.
    kRecLineLimit,
    /// A caller-configured soft budget stopped new work before the line limit:
    /// either `soft_budget` elapsed or the remaining time to the hard deadline
    /// no longer fit one measured atomic unit plus `finalize_reserve`.
    kSoftBudget,
};

/// Overall completeness of a report (DEC-025 section 2). kComplete means every
/// detected candidate line has a final record in this report; it does NOT claim
/// that detection recalled all text on the image (check `det_limit_hit`).
enum class OcrBatchCompletion : uint8_t { kComplete, kPartial };

/// Candidate selection order for recognition work (DEC-025 section 4). The
/// strategy orders equal-priority lines; caller priorities always dominate.
enum class OcrSelectionStrategy : uint8_t {
    /// Deterministic reading order: top-to-bottom, then left-to-right (the
    /// frozen PP-OCR reference order).
    kReadingOrder,
    /// Detection score descending; ties fall back to reading order.
    kDetScore,
};

/// One candidate line of a budgeted run: detection geometry plus, when
/// recognition ran for it, the decoded text (DEC-025 section 2 per-box record).
/// `bounds` lives in the backend's reported space — prepared-image pixel space
/// at the SPI layer ([0, w) x [0, h)), the request's output space once a
/// session entry has recovered it (DEC-012).
struct OcrLineRecord {
    /// Zero-based index of this line in the run's detection output order (the
    /// det stage's own deterministic order, before any reordering). Unique
    /// within one report; the stable handle for "deferred geometry" queries.
    int32_t candidate_id = -1;
    RectF bounds{};
    /// Detection stage score; never modified by selection or priorities.
    float det_confidence = 0.0F;
    OcrCompletionState state = OcrCompletionState::kDeferred;
    /// Decoded text; non-empty only when `state != kDeferred`. Empty text with
    /// kRecognized means the recognizer decoded nothing (a valid outcome).
    std::string utf8_text;
    /// Recognition score; set iff `state == kRecognized` (kReused carries its
    /// own source semantics in M9). Never fabricated for deferred lines.
    std::optional<float> rec_confidence;
    /// Zero-based position in the run's recognition execution sequence; -1 for
    /// deferred lines. The output list stays in reading order regardless.
    int32_t execution_order = -1;

    [[nodiscard]] friend bool operator==(const OcrLineRecord& lhs, const OcrLineRecord& rhs) noexcept {
        return lhs.candidate_id == rhs.candidate_id && lhs.bounds == rhs.bounds &&
               lhs.det_confidence == rhs.det_confidence && lhs.state == rhs.state &&
               lhs.utf8_text == rhs.utf8_text && lhs.rec_confidence == rhs.rec_confidence &&
               lhs.execution_order == rhs.execution_order;
    }
};

/// Caller priority entry (DEC-025 section 4): lines whose bounds intersect
/// `roi` compete first, ordered by descending `priority`. Entries never modify
/// det/rec confidence and never add text. At the SPI layer the ROI is in
/// prepared-image pixel space; session entries convert from the caller's
/// declared space before invoking the backend.
struct OcrPriorityEntry {
    RectF roi{};
    /// Higher wins. Any finite float is accepted; callers typically use 0..1.
    float priority = 0.0F;

    [[nodiscard]] friend bool operator==(const OcrPriorityEntry& lhs, const OcrPriorityEntry& rhs) noexcept {
        return lhs.roi == rhs.roi && lhs.priority == rhs.priority;
    }
};

/// Budgeted OCR execution request (DEC-025 sections 2-4; names frozen at
/// M8-02). One type serves the `BudgetedOcrBackend` SPI; session entries
/// translate their pipeline-owned fields into it. Fields follow the DEC-012
/// one-consumer rule:
/// - consumed by the backend: `language_hint`, `min_confidence` (det-candidate
///   gating; recognition scores are reported, never text-gated),
///   `backend_params`, every selection/budget field;
/// - pre-resolved by the caller/session: priority ROIs arrive in prepared-image
///   pixel space.
struct OcrBatchRequest {
    std::string language_hint;  ///< e.g. "zh-Hans"; empty = implementation default
    float min_confidence = 0.0F;
    std::string backend_params;  ///< opaque, implementation-defined
    /// Recognition line budget K. Must be > 0; at most min(K, N) lines are
    /// recognized. Det-only callers use a det backend instead (DEC-025).
    int32_t max_rec_lines = 0;
    OcrSelectionStrategy selection = OcrSelectionStrategy::kReadingOrder;
    /// Caller priorities; entry count/byte caps are validated per request.
    std::vector<OcrPriorityEntry> priorities;
    /// Soft budget: once this much time has elapsed since the call started, no
    /// new recognition unit is started and the run returns partial (DEC-025
    /// section 3). nullopt disables it.
    std::optional<std::chrono::steady_clock::duration> soft_budget;
    /// Finalize reserve: with a hard deadline set, a recognition unit is not
    /// started when the remaining time does not fit one measured atomic unit
    /// plus this reserve (the unit upper bound is implementation knowledge).
    /// Requires a context deadline; nullopt disables the guard.
    std::optional<std::chrono::steady_clock::duration> finalize_reserve;
    /// Tightened-only validation caps (see the kOcrBatchMax* constants).
    int32_t max_priority_entries = kOcrBatchMaxPriorityEntries;
    int64_t max_priority_bytes = kOcrBatchMaxPriorityBytes;
    int64_t max_report_bytes = kOcrBatchMaxReportBytes;
};

/// Budgeted execution report of one valid run (DEC-025 section 2). Count
/// conservation is a hard invariant:
/// `detected_count == recognized_count + reused_count + deferred_count` and
/// `lines.size() == detected_count`, every candidate appearing exactly once.
/// Session entries wrap this report with request binding fields; the SPI value
/// itself stays binding-free so implementations never see session identity.
struct OcrBatchResult {
    /// All detected candidate lines in deterministic reading order (recognized,
    /// reused and deferred alike) — deferred geometry stays queryable here.
    std::vector<OcrLineRecord> lines;
    /// Detection boxes that became candidates of this run (post min_confidence).
    int32_t detected_count = 0;
    /// The backend's detection cap; 0 when the implementation declares none.
    int32_t det_limit = 0;
    /// True when detection hit `det_limit`: boxes past the cap were dropped and
    /// coverage is uncertain even at completion kComplete.
    bool det_limit_hit = false;
    int32_t recognized_count = 0;
    int32_t reused_count = 0;  ///< M8 first version: always 0 (DEC-025 section 2)
    int32_t deferred_count = 0;
    /// Actual recognition forwards. AC8-03 requires this to equal the number of
    /// lines whose state is kRecognized (batching, if ever adopted, must keep
    /// the equality or report per-forward sample counts through a reviewed
    /// contract extension).
    int32_t rec_forward_count = 0;
    OcrBatchStopReason stop_reason = OcrBatchStopReason::kCompleted;
    OcrBatchCompletion completion = OcrBatchCompletion::kComplete;

    [[nodiscard]] friend bool operator==(const OcrBatchResult& lhs, const OcrBatchResult& rhs) noexcept {
        return lhs.lines == rhs.lines && lhs.detected_count == rhs.detected_count &&
               lhs.det_limit == rhs.det_limit && lhs.det_limit_hit == rhs.det_limit_hit &&
               lhs.recognized_count == rhs.recognized_count && lhs.reused_count == rhs.reused_count &&
               lhs.deferred_count == rhs.deferred_count && lhs.rec_forward_count == rhs.rec_forward_count &&
               lhs.stop_reason == rhs.stop_reason && lhs.completion == rhs.completion;
    }
};

/// Synchronous budgeted OCR Backend SPI (DEC-025 section 2, frozen at M8-02).
/// Deliberately NOT derived from `OcrBackend`: old backends stay untouched, and
/// a budgeted implementation does not have to expose an all-or-nothing
/// `recognize`. Callers inject it explicitly; a session entry without one fails
/// with kBackendUnavailable instead of silently falling back to full
/// recognition. The same image/domain-only boundary applies (DEC-002): no
/// runtime tensor or scheduling type crosses this interface.
///
/// Contract highlights (full text in DEC-025; enforce via the helpers below):
/// - Validation happens before any new work; malformed requests and over-limit
///   reports fail with kInvalidArgument / kBudgetExceeded and no partial data.
/// - Soft budgets produce successful partial reports; hard deadline produces
///   kTimeout and cancellation kCancelled, both discarding the prefix (a
///   context-observed cancellation always beats a simultaneously-expired
///   deadline). Model/illegal-output failures and allocation failures keep the
///   whole call failed — never a partial success.
/// - Deterministic for equal inputs under equal fixed configuration (K,
///   selection, priorities): the selected set, texts and orders are bitwise
///   stable; time-stopped runs may vary in the completed count but must keep
///   the priority prefix and every bookkeeping invariant.
class BudgetedOcrBackend {
public:
    virtual ~BudgetedOcrBackend() = default;

    /// Capability and identity query, same rules as `OcrBackend::info` (cache
    /// keys, format gating, thread-safety declaration).
    [[nodiscard]] virtual BackendInfo info() const = 0;

    /// Runs bounded detection + recognition on an already-prepared image and
    /// reports lines in prepared-image pixel space ([0, w) x [0, h)); the
    /// caller recovers them to its output space (DEC-012). Implementations must
    /// validate the request first, honor `min_confidence` at the detection
    /// stage, keep every invariant of `OcrBatchResult`, and poll `context` at
    /// stage boundaries (kCancelled/kTimeout, never silently swallowed).
    virtual Result<OcrBatchResult> recognize_budgeted(const ImageView& prepared_image, const OcrBatchRequest& request,
                                                      const ExecutionContext& context) = 0;
};

/// Validates a budgeted request against its own caps and the contract defaults
/// (DEC-025 section 2: explicit validation, no silent truncation). Checks:
/// `max_rec_lines > 0`; finite priority entries within count/byte caps
/// (over-cap -> kBudgetExceeded); positive report byte cap; non-negative
/// durations. Whether a `finalize_reserve` is paired with a hard deadline
/// cannot be known here — session entries enforce that pairing with
/// kInvalidArgument before invoking a backend. Never throws.
[[nodiscard]] Result<void> validate_ocr_batch_request(const OcrBatchRequest& request) noexcept;

/// Effective priority of one line: the highest priority among entries whose
/// ROI intersects `bounds`; 0 exactly when no entry applies (matched entries
/// may rank below it — a negative priority is meaningful, and NaN ranks
/// last). Deterministic and order-independent.
[[nodiscard]] float resolve_ocr_line_priority(const RectF& bounds, const std::vector<OcrPriorityEntry>& priorities) noexcept;

/// Total execution order over candidate lines (DEC-025 section 4): caller
/// priority descending, then the strategy key (reading order, or det score
/// descending), then reading-order geometry, then `candidate_id` as the final
/// tie-break. Returns true when `lhs` executes before `rhs`. NaN priorities
/// compare as "never first"; consumers validate finiteness before selection.
[[nodiscard]] bool ocr_line_executes_before(const OcrLineRecord& lhs, const OcrLineRecord& rhs,
                                            float lhs_priority, float rhs_priority,
                                            OcrSelectionStrategy strategy) noexcept;

/// Reading order over candidate lines: bounds y, then x, then -width, then
/// -height (longer lines first among equal origins), then det confidence
/// descending, then candidate_id. A strict total order for equal inputs.
[[nodiscard]] bool ocr_line_reading_less(const OcrLineRecord& lhs, const OcrLineRecord& rhs) noexcept;

/// Bytes owned by a report's buffers (text strings + line vector overhead),
/// used to enforce `max_report_bytes` (DEC-025 section 2). Deterministic.
[[nodiscard]] int64_t ocr_batch_result_owned_bytes(const OcrBatchResult& result) noexcept;

/// Structural consistency of a report against the DEC-025 invariants: count
/// conservation, unique candidate ids, non-negative execution orders exactly
/// for executed lines, no text or rec score on deferred lines, rec score set
/// exactly on recognized lines, stop reason / completion coherence (kCompleted
/// implies kComplete; kRecLineLimit/kSoftBudget with any deferred line imply
/// kPartial). Diagnostics only — implementations must construct valid reports,
/// not filter them through this check.
[[nodiscard]] bool ocr_batch_result_consistent(const OcrBatchResult& result) noexcept;

/// Stable parameter digest of the selection-relevant request fields (K,
/// strategy, priorities, soft budget configuration, caps, language hint and
/// opaque params) for session binding diagnostics and future per-line cache
/// keys (DEC-025 section 5). Never throws.
[[nodiscard]] uint64_t ocr_batch_request_params_digest(const OcrBatchRequest& request) noexcept;

}  // namespace mirador

#endif  // MIRADOR_OCR_BATCH_HPP
