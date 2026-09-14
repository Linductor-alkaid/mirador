#ifndef MIRADOR_FRAME_CACHE_HPP
#define MIRADOR_FRAME_CACHE_HPP

#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace mirador {

/// Shared, immutable payload of one frame-cache entry. A null pointer is a cache
/// miss; a non-null pointer to an empty vector is a stored empty value.
using FramePayload = std::shared_ptr<const std::vector<std::byte>>;

/// Byte-budgeted LRU cache for frame-level artifacts: fingerprints, thumbnails
/// and change-detection intermediates (design section 12 frame-cache layer).
/// Keys are caller-owned 64-bit identifiers (a source id and an artifact kind,
/// for example); values are immutable byte payloads that callers interpret.
///
/// Budget accounting (RULE-06): every entry costs its payload bytes plus a fixed
/// `kEntryOverheadBytes` allowance for the key, the container nodes and allocator
/// slack, so the entry count is bounded by `max_bytes / kEntryOverheadBytes` even
/// for empty payloads. `insert` never grows the cache beyond `max_bytes`: it
/// evicts least-recently-used entries first and rejects (with kBudgetExceeded,
/// leaving the cache untouched) an entry that could never fit. Promotion happens
/// on `lookup` only; `contains` peeks without reordering. The cache performs no
/// locking and no internal threads: it belongs to one session, and concurrent
/// use is the caller's concern (AGENTS.md, sync API boundary). Never throws.
class FrameCache {
public:
    /// Conservative per-entry byte overhead included in the budget.
    static constexpr int64_t kEntryOverheadBytes = 64;

    /// Empty cache; every accessor behaves as a miss and `insert` fails against
    /// a zero budget.
    FrameCache() noexcept = default;
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;
    FrameCache(FrameCache&&) noexcept = default;
    FrameCache& operator=(FrameCache&&) noexcept = default;
    ~FrameCache() noexcept = default;

    /// Creates a cache with the total budget `max_bytes`. Returns kInvalidArgument
    /// for non-positive budgets. Never throws.
    [[nodiscard]] static Result<FrameCache> create(int64_t max_bytes) noexcept;

    /// Inserts or replaces `key` with a copy of `value` as the most-recently-used
    /// entry, evicting least-recently-used entries while the budget is exceeded.
    /// Returns kBudgetExceeded without any modification when the entry alone
    /// exceeds the total budget; a too-large replacement keeps the old value.
    /// Returns kBudgetExceeded on internal allocation failure. Never throws.
    [[nodiscard]] Result<void> insert(uint64_t key, std::span<const std::byte> value) noexcept;

    /// Promotes the entry and returns its shared payload. A miss is reported as
    /// an ok Result carrying a null payload. Never throws.
    [[nodiscard]] Result<FramePayload> lookup(uint64_t key) noexcept;

    /// True when `key` is present; does not change recency.
    [[nodiscard]] bool contains(uint64_t key) const noexcept;

    /// Removes `key` if present and reports whether it was.
    bool erase(uint64_t key) noexcept;

    /// Drops every entry.
    void clear() noexcept;

    [[nodiscard]] int64_t max_bytes() const noexcept { return max_bytes_; }
    /// Current budget usage: payload bytes plus the per-entry overhead of every
    /// entry.
    [[nodiscard]] int64_t byte_size() const noexcept { return used_bytes_; }
    [[nodiscard]] size_t entry_count() const noexcept { return index_.size(); }

private:
    struct CacheNode {
        uint64_t key = 0;
        int64_t bytes = 0;
        FramePayload payload;
    };

    explicit FrameCache(int64_t max_bytes) noexcept : max_bytes_(max_bytes) {}

    std::list<CacheNode> entries_;  // front = most recently used
    std::unordered_map<uint64_t, std::list<CacheNode>::iterator> index_;
    int64_t max_bytes_ = 0;
    int64_t used_bytes_ = 0;
};

}  // namespace mirador

#endif  // MIRADOR_FRAME_CACHE_HPP
