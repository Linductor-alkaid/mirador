#include <mirador/stable_id_tracker.hpp>

#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/text_normalize.hpp>

#include "rect_math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mirador {
namespace {

using fusion_internal::center_distance;
using fusion_internal::rect_iou;

// Stage-boundary cancellation/deadline check (same contract as the session).
Status context_status(const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "stable-id advance cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "stable-id advance deadline exceeded"};
    }
    return {};
}

Status validate_options(const StableIdOptions& options) noexcept {
    if (!std::isfinite(options.match_iou_threshold) || options.match_iou_threshold < 0.0 ||
        options.match_iou_threshold > 1.0) {
        return {ErrorCode::kInvalidArgument, "match_iou_threshold must be finite in [0, 1]"};
    }
    if (!std::isfinite(options.center_gate_ratio) || options.center_gate_ratio < 0.0) {
        return {ErrorCode::kInvalidArgument, "center_gate_ratio must be finite and non-negative"};
    }
    for (const double weight : {options.weight_iou, options.weight_center, options.weight_text}) {
        if (!std::isfinite(weight) || weight < 0.0) {
            return {ErrorCode::kInvalidArgument, "cost weights must be finite and non-negative"};
        }
    }
    if (options.weight_iou + options.weight_center + options.weight_text <= 0.0) {
        return {ErrorCode::kInvalidArgument, "at least one cost weight must be positive"};
    }
    if (!std::isfinite(options.generation_retention_ratio) || options.generation_retention_ratio < 0.0 ||
        options.generation_retention_ratio > 1.0) {
        return {ErrorCode::kInvalidArgument, "generation_retention_ratio must be finite in [0, 1]"};
    }
    if (options.max_regions <= 0) {
        return {ErrorCode::kInvalidArgument, "max_regions must be positive"};
    }
    return {};
}

std::string normalized_text(std::string_view text) {
    const uint32_t flags = static_cast<uint32_t>(TextNormalizeFlags::kTrim) |
                           static_cast<uint32_t>(TextNormalizeFlags::kCollapseWhitespace) |
                           static_cast<uint32_t>(TextNormalizeFlags::kStripControl) |
                           static_cast<uint32_t>(TextNormalizeFlags::kFoldFullwidthAscii);
    std::string normalized = normalize_text(text, flags);
    if (normalized.size() > StableIdTracker::kMaxTextBytes) {
        normalized.resize(StableIdTracker::kMaxTextBytes);
    }
    return normalized;
}

/// Byte-level Levenshtein over the (already normalized, truncated) texts;
/// deterministic rolling-row DP without heap use.
double text_similarity(std::string_view a, std::string_view b) noexcept {
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    std::vector<size_t> row(b.size() + 1, 0);
    for (size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t above = row[j];
            const size_t substitution = a[i - 1] == b[j - 1] ? 0U : 1U;
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + substitution});
            diagonal = above;
        }
    }
    const size_t longest = std::max(a.size(), b.size());
    return 1.0 - static_cast<double>(row[b.size()]) / static_cast<double>(longest);
}

/// Normalized greedy assignment cost (DEC-010): weighted mean of the active
/// mismatch signals; the text signal is active only when both texts exist.
double match_cost(double iou, double center_proximity, double text_signal, bool text_active,
                  const StableIdOptions& options) noexcept {
    double weighted = options.weight_iou * (1.0 - iou) + options.weight_center * (1.0 - center_proximity);
    double weights = options.weight_iou + options.weight_center;
    if (text_active) {
        weighted += options.weight_text * (1.0 - text_signal);
        weights += options.weight_text;
    }
    return weighted / weights;
}

/// One gated (prev, cur) pair with its normalized assignment cost.
struct Candidate {
    size_t prev_index = 0;
    size_t cur_index = 0;
    double cost = 0.0;
};

