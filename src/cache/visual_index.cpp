#include <mirador/visual_index.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

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
    if (fingerprint.thumb_width != thumb_side_ || fingerprint.thumb_height != thumb_side_ ||
        fingerprint.thumbnail_gray.size() != static_cast<size_t>(thumb_side_) * thumb_side_) {
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
    if (fingerprint.thumb_width != thumb_side_ || fingerprint.thumb_height != thumb_side_ ||
        fingerprint.thumbnail_gray.size() != static_cast<size_t>(thumb_side_) * thumb_side_) {
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
    // A layer is active when its threshold is below 1.0; thresholds at 1.0
    // collapse the query to exact content matches (header contract).
    const bool perceptual_active = params.perceptual_similarity_threshold < 1.0;
    const bool template_active = params.template_ncc_threshold < 1.0;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const VisualPatchFingerprint& entry = it->fingerprint;
        VisualCandidate candidate;
        candidate.entry_id = it->entry_id;
        if (entry.content_hash == fingerprint.content_hash && entry.thumbnail_gray == fingerprint.thumbnail_gray) {
            candidate.evidence = VisualEvidenceKind::kExactContent;
            candidate.similarity = 1.0;
        } else {
            const uint64_t distance = static_cast<uint64_t>(std::popcount(entry.dhash ^ fingerprint.dhash));
            const double similarity = 1.0 - static_cast<double>(distance) / 64.0;
            const double correlation = thumbnail_ncc(fingerprint, entry);
            if (perceptual_active && similarity >= params.perceptual_similarity_threshold) {
                candidate.evidence = VisualEvidenceKind::kPerceptualHash;
                candidate.similarity = similarity;
            } else if (template_active && correlation >= params.template_ncc_threshold) {
                candidate.evidence = VisualEvidenceKind::kTemplate;
                candidate.similarity = correlation;
            } else {
                continue;  // no layer accepted this entry
            }
        }
        hits.push_back(candidate);
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
