#ifndef MIRADOR_VISUAL_INDEX_HPP
#define MIRADOR_VISUAL_INDEX_HPP

#include <mirador/result.hpp>
#include <mirador/visual_fingerprint.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace mirador {

/// Which evidence layer produced a candidate (design section 12, DEC-014):
/// the exact layer dominates, then the perceptual hash, then template
/// matching. Higher layers never mask lower ones — each entry reports its
/// strongest evidence.
enum class VisualEvidenceKind : uint8_t {
    kExactContent,    ///< content hash equal and thumbnail bytes identical
    kPerceptualHash,  ///< 1 - hamming(dHash)/64 >= perceptual threshold
    kTemplate,        ///< thumbnail NCC >= template threshold
};

/// One query hit. `similarity` is layer-dependent: exactly 1.0 for
/// kExactContent, the hash similarity in [0, 1] for kPerceptualHash and the
/// NCC in [-1, 1] for kTemplate. A candidate is evidence of visual
/// closeness, never a claim of semantic identity (design section 12).
struct VisualCandidate {
    uint64_t entry_id = 0;
    double similarity = 0.0;
    VisualEvidenceKind evidence = VisualEvidenceKind::kTemplate;

    /// Component equality (test convenience).
    [[nodiscard]] friend bool operator==(const VisualCandidate& lhs, const VisualCandidate& rhs) noexcept {
        return lhs.entry_id == rhs.entry_id && lhs.similarity == rhs.similarity && lhs.evidence == rhs.evidence;
    }
};

/// Query thresholds. A layer is active when its threshold is below 1.0; both
/// thresholds at 1.0 reduce the query to exact matches.
struct VisualQueryParams {
    /// Minimum hash similarity for the perceptual layer, in [0, 1].
    double perceptual_similarity_threshold = 0.9;
    /// Minimum NCC for the template layer, in [0, 1].
    double template_ncc_threshold = 0.9;
    /// Maximum number of returned candidates, in [1, 1024].
    int32_t max_candidates = 8;
};

/// Bounded visual index over caller-extracted `VisualPatchFingerprint`s
/// (design section 12, M3-10): per-entry content hash, dHash and normalized
/// gray thumbnail, matched layer by layer. Byte-budgeted like every Mirador
/// cache (RULE-06): an entry costs `thumb_side^2 + kEntryOverheadBytes`,
/// insertion evicts least-recently-inserted entries, and an entry that could
/// never fit fails with kBudgetExceeded leaving the index untouched. Queries
/// promote the entries they touch (LRU). No locking, no threads (AGENTS.md).
/// Never throws.
class VisualIndex {
public:
    /// Conservative per-entry byte overhead included in the budget.
    static constexpr int64_t kEntryOverheadBytes = 64;

    VisualIndex() noexcept = default;
    VisualIndex(const VisualIndex&) = delete;
    VisualIndex& operator=(const VisualIndex&) = delete;
    VisualIndex(VisualIndex&&) noexcept = default;
    VisualIndex& operator=(VisualIndex&&) noexcept = default;
    ~VisualIndex() noexcept = default;

    /// Creates an index for fingerprints with square thumbnails of
    /// `thumb_side` pixels per side (in [8, 64], matching
    /// PatchFingerprintParams). Returns kInvalidArgument for non-positive
    /// budgets or an out-of-range side. Never throws.
    [[nodiscard]] static Result<VisualIndex> create(int64_t max_bytes, int32_t thumb_side) noexcept;

    /// Inserts or replaces `entry_id` with a copy of `fingerprint` as the
    /// most-recently-inserted entry. The fingerprint must carry exactly this
    /// index's thumbnail geometry. Errors: kInvalidArgument (geometry
    /// mismatch), kBudgetExceeded without any modification when the entry
    /// alone exceeds the budget. Never throws.
    [[nodiscard]] Result<void> insert(uint64_t entry_id, const VisualPatchFingerprint& fingerprint) noexcept;

    /// Removes `entry_id` if present; true when it was.
    bool erase(uint64_t entry_id) noexcept;

    /// True when `entry_id` is present; does not change recency.
    [[nodiscard]] bool contains(uint64_t entry_id) const noexcept;

    /// Layered query over all entries. Every entry reports its strongest
    /// evidence (kExactContent beats kPerceptualHash beats kTemplate);
    /// output is ordered by evidence layer, then descending similarity, then
    /// ascending entry id, capped at `params.max_candidates`. Matching hits
    /// are promoted for eviction purposes. Errors: kInvalidArgument for a
    /// fingerprint with foreign geometry, invalid thresholds, or a
    /// non-positive max_candidates. Never throws.
    [[nodiscard]] Result<std::vector<VisualCandidate>> query(const VisualPatchFingerprint& fingerprint,
                                                             const VisualQueryParams& params) noexcept;

    [[nodiscard]] int64_t max_bytes() const noexcept { return max_bytes_; }
    [[nodiscard]] int64_t byte_size() const noexcept { return used_bytes_; }
    [[nodiscard]] size_t entry_count() const noexcept { return entries_.size(); }
    [[nodiscard]] int32_t thumb_side() const noexcept { return thumb_side_; }

private:
    struct Entry {
        uint64_t entry_id = 0;
        VisualPatchFingerprint fingerprint;
    };

    explicit VisualIndex(int64_t max_bytes, int32_t thumb_side) noexcept;

    /// Normalized cross-correlation of two same-size gray thumbnails.
    [[nodiscard]] double ncc(const VisualPatchFingerprint& query, const Entry& entry) const noexcept;

    std::list<Entry> entries_;  // front = most recently inserted/used
    std::unordered_map<uint64_t, std::list<Entry>::iterator> index_;
    int64_t max_bytes_ = 0;
    int64_t used_bytes_ = 0;
    int32_t thumb_side_ = 0;
};

}  // namespace mirador

#endif  // MIRADOR_VISUAL_INDEX_HPP