/// Gate and cost every (prev, cur) pair in deterministic index order,
/// polling `context` every 64 outer stripes.
Result<std::vector<Candidate>> collect_candidates(const std::vector<RectF>& prev_bounds,
                                                  const std::vector<std::string>& prev_texts,
                                                  const std::vector<RectF>& cur_bounds,
                                                  const std::vector<std::string>& cur_texts,
                                                  const StableIdOptions& options, const ExecutionContext& context) {
    std::vector<Candidate> candidates;
    candidates.reserve(prev_bounds.size() * 2);
    for (size_t prev_index = 0; prev_index < prev_bounds.size(); ++prev_index) {
        if ((prev_index % 64) == 0) {
            if (const Status stage = context_status(context); !stage.ok()) {
                return stage;
            }
        }
        const double radius =
            options.center_gate_ratio * std::hypot(static_cast<double>(prev_bounds[prev_index].width),
                                                   static_cast<double>(prev_bounds[prev_index].height));
        for (size_t cur_index = 0; cur_index < cur_bounds.size(); ++cur_index) {
            const double iou = rect_iou(prev_bounds[prev_index], cur_bounds[cur_index]);
            const double displacement = center_distance(prev_bounds[prev_index], cur_bounds[cur_index]);
            if (iou < options.match_iou_threshold && displacement > radius) {
                continue;
            }
            const double proximity = radius > 0.0 ? 1.0 - displacement / radius : 0.0;
            const double signal = text_similarity(prev_texts[prev_index], cur_texts[cur_index]);
            const bool text_active = !prev_texts[prev_index].empty() && !cur_texts[cur_index].empty();
            candidates.push_back(
                Candidate{prev_index, cur_index, match_cost(iou, proximity, signal, text_active, options)});
        }
    }
    return candidates;
}

/// Gated greedy one-to-one assignment: ascending cost, ties by (prev, cur)
/// index. `match_of_cur` holds prev_count for unmatched; `prev_taken` marks
/// consumed previous indices.
void assign_greedy(std::vector<Candidate>& candidates, size_t prev_count, std::vector<size_t>& match_of_cur,
                   std::vector<bool>& prev_taken) noexcept {
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.cost != b.cost) {
            return a.cost < b.cost;
        }
        if (a.prev_index != b.prev_index) {
            return a.prev_index < b.prev_index;
        }
        return a.cur_index < b.cur_index;
    });
    for (const Candidate& candidate : candidates) {
        if (!prev_taken[candidate.prev_index] && match_of_cur[candidate.cur_index] == prev_count) {
            match_of_cur[candidate.cur_index] = candidate.prev_index;
            prev_taken[candidate.prev_index] = true;
        }
    }
}

/// Merge evidence: a fresh region that swallowed >= 2 unmatched previous
/// regions (centers inside its bounds) becomes kMerged.
void detect_merges(StableIdReport& report, const std::vector<RectF>& prev_bounds, const std::vector<bool>& prev_taken,
                   const std::vector<RectF>& cur_bounds) noexcept {
    for (size_t cur_index = 0; cur_index < cur_bounds.size(); ++cur_index) {
        if (report.assignments[cur_index].event != IdEvent::kNew) {
            continue;
        }
        size_t swallowed = 0;
        for (size_t prev_index = 0; prev_index < prev_bounds.size(); ++prev_index) {
            if (!prev_taken[prev_index] &&
                fusion_internal::center_inside(cur_bounds[cur_index], prev_bounds[prev_index])) {
                ++swallowed;
            }
        }
        if (swallowed >= 2) {
            report.assignments[cur_index].event = IdEvent::kMerged;
            ++report.merge_count;
        }
    }
}

/// True when region `cur_index` is a fresh region whose center lies inside
/// `prev_bounds`.
bool is_fresh_inside(const StableIdReport& report, size_t cur_index, const RectF& prev_bounds,
                     const std::vector<RectF>& cur_bounds) noexcept {
    return report.assignments[cur_index].event == IdEvent::kNew &&
           fusion_internal::center_inside(prev_bounds, cur_bounds[cur_index]);
}

