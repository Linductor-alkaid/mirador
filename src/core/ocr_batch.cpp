#include <mirador/ocr_batch.hpp>

#include <mirador/status.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace mirador {
namespace {

// FNV-1a 64-bit over a canonical field-by-field feed (same pattern as the
// capability cache key digest; a distinct domain string keeps the hash spaces
// separate). The serialization never materializes.
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x00000100000001b3ULL;

class DigestHasher {
public:
    explicit DigestHasher(const char* domain) noexcept : state_(kFnvOffsetBasis) {
        while (*domain != '\0') {
            mix(static_cast<uint8_t>(*domain++));
        }
        mix(0);
    }

    void u64(uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            mix(static_cast<uint8_t>(value >> (8 * i)));
        }
    }
    void u32(uint32_t value) noexcept {
        for (int i = 0; i < 4; ++i) {
            mix(static_cast<uint8_t>(value >> (8 * i)));
        }
    }
    void f32(float value) noexcept {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "32-bit IEEE 754 float expected");
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void str(const std::string& value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (const char c : value) {
            mix(static_cast<uint8_t>(c));
        }
    }

    [[nodiscard]] uint64_t state() const noexcept { return state_; }

private:
    void mix(uint8_t byte) noexcept {
        state_ ^= byte;
        state_ *= kFnvPrime;
    }

    uint64_t state_;
};

// Floats enter orders and digests sanitized: NaN behaves as the lowest value
// so comparisons stay a strict total order and digests stay stable.
float ordered(float value) noexcept { return std::isnan(value) ? -std::numeric_limits<float>::infinity() : value; }

std::chrono::steady_clock::duration::rep duration_ticks(const std::chrono::steady_clock::duration& duration) noexcept {
    return duration.count();
}

RectF sanitized_rect(const RectF& rect) noexcept {
    return RectF{ordered(rect.x), ordered(rect.y), ordered(rect.width), ordered(rect.height)};
}

/// Positive-area intersection of two finite rectangles.
bool rects_overlap(const RectF& first, const RectF& second) noexcept {
    const RectF a = sanitized_rect(first);
    const RectF b = sanitized_rect(second);
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height &&
           std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x) > 0.0F &&
           std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y) > 0.0F;
}

}  // namespace

Result<void> validate_ocr_batch_request(const OcrBatchRequest& request) noexcept {
    if (request.max_rec_lines <= 0) {
        return Status(ErrorCode::kInvalidArgument, "max_rec_lines must be positive");
    }
    if (request.max_priority_entries <= 0 || request.max_priority_entries > kOcrBatchMaxPriorityEntries) {
        return Status(ErrorCode::kInvalidArgument, "max_priority_entries must be in (0, kOcrBatchMaxPriorityEntries]");
    }
    if (request.max_priority_bytes <= 0 || request.max_priority_bytes > kOcrBatchMaxPriorityBytes) {
        return Status(ErrorCode::kInvalidArgument, "max_priority_bytes must be in (0, kOcrBatchMaxPriorityBytes]");
    }
    if (request.max_report_bytes <= 0 || request.max_report_bytes > kOcrBatchMaxReportBytes) {
        return Status(ErrorCode::kInvalidArgument, "max_report_bytes must be in (0, kOcrBatchMaxReportBytes]");
    }
    if (request.soft_budget.has_value() && request.soft_budget->count() < 0) {
        return Status(ErrorCode::kInvalidArgument, "soft_budget must be non-negative");
    }
    if (request.finalize_reserve.has_value() && request.finalize_reserve->count() < 0) {
        return Status(ErrorCode::kInvalidArgument, "finalize_reserve must be non-negative");
    }
    if (static_cast<int32_t>(request.priorities.size()) > request.max_priority_entries) {
        return Status(ErrorCode::kBudgetExceeded, "priority entries exceed max_priority_entries");
    }
    const int64_t priority_bytes = static_cast<int64_t>(request.priorities.size()) * static_cast<int64_t>(sizeof(OcrPriorityEntry));
    if (priority_bytes > request.max_priority_bytes) {
        return Status(ErrorCode::kBudgetExceeded, "priority entries exceed max_priority_bytes");
    }
    for (const OcrPriorityEntry& entry : request.priorities) {
        if (!std::isfinite(entry.roi.x) || !std::isfinite(entry.roi.y) || !std::isfinite(entry.roi.width) ||
            !std::isfinite(entry.roi.height) || !std::isfinite(entry.priority)) {
            return Status(ErrorCode::kInvalidArgument, "priority entries must be finite");
        }
        if (entry.roi.width < 0.0F || entry.roi.height < 0.0F) {
            return Status(ErrorCode::kInvalidArgument, "priority roi dimensions must be non-negative");
        }
    }
    return Status::success();
}

float resolve_ocr_line_priority(const RectF& bounds, const std::vector<OcrPriorityEntry>& priorities) noexcept {
    bool matched = false;
    float priority = 0.0F;
    for (const OcrPriorityEntry& entry : priorities) {
        if (rects_overlap(bounds, entry.roi)) {
            // Matched entries may rank below unprioritized lines (a negative
            // priority is meaningful); only NaN ranks last (-inf), and 0 is
            // returned exclusively when no entry applies.
            const float value = ordered(entry.priority);
            if (!matched || value > priority) {
                priority = value;
                matched = true;
            }
        }
    }
    return priority;
}

bool ocr_line_reading_less(const OcrLineRecord& lhs, const OcrLineRecord& rhs) noexcept {
    const RectF a = sanitized_rect(lhs.bounds);
    const RectF b = sanitized_rect(rhs.bounds);
    if (a.y != b.y) {
        return a.y < b.y;
    }
    if (a.x != b.x) {
        return a.x < b.x;
    }
    if (a.width != b.width) {
        return a.width > b.width;  // longer line first among equal origins
    }
    if (a.height != b.height) {
        return a.height > b.height;
    }
    const float score_a = ordered(lhs.det_confidence);
    const float score_b = ordered(rhs.det_confidence);
    if (score_a != score_b) {
        return score_a > score_b;
    }
    return lhs.candidate_id < rhs.candidate_id;
}

