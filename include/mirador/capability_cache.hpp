#ifndef MIRADOR_CAPABILITY_CACHE_HPP
#define MIRADOR_CAPABILITY_CACHE_HPP

#include <mirador/detector_backend.hpp>
#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>
#include <mirador/transform.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mirador {

/// 128-bit, platform-stable digest of one capability-result cache key. Both
/// halves are FNV-1a over the same canonical serialization with distinct
/// domain separators, so equal fields always produce equal digests and any
/// field difference flips both halves with overwhelming probability. Not
/// cryptographic; collision resistance comes from the full 128 bits.
struct CacheKeyDigest {
    uint64_t high = 0;
    uint64_t low = 0;

    [[nodiscard]] friend bool operator==(const CacheKeyDigest& lhs, const CacheKeyDigest& rhs) noexcept {
        return lhs.high == rhs.high && lhs.low == rhs.low;
    }
    [[nodiscard]] friend bool operator<(const CacheKeyDigest& lhs, const CacheKeyDigest& rhs) noexcept {
        if (lhs.high != rhs.high) {
            return lhs.high < rhs.high;
        }
        return lhs.low < rhs.low;
    }
};

/// Which capability a cached entry belongs to; part of the key digest.
enum class CapabilityKind : uint8_t {
    kOcr,
    kDetection,
};

/// Every input the capability-result cache key must cover (design section 12,
/// RULE-07). `request_params_digest` carries the backend-relevant request
/// parameters (`ocr_request_params_digest` / `detection_request_params_digest`);
/// `cache_policy` is deliberately absent — refreshing overwrites the entry for
/// the same key instead of creating a new one (DEC-012).
struct CapabilityKeyFields {
    uint64_t image_fingerprint = 0;      ///< fingerprint of the executed region content (rotation-normalized)
    std::string source_id;               ///< image source the frame came from
    RectI roi;                           ///< executed pixel ROI in oriented space (whole view for full-frame requests)
    uint32_t preprocessing_version = 0;  ///< version of the crop/convert/resize pipeline
    CapabilityKind kind = CapabilityKind::kOcr;
    CoordinateSpaceId output_space = CoordinateSpaceId::kOriented;  ///< space of the stored regions
    std::string backend_name;
    std::string implementation_version;
    std::string model_id;
    std::string model_revision;
    uint64_t request_params_digest = 0;
};

/// Stable digest of the backend-relevant OCR request parameters (DEC-012):
/// `min_confidence`, `max_side`, `language_hint`, `backend_params`. Pipeline
/// fields (`roi`, spaces, `cache_policy`) are excluded; `roi` and
/// `output_space` enter the key through CapabilityKeyFields.
[[nodiscard]] uint64_t ocr_request_params_digest(const OcrRequest& request) noexcept;

/// Stable digest of the backend-relevant detection request parameters.
[[nodiscard]] uint64_t detection_request_params_digest(const DetectionRequest& request) noexcept;

/// Canonical key digest over all RULE-07 fields. Deterministic across
/// platforms (explicit little-endian serialization, no std::hash); allocation
/// free.
[[nodiscard]] CacheKeyDigest capability_cache_digest(const CapabilityKeyFields& fields) noexcept;

/// Recovered capability result stored by the perception pipeline: regions in
/// `output_space`, ready to use without further coordinate work on a hit.
/// Exactly one of the two vectors is meaningful, selected by `kind`; both may
/// be empty when the backend reported no regions.
struct CachedCapabilityResult {
    CapabilityKind kind = CapabilityKind::kOcr;
    CoordinateSpaceId output_space = CoordinateSpaceId::kOriented;
    std::vector<TextRegion> text_regions;
    std::vector<DetectionRegion> detection_regions;
};

/// Shared, immutable payload of one cache entry. A null pointer is a miss.
using CapabilityPayload = std::shared_ptr<const CachedCapabilityResult>;

/// Byte-budgeted LRU cache for capability results (design section 12, second
/// layer): one backend's output for a specific image fingerprint, ROI,
/// request parameters and model revision. Mirrors FrameCache semantics
/// (RULE-06): entries cost payload estimate plus a fixed
/// `kEntryOverheadBytes` allowance, `insert` evicts least-recently-used
/// entries and rejects (kBudgetExceeded, cache untouched) an entry that could
/// never fit, a too-large replacement keeps the old value, promotion happens
/// on `lookup` only. No locking, no threads: one session owns the cache.
/// Never throws.
class CapabilityResultCache {
public:
    /// Conservative per-entry byte overhead included in the budget.
    static constexpr int64_t kEntryOverheadBytes = 64;

    CapabilityResultCache() noexcept = default;
    CapabilityResultCache(const CapabilityResultCache&) = delete;
    CapabilityResultCache& operator=(const CapabilityResultCache&) = delete;
    CapabilityResultCache(CapabilityResultCache&&) noexcept = default;
    CapabilityResultCache& operator=(CapabilityResultCache&&) noexcept = default;
    ~CapabilityResultCache() noexcept = default;

    /// Creates a cache with the total budget `max_bytes`. Returns
    /// kInvalidArgument for non-positive budgets. Never throws.
    [[nodiscard]] static Result<CapabilityResultCache> create(int64_t max_bytes) noexcept;

    /// Inserts or replaces `key` with `value` as the most-recently-used entry.
    /// Returns kInvalidArgument when the payload mixes capability kinds
    /// (both region vectors non-empty, or regions outside `kind`), and
    /// kBudgetExceeded without any modification when the entry alone exceeds
    /// the total budget or on internal allocation failure. Never throws.
    [[nodiscard]] Result<void> insert(CacheKeyDigest key, const CachedCapabilityResult& value) noexcept;

    /// Promotes the entry and returns its shared payload. A miss is an ok
    /// Result carrying a null payload. Never throws.
    [[nodiscard]] Result<CapabilityPayload> lookup(CacheKeyDigest key) noexcept;

    /// True when `key` is present; does not change recency.
    [[nodiscard]] bool contains(CacheKeyDigest key) const noexcept;

    /// Removes `key` if present and reports whether it was.
    bool erase(CacheKeyDigest key) noexcept;

    /// Drops every entry.
    void clear() noexcept;

    [[nodiscard]] int64_t max_bytes() const noexcept { return max_bytes_; }
    /// Current budget usage: per-entry payload estimates plus entry overheads.
    [[nodiscard]] int64_t byte_size() const noexcept { return used_bytes_; }
    [[nodiscard]] size_t entry_count() const noexcept { return index_.size(); }

private:
    struct CacheNode {
        CacheKeyDigest key;
        int64_t bytes = 0;
        CapabilityPayload payload;
    };

    /// Deterministic budget estimate of one payload: header plus fixed cost
    /// per region plus text/polygon bytes. Documented in the header contract
    /// so callers can reason about budgets without measuring the heap.
    [[nodiscard]] static int64_t payload_bytes(const CachedCapabilityResult& value) noexcept;

    explicit CapabilityResultCache(int64_t max_bytes) noexcept : max_bytes_(max_bytes) {}

    std::list<CacheNode> entries_;  // front = most recently used
    std::map<CacheKeyDigest, std::list<CacheNode>::iterator> index_;
    int64_t max_bytes_ = 0;
    int64_t used_bytes_ = 0;
};

}  // namespace mirador

#endif  // MIRADOR_CAPABILITY_CACHE_HPP
