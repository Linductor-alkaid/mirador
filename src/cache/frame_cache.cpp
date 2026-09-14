#include <mirador/frame_cache.hpp>

#include <mirador/status.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mirador/result.hpp>
#include <span>
#include <utility>
#include <vector>

namespace mirador {

Result<FrameCache> FrameCache::create(const int64_t max_bytes) noexcept {
    if (max_bytes <= 0) {
        return Status(ErrorCode::kInvalidArgument, "FrameCache::create: max_bytes must be positive");
    }
    return FrameCache(max_bytes);
}

Result<void> FrameCache::insert(const uint64_t key, const std::span<const std::byte> value) noexcept {
    const int64_t entry_bytes = static_cast<int64_t>(value.size()) + kEntryOverheadBytes;
    if (entry_bytes > max_bytes_) {
        return Status(ErrorCode::kBudgetExceeded, "FrameCache::insert: entry exceeds the total cache budget");
    }
    try {
        // C++20: make_shared is allowed to construct cv-qualified payload vectors.
        auto payload = std::make_shared<const std::vector<std::byte>>(value.begin(), value.end());
        if (const auto existing = index_.find(key); existing != index_.end()) {
            used_bytes_ -= existing->second->bytes;
            entries_.erase(existing->second);
            index_.erase(existing);
        }
        while (!entries_.empty() && used_bytes_ + entry_bytes > max_bytes_) {
            used_bytes_ -= entries_.back().bytes;
            index_.erase(entries_.back().key);
            entries_.pop_back();
        }
        entries_.push_front(CacheNode{key, entry_bytes, std::move(payload)});
        index_[key] = entries_.begin();
        used_bytes_ += entry_bytes;
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "FrameCache::insert: allocation failed");
    }
    return {};
}

Result<FramePayload> FrameCache::lookup(const uint64_t key) noexcept {
    const auto entry = index_.find(key);
    if (entry == index_.end()) {
        return {nullptr};
    }
    entries_.splice(entries_.begin(), entries_, entry->second);  // promote to MRU
    return entry->second->payload;
}

bool FrameCache::contains(const uint64_t key) const noexcept {
    return index_.find(key) != index_.end();
}

bool FrameCache::erase(const uint64_t key) noexcept {
    const auto entry = index_.find(key);
    if (entry == index_.end()) {
        return false;
    }
    used_bytes_ -= entry->second->bytes;
    entries_.erase(entry->second);
    index_.erase(entry);
    return true;
}

void FrameCache::clear() noexcept {
    entries_.clear();
    index_.clear();
    used_bytes_ = 0;
}

}  // namespace mirador
