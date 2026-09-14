#ifndef MIRADOR_CACHE_POLICY_HPP
#define MIRADOR_CACHE_POLICY_HPP

#include <cstdint>

namespace mirador {

/// Caller intent for cached capability execution (design section 6 "缓存策略").
/// The policy is execution control, not part of the cache key (DEC-012): a
/// kRefresh request overwrites the entry a kReadWrite request would read.
enum class CachePolicy : uint8_t {
    kReadWrite,  ///< read the cached result when present; store fresh results
    kReadOnly,   ///< consult the cache but never store (read-only probes)
    kRefresh,    ///< bypass the cached value, execute, and overwrite the entry
};

}  // namespace mirador

#endif  // MIRADOR_CACHE_POLICY_HPP