bool ocr_line_executes_before(const OcrLineRecord& lhs, const OcrLineRecord& rhs, float lhs_priority,
                              float rhs_priority, OcrSelectionStrategy strategy) noexcept {
    const float priority_a = ordered(lhs_priority);
    const float priority_b = ordered(rhs_priority);
    if (priority_a != priority_b) {
        return priority_a > priority_b;
    }
    if (strategy == OcrSelectionStrategy::kDetScore) {
        const float score_a = ordered(lhs.det_confidence);
        const float score_b = ordered(rhs.det_confidence);
        if (score_a != score_b) {
            return score_a > score_b;
        }
    }
    return ocr_line_reading_less(lhs, rhs);
}

int64_t ocr_batch_result_owned_bytes(const OcrBatchResult& result) noexcept {
    int64_t bytes = static_cast<int64_t>(sizeof(OcrBatchResult));
    for (const OcrLineRecord& line : result.lines) {
        bytes += static_cast<int64_t>(sizeof(OcrLineRecord));
        bytes += static_cast<int64_t>(line.utf8_text.capacity());
    }
    return bytes;
}

bool ocr_batch_result_consistent(const OcrBatchResult& result) noexcept {
    const int32_t detected = static_cast<int32_t>(result.lines.size());
    if (detected != result.detected_count) {
        return false;
    }
    if (result.detected_count != result.recognized_count + result.reused_count + result.deferred_count) {
        return false;
    }
    // M8 contract freeze (DEC-025 section 2): the reuse channel stays silent
    // until M9 freezes its source semantics.
    if (result.reused_count != 0) {
        return false;
    }
    if (result.rec_forward_count != result.recognized_count + result.reused_count) {
        return false;
    }
    if (result.det_limit_hit && result.det_limit <= 0) {
        return false;
    }

    int32_t recognized = 0;
    int32_t reused = 0;
    int32_t deferred = 0;
    std::vector<int32_t> ids;
    std::vector<int32_t> execution_orders;
    ids.reserve(result.lines.size());
    for (const OcrLineRecord& line : result.lines) {
        if (line.candidate_id < 0) {
            return false;
        }
        ids.push_back(line.candidate_id);
        switch (line.state) {
            case OcrCompletionState::kRecognized:
                if (!line.rec_confidence.has_value()) {
                    return false;
                }
                if (line.execution_order < 0) {
                    return false;
                }
                execution_orders.push_back(line.execution_order);
                ++recognized;
                break;
            case OcrCompletionState::kReused:
                ++reused;
                break;
            case OcrCompletionState::kDeferred:
                if (!line.utf8_text.empty() || line.rec_confidence.has_value()) {
                    return false;  // deferred lines carry geometry only
                }
                if (line.execution_order != -1) {
                    return false;
                }
                ++deferred;
                break;
        }
    }
    if (recognized != result.recognized_count || reused != result.reused_count || deferred != result.deferred_count) {
        return false;
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return false;  // duplicate candidate id
    }
    // Execution orders of the recognized lines are a permutation of
    // 0..recognized-1 (reading order and execution order are independent).
    std::sort(execution_orders.begin(), execution_orders.end());
    for (size_t index = 0; index < execution_orders.size(); ++index) {
        if (execution_orders[index] != static_cast<int32_t>(index)) {
            return false;
        }
    }

    // Stop reason / completion coherence: kCompleted means nothing was
    // deferred; a line-limit or soft stop always leaves deferred lines (a stop
    // with nothing left to defer is reported as kCompleted).
    const bool has_deferred = result.deferred_count > 0;
    switch (result.stop_reason) {
        case OcrBatchStopReason::kCompleted:
            if (has_deferred || result.completion != OcrBatchCompletion::kComplete) {
                return false;
            }
            break;
        case OcrBatchStopReason::kRecLineLimit:
        case OcrBatchStopReason::kSoftBudget:
            if (!has_deferred || result.completion != OcrBatchCompletion::kPartial) {
                return false;
            }
            break;
    }
    return true;
}

uint64_t ocr_batch_request_params_digest(const OcrBatchRequest& request) noexcept {
    DigestHasher hasher("mirador.ocr_batch_request.v1");
    hasher.str(request.language_hint);
    hasher.f32(request.min_confidence);
    hasher.str(request.backend_params);
    hasher.u32(static_cast<uint32_t>(request.max_rec_lines));
    hasher.u32(static_cast<uint32_t>(request.selection));
    hasher.u64(static_cast<uint64_t>(request.priorities.size()));
    for (const OcrPriorityEntry& entry : request.priorities) {
        const RectF rect = sanitized_rect(entry.roi);
        hasher.f32(rect.x);
        hasher.f32(rect.y);
        hasher.f32(rect.width);
        hasher.f32(rect.height);
        hasher.f32(ordered(entry.priority));
    }
    hasher.u64(request.soft_budget.has_value() ? static_cast<uint64_t>(duration_ticks(*request.soft_budget))
                                               : 0);
    hasher.u64(request.finalize_reserve.has_value() ? static_cast<uint64_t>(duration_ticks(*request.finalize_reserve))
                                                    : 0);
    hasher.u32(static_cast<uint32_t>(request.max_priority_entries));
    hasher.u64(static_cast<uint64_t>(request.max_priority_bytes));
    hasher.u64(static_cast<uint64_t>(request.max_report_bytes));
    return hasher.state();
}

}  // namespace mirador
