#include <mirador/capability_cache.hpp>

#include <mirador/detector_backend.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace mirador {
namespace {

// FNV-1a 64-bit over a canonical little-endian serialization. Two independent
// hashes with distinct domain separators form the 128-bit key digest; the
// serialization never materializes (fields feed the hash directly), keeping
// the digest allocation free.
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x00000100000001b3ULL;

class FieldHasher {
public:
    explicit FieldHasher(const char* domain) noexcept : state_(kFnvOffsetBasis) {
        while (*domain != '\0') {
            mix_byte(static_cast<uint8_t>(*domain++));
        }
        mix_byte(0);  // domain separator terminator, keeps domains unambiguous
    }

    void u64(uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            mix_byte(static_cast<uint8_t>(value >> (8 * i)));
        }
    }
    void u32(uint32_t value) noexcept {
        for (int i = 0; i < 4; ++i) {
            mix_byte(static_cast<uint8_t>(value >> (8 * i)));
        }
    }
    void i32(int32_t value) noexcept { u32(static_cast<uint32_t>(value)); }
    void f32(float value) noexcept {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "32-bit IEEE 754 float expected");
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void byte(uint8_t value) noexcept { mix_byte(value); }
    void str(const std::string& value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (const char c : value) {
            mix_byte(static_cast<uint8_t>(c));
        }
    }

    [[nodiscard]] uint64_t state() const noexcept { return state_; }

private:
    void mix_byte(uint8_t value) noexcept {
        state_ ^= value;
        state_ *= kFnvPrime;
    }

    uint64_t state_;
};

constexpr const char* kParamsDomain = "mirador-params-a-v1";
constexpr const char* kKeyDomainA = "mirador-capkey-a-v1";
constexpr const char* kKeyDomainB = "mirador-capkey-b-v1";

void feed_common_request_fields(FieldHasher& hasher, float min_confidence, int32_t max_side,
                                const std::string& backend_params) noexcept {
    hasher.f32(min_confidence);
    hasher.i32(max_side);
    hasher.str(backend_params);
}

void feed_key_fields(FieldHasher& hasher, const CapabilityKeyFields& fields) noexcept {
    hasher.u64(fields.image_fingerprint);
    hasher.str(fields.source_id);
    hasher.i32(fields.roi.x);
    hasher.i32(fields.roi.y);
    hasher.i32(fields.roi.width);
    hasher.i32(fields.roi.height);
    hasher.u32(fields.preprocessing_version);
    hasher.byte(static_cast<uint8_t>(fields.kind));
    hasher.u32(static_cast<uint32_t>(fields.output_space));
    hasher.str(fields.backend_name);
    hasher.str(fields.implementation_version);
    hasher.str(fields.model_id);
    hasher.str(fields.model_revision);
    hasher.u64(fields.request_params_digest);
}

template <typename Feed>
uint64_t single_hash(const char* domain, Feed&& feed) noexcept {
    FieldHasher hasher(domain);
    feed(hasher);
    return hasher.state();
}

template <typename Feed>
CacheKeyDigest hash_twice(const char* domain_a, const char* domain_b, Feed&& feed) noexcept {
    FieldHasher hasher_a(domain_a);
    FieldHasher hasher_b(domain_b);
    feed(hasher_a);
    feed(hasher_b);
    CacheKeyDigest digest;
    digest.high = hasher_a.state();
    digest.low = hasher_b.state();
    return digest;
}

}  // namespace

uint64_t ocr_request_params_digest(const OcrRequest& request) noexcept {
    return single_hash(kParamsDomain, [&request](FieldHasher& hasher) noexcept {
        hasher.byte(1);  // kind discriminator
        feed_common_request_fields(hasher, request.min_confidence, request.max_side, request.backend_params);
        hasher.str(request.language_hint);
    });
}

uint64_t detection_request_params_digest(const DetectionRequest& request) noexcept {
    return single_hash(kParamsDomain, [&request](FieldHasher& hasher) noexcept {
        hasher.byte(2);  // kind discriminator
        feed_common_request_fields(hasher, request.min_confidence, request.max_side, request.backend_params);
    });
}