/// Split evidence: an unmatched previous region hosting >= 2 fresh regions
/// (centers inside its bounds) marks them as split children.
void detect_splits(StableIdReport& report, const std::vector<RectF>& prev_bounds, const std::vector<bool>& prev_taken,
                   const std::vector<RectF>& cur_bounds) noexcept {
    for (size_t prev_index = 0; prev_index < prev_bounds.size(); ++prev_index) {
        if (prev_taken[prev_index]) {
            continue;
        }
        size_t children = 0;
        for (size_t cur_index = 0; cur_index < cur_bounds.size(); ++cur_index) {
            if (is_fresh_inside(report, cur_index, prev_bounds[prev_index], cur_bounds)) {
                ++children;
            }
        }
        if (children < 2) {
            continue;
        }
        for (size_t cur_index = 0; cur_index < cur_bounds.size(); ++cur_index) {
            if (is_fresh_inside(report, cur_index, prev_bounds[prev_index], cur_bounds)) {
                report.assignments[cur_index].event = IdEvent::kSplitChild;
            }
        }
        ++report.split_count;
    }
}

}  // namespace

Result<StableIdReport> StableIdTracker::advance(std::span<const VisualRegion> current_regions,
                                                const StableIdOptions& options,
                                                const ExecutionContext& context) noexcept {
    try {
        if (const Status stage = context_status(context); !stage.ok()) {
            return stage;
        }
        if (const Status valid = validate_options(options); !valid.ok()) {
            return valid;
        }
        if (current_regions.size() > static_cast<size_t>(options.max_regions)) {
            return Status(ErrorCode::kBudgetExceeded, "current region count exceeds max_regions");
        }

        // Compact plain-type snapshots of both sides; on error paths below
        // nothing is committed.
        std::vector<RectF> prev_bounds;
        std::vector<std::string> prev_texts;
        prev_bounds.reserve(previous_.size());
        prev_texts.reserve(previous_.size());
        for (const TrackedRegion& region : previous_) {
            prev_bounds.push_back(region.bounds);
            prev_texts.push_back(region.text);
        }
        std::vector<RectF> cur_bounds;
        std::vector<std::string> cur_texts;
        cur_bounds.reserve(current_regions.size());
        cur_texts.reserve(current_regions.size());
        for (const VisualRegion& region : current_regions) {
            cur_bounds.push_back(region.bounds);
            cur_texts.push_back(normalized_text(region.text));
        }

        const size_t prev_count = prev_bounds.size();
        const size_t cur_count = cur_bounds.size();

        Result<std::vector<Candidate>> candidates =
            collect_candidates(prev_bounds, prev_texts, cur_bounds, cur_texts, options, context);
        if (!candidates.ok()) {
            return candidates.status();
        }
        std::vector<Candidate> pairs = candidates.take_value();
        std::vector<size_t> match_of_cur(cur_count, prev_count);  // prev_count = "unmatched"
        std::vector<bool> prev_taken(prev_count, false);
        assign_greedy(pairs, prev_count, match_of_cur, prev_taken);

        // Baseline events: retained or fresh ids, allocated in cur order.
        StableIdReport report;
        report.assignments.resize(cur_count);
        for (size_t cur_index = 0; cur_index < cur_count; ++cur_index) {
            if (match_of_cur[cur_index] != prev_count) {
                report.assignments[cur_index] =
                    IdAssignment{previous_[match_of_cur[cur_index]].stable_id, IdEvent::kRetained};
                ++report.retained_count;
            } else {
                report.assignments[cur_index] = IdAssignment{next_id_, IdEvent::kNew};
                ++next_id_;
                ++report.new_count;
            }
        }
        detect_merges(report, prev_bounds, prev_taken, cur_bounds);
        detect_splits(report, prev_bounds, prev_taken, cur_bounds);

        // Generation decision (DEC-010 section 3): explicit split/merge, or
        // the retained fraction fell below the configured ratio.
        report.generation_bump =
            report.split_count > 0 || report.merge_count > 0 ||
            (prev_count > 0 && static_cast<double>(report.retained_count) <
                                   options.generation_retention_ratio * static_cast<double>(prev_count));

        // Commit: the tracked state becomes the assigned current snapshot.
        previous_.clear();
        previous_.reserve(cur_count);
        for (size_t cur_index = 0; cur_index < cur_count; ++cur_index) {
            previous_.push_back(TrackedRegion{report.assignments[cur_index].stable_id, cur_bounds[cur_index],
                                              std::move(cur_texts[cur_index])});
        }
        return report;
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "stable-id advance: internal allocation failed");
    }
}

void StableIdTracker::reset() noexcept {
    previous_.clear();
    next_id_ = 1;
}

}  // namespace mirador
