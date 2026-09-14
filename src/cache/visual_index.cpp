#include <cstddef>
#include <cstdint>
#include <mirador/visual_index.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>
#include "mirador/result.hpp"
#include "mirador/status.hpp"
#include "mirador/visual_fingerprint.hpp"

namespace mirador {
namespace {

constexpr int64_t kEntryBytes(int32_t thumb_side) noexcept {
    return static_cast<int64_t>(thumb_side) * thumb_side + VisualIndex::kEntryOverheadBytes;
}

double thumbnail_ncc(const VisualPatchFingerprint& query, const VisualPatchFingerprint& entry) noexcept {
    const size_t count = query.thumbnail_gray.size();
    if (count == 0 || entry.thumbnail_gray.size() != count) {
        return 0.0;
    }
    double query_mean = 0.0;
    double entry_mean = 0.0;
    for (size_t i = 0; i < count; ++i) {
        query_mean += std::to_integer<uint8_t>(query.thumbnail_gray[i]);
        entry_mean += std::to_integer<uint8_t>(entry.thumbnail_gray[i]);
    }
    query_mean /= static_cast<double>(count);
    entry_mean /= static_cast<double>(count);
    double covariance = 0.0;
    double query_variance = 0.0;
    double entry_variance = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double q = std::to_integer<uint8_t>(query.thumbnail_gray[i]) - query_mean;
        const double e = std::to_integer<uint8_t>(entry.thumbnail_gray[i]) - entry_mean;
        covariance += q * e;
        query_variance += q * q;
        entry_variance += e * e;
    }
    const double denominator = std::sqrt(query_variance * entry_variance);
    if (denominator <= 0.0) {
        // One side is flat: identical flat thumbnails are a perfect match.
        return query.thumbnail_gray == entry.thumbnail_gray ? 1.0 : 0.0;
    }
    return covariance / denominator;
}

/// True when the fingerprint carries exactly this index's thumbnail geometry.
[[nodiscard]] bool geometry_matches(int32_t thumb_side, const VisualPatchFingerprint& fingerprint) noexcept {
    const auto side = static_cast<size_t>(thumb_side);
    return fingerprint.thumb_width == thumb_side && fingerprint.thumb_height == thumb_side &&
           fingerprint.thumbnail_gray.size() == side * side;
}

/// Strongest evidence of `entry` against the query fingerprint; nullopt when
/// no active layer accepts it.
[[nodiscard]] std::optional<VisualCandidate> match_entry(uint64_t entry_id, const VisualPatchFingerprint& entry,
                                                         const VisualPatchFingerprint& query,
                                                         const VisualQueryParams& params) noexcept {
    VisualCandidate candidate;
    candidate.entry_id = entry_id;
    if (entry.content_hash == query.content_hash && entry.thumbnail_gray == query.thumbnail_gray) {
        candidate.evidence = VisualEvidenceKind::kExactContent;
        candidate.similarity = 1.0;
        return candidate;
    }
    // A layer is active when its threshold is below 1.0; thresholds at 1.0
    // collapse the query to exact content matches (header contract).
    if (params.perceptual_similarity_threshold < 1.0) {
        const auto distance = static_cast<uint64_t>(std::popcount(entry.dhash ^ query.dhash));
        const double similarity = 1.0 - static_cast<double>(distance) / 64.0;
        if (similarity >= params.perceptual_similarity_threshold) {
            candidate.evidence = VisualEvidenceKind::kPerceptualHash;
            candidate.similarity = similarity;
            return candidate;
        }
    }
    if (params.template_ncc_threshold < 1.0) {
        const double correlation = thumbnail_ncc(query, entry);
        if (correlation >= params.template_ncc_threshold) {
            candidate.evidence = VisualEvidenceKind::kTemplate;
            candidate.similarity = correlation;
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace

VisualIndex::VisualIndex(int64_t max_bytes, int32_t thumb_side) noexcept
    : max_bytes_(max_bytes), thumb_side_(thumb_side) {}

Result<VisualIndex> VisualIndex::create(int64_t max_bytes, int32_t thumb_side) noexcept {
    if (max_bytes <= 0) {
        return Status(ErrorCode::kInvalidArgument, "budget must be positive");
    }
    if (thumb_side < 8 || thumb_side > 64) {
        return Status(ErrorCode::kInvalidArgument, "thumb_side must lie in [8, 64]");
    }
    return VisualIndex(max_bytes, thumb_side);
}

Result<void> VisualIndex::insert(uint64_t entry_id, const VisualPatchFingerprint& fingerprint) noexcept {
    if (!geometry_matches(thumb_side_, fingerprint)) {
        return Status(ErrorCode::kInvalidArgument, "fingerprint geometry does not match the index");
    }
    const int64_t entry_bytes = kEntryBytes(thumb_side_);
    if (entry_bytes > max_bytes_) {
        return Status(ErrorCode::kBudgetExceeded, "entry cannot fit the index budget");
    }
    erase(entry_id);  // replacement takes the newest slot
    while (used_bytes_ + entry_bytes > max_bytes_ && !entries_.empty()) {
        used_bytes_ -= kEntryBytes(thumb_side_);
        index_.erase(entries_.back().entry_id);
        entries_.pop_back();
    }
    entries_.push_front(Entry{entry_id, fingerprint});
    index_.emplace(entry_id, entries_.begin());
    used_bytes_ += entry_bytes;
    return Status::success();
}

bool VisualIndex::erase(uint64_t entry_id) noexcept {
    const auto found = index_.find(entry_id);
    if (found == index_.end()) {
        return false;
    }
    entries_.erase(found->second);
    index_.erase(found);
    used_bytes_ -= kEntryBytes(thumb_side_);
    return true;
}

bool VisualIndex::contains(uint64_t entry_id) const noexcept {
    return index_.find(entry_id) != index_.end();
}

Result<std::vector<VisualCandidate>> VisualIndex::query(const VisualPatchFingerprint& fingerprint,
                                                        const VisualQueryParams& params) noexcept {
    if (!geometry_matches(thumb_side_, fingerprint)) {
        return Status(ErrorCode::kInvalidArgument, "fingerprint geometry does not match the index");
    }
    if (!(params.perceptual_similarity_threshold >= 0.0) || params.perceptual_similarity_threshold > 1.0 ||
        !std::isfinite(params.perceptual_similarity_threshold) || !(params.template_ncc_threshold >= 0.0) ||
        params.template_ncc_threshold > 1.0 || !std::isfinite(params.template_ncc_threshold)) {
        return Status(ErrorCode::kInvalidArgument, "thresholds must lie in [0, 1]");
    }
    if (params.max_candidates < 1 || params.max_candidates > 1024) {
        return Status(ErrorCode::kInvalidArgument, "max_candidates must lie in [1, 1024]");
    }

    std::vector<VisualCandidate> hits;
    for (auto& entrie : entries_) {
        if (auto candidate = match_entry(entrie.entry_id, entrie.fingerprint, fingerprint, params)) {
            hits.push_back(*candidate);
        }
    }

    std::sort(hits.begin(), hits.end(), [](const VisualCandidate& a, const VisualCandidate& b) {
        if (a.evidence != b.evidence) {
            return a.evidence < b.evidence;
        }
        if (a.similarity != b.similarity) {
            return a.similarity > b.similarity;
        }
        return a.entry_id < b.entry_id;
    });
    if (hits.size() > static_cast<size_t>(params.max_candidates)) {
        hits.resize(static_cast<size_t>(params.max_candidates));
    }
    // Promote the surviving hits (LRU bookkeeping).
    for (const VisualCandidate& hit : hits) {
        const auto found = index_.find(hit.entry_id);
        if (found != index_.end() && found->second != entries_.begin()) {
            entries_.splice(entries_.begin(), entries_, found->second);
            found->second = entries_.begin();
        }
    }
    return hits;
}

}  // namespace mirador