CacheKeyDigest capability_cache_digest(const CapabilityKeyFields& fields) noexcept {
    return hash_twice(kKeyDomainA, kKeyDomainB,
                      [&fields](FieldHasher& hasher) noexcept { feed_key_fields(hasher, fields); });
}

Result<CapabilityResultCache> CapabilityResultCache::create(int64_t max_bytes) noexcept {
    if (max_bytes <= 0) {
        return Status(ErrorCode::kInvalidArgument, "cache budget must be positive");
    }
    return CapabilityResultCache(max_bytes);
}

int64_t CapabilityResultCache::payload_bytes(const CachedCapabilityResult& value) noexcept {
    int64_t bytes = 32;  // kind + output space + container bookkeeping
    for (const TextRegion& region : value.text_regions) {
        bytes += 64;  // fixed per-region cost
        bytes += static_cast<int64_t>(region.utf8_text.size());
        bytes += 8 * static_cast<int64_t>(region.polygon.size());
    }
    for (const DetectionRegion& region : value.detection_regions) {
        // Fixed per-region cost (bounds, class id, confidence) plus label bytes.
        bytes += 48 + static_cast<int64_t>(region.label.size());
    }
    return bytes;
}

Result<void> CapabilityResultCache::insert(CacheKeyDigest key, const CachedCapabilityResult& value) noexcept {
    const bool has_text = !value.text_regions.empty();
    const bool has_detections = !value.detection_regions.empty();
    if (has_text && has_detections) {
        return Status(ErrorCode::kInvalidArgument, "cached result mixes ocr and detection regions");
    }
    if (value.kind == CapabilityKind::kOcr && has_detections) {
        return Status(ErrorCode::kInvalidArgument, "detection regions stored under kOcr kind");
    }
    if (value.kind == CapabilityKind::kDetection && has_text) {
        return Status(ErrorCode::kInvalidArgument, "ocr regions stored under kDetection kind");
    }

    CapabilityPayload payload;
    try {
        payload = std::make_shared<const CachedCapabilityResult>(value);
    } catch (...) {
        return Status(ErrorCode::kBudgetExceeded, "capability cache payload allocation failed");
    }

    const int64_t entry_bytes = kEntryOverheadBytes + payload_bytes(value);
    if (entry_bytes > max_bytes_) {
        return Status(ErrorCode::kBudgetExceeded, "capability cache entry alone exceeds budget");
    }

    // Replacement path: drop any old value for this key first (RULE-06: the
    // cache state stays consistent even when the caller ignores the result).
    erase(key);
    try {
        while (used_bytes_ + entry_bytes > max_bytes_ && !entries_.empty()) {
            used_bytes_ -= entries_.back().bytes;
            index_.erase(entries_.back().key);
            entries_.pop_back();
        }
        entries_.push_front(CacheNode{key, entry_bytes, std::move(payload)});
        index_[key] = entries_.begin();
        used_bytes_ += entry_bytes;
    } catch (...) {
        return Status(ErrorCode::kBudgetExceeded, "capability cache insertion failed");
    }
    return Status::success();
}

Result<CapabilityPayload> CapabilityResultCache::lookup(CacheKeyDigest key) noexcept {
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return CapabilityPayload{};  // miss: ok Result with null payload
    }
    entries_.splice(entries_.begin(), entries_, it->second);  // promote to MRU
    it->second = entries_.begin();
    return it->second->payload;
}

bool CapabilityResultCache::contains(CacheKeyDigest key) const noexcept {
    return index_.find(key) != index_.end();
}

bool CapabilityResultCache::erase(CacheKeyDigest key) noexcept {
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    used_bytes_ -= it->second->bytes;
    entries_.erase(it->second);
    index_.erase(it);
    return true;
}

void CapabilityResultCache::clear() noexcept {
    entries_.clear();
    index_.clear();
    used_bytes_ = 0;
}

}  // namespace mirador
