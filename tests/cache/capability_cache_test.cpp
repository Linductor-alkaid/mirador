// Unit tests for the M2 capability-result cache (M2-02): RULE-07 key digest
// stability and per-field sensitivity, byte-budgeted LRU semantics, refresh
// and replacement invalidation paths.

#include <mirador/cache_policy.hpp>
#include <mirador/capability_cache.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

using mirador::CachedCapabilityResult;
using mirador::CacheKeyDigest;
using mirador::capability_cache_digest;
using mirador::CapabilityKeyFields;
using mirador::CapabilityKind;
using mirador::CapabilityPayload;
using mirador::CapabilityResultCache;
using mirador::CoordinateSpaceId;
using mirador::detection_request_params_digest;
using mirador::DetectionRegion;
using mirador::DetectionRequest;
using mirador::ErrorCode;
using mirador::ocr_request_params_digest;
using mirador::OcrRequest;
using mirador::RectF;
using mirador::RectI;
using mirador::TextRegion;

CapabilityKeyFields base_fields() {
    CapabilityKeyFields fields;
    fields.image_fingerprint = 0x1234567890abcdefULL;
    fields.source_id = "screen-main";
    fields.roi = RectI{10, 20, 300, 200};
    fields.preprocessing_version = 1;
    fields.kind = CapabilityKind::kOcr;
    fields.output_space = CoordinateSpaceId::kOriented;
    fields.backend_name = "fake-ocr";
    fields.implementation_version = "1.0.0";
    fields.model_id = "fake-model";
    fields.model_revision = "r7";
    fields.request_params_digest = 42;
    return fields;
}

CachedCapabilityResult text_result(size_t region_count) {
    CachedCapabilityResult result;
    result.kind = CapabilityKind::kOcr;
    result.output_space = CoordinateSpaceId::kOriented;
    for (size_t i = 0; i < region_count; ++i) {
        TextRegion region;
        region.bounds = RectF{static_cast<float>(i), 1.0F, 10.0F, 5.0F};
        region.utf8_text = "text";
        region.confidence = 0.9F;
        region.polygon = {{0.0F, 0.0F}, {1.0F, 0.0F}};
        result.text_regions.push_back(std::move(region));
    }
    return result;
}

// --- Key digest ---------------------------------------------------------------

TEST(CapabilityCacheKeyTest, SameFieldsProduceSameDigest) {
    const CacheKeyDigest first = capability_cache_digest(base_fields());
    const CacheKeyDigest second = capability_cache_digest(base_fields());
    EXPECT_EQ(first, second);
}

TEST(CapabilityCacheKeyTest, EveryKeyFieldChangesTheDigest) {
    const CacheKeyDigest base = capability_cache_digest(base_fields());

    const auto expect_different = [base](const CapabilityKeyFields& mutated, const char* what) {
        const CacheKeyDigest digest = capability_cache_digest(mutated);
        EXPECT_NE(digest, base) << "field not covered by the key: " << what;
        EXPECT_NE(digest.high, base.high) << "high half not flipped by: " << what;
        EXPECT_NE(digest.low, base.low) << "low half not flipped by: " << what;
    };

    {
        CapabilityKeyFields fields = base_fields();
        fields.image_fingerprint += 1;
        expect_different(fields, "image_fingerprint");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.source_id = "screen-other";
        expect_different(fields, "source_id");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.roi.x += 1;
        expect_different(fields, "roi.x");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.roi.y += 1;
        expect_different(fields, "roi.y");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.roi.width += 1;
        expect_different(fields, "roi.width");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.roi.height += 1;
        expect_different(fields, "roi.height");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.preprocessing_version += 1;
        expect_different(fields, "preprocessing_version");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.kind = CapabilityKind::kDetection;
        expect_different(fields, "kind");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.output_space = CoordinateSpaceId::kFrame;
        expect_different(fields, "output_space");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.backend_name = "other-ocr";
        expect_different(fields, "backend_name");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.implementation_version = "1.0.1";
        expect_different(fields, "implementation_version");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.model_id = "other-model";
        expect_different(fields, "model_id");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.model_revision = "r8";
        expect_different(fields, "model_revision");
    }
    {
        CapabilityKeyFields fields = base_fields();
        fields.request_params_digest += 1;
        expect_different(fields, "request_params_digest");
    }
}

TEST(CapabilityCacheKeyTest, OcrParamsDigestCoversBackendRelevantFieldsOnly) {
    OcrRequest base;
    base.min_confidence = 0.5F;
    base.max_side = 1024;
    base.language_hint = "zh-Hans";
    base.backend_params = "{}";
    const uint64_t base_digest = ocr_request_params_digest(base);

    // Pipeline-owned fields must NOT change the params digest (DEC-012).
    {
        OcrRequest request = base;
        request.roi = RectF{1.0F, 1.0F, 5.0F, 5.0F};
        EXPECT_EQ(ocr_request_params_digest(request), base_digest);
    }
    {
        OcrRequest request = base;
        request.output_space = CoordinateSpaceId::kFrame;
        EXPECT_EQ(ocr_request_params_digest(request), base_digest);
    }
    {
        OcrRequest request = base;
        request.cache_policy = mirador::CachePolicy::kRefresh;
        EXPECT_EQ(ocr_request_params_digest(request), base_digest);
    }

    // Backend-owned fields must change it.
    OcrRequest request = base;
    request.min_confidence = 0.6F;
    EXPECT_NE(ocr_request_params_digest(request), base_digest);
    request = base;
    request.max_side = 512;
    EXPECT_NE(ocr_request_params_digest(request), base_digest);
    request = base;
    request.language_hint = "en";
    EXPECT_NE(ocr_request_params_digest(request), base_digest);
    request = base;
    request.backend_params = "{\"thr\":0.1}";
    EXPECT_NE(ocr_request_params_digest(request), base_digest);
}

TEST(CapabilityCacheKeyTest, DetectionParamsDigestIsKindSeparated) {
    DetectionRequest detection;
    detection.min_confidence = 0.5F;
    detection.max_side = 1024;
    detection.backend_params = "{}";

    OcrRequest ocr;
    ocr.min_confidence = 0.5F;
    ocr.max_side = 1024;
    ocr.backend_params = "{}";
    ocr.language_hint.clear();

    EXPECT_NE(detection_request_params_digest(detection), ocr_request_params_digest(ocr));

    DetectionRequest changed = detection;
    changed.min_confidence = 0.9F;
    EXPECT_NE(detection_request_params_digest(changed), detection_request_params_digest(detection));
}

// --- Cache container ------------------------------------------------------------

TEST(CapabilityResultCacheTest, CreateRejectsNonPositiveBudgets) {
    EXPECT_EQ(CapabilityResultCache::create(0).status().code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(CapabilityResultCache::create(-1).status().code(), ErrorCode::kInvalidArgument);
}

TEST(CapabilityResultCacheTest, MissCarriesNullPayload) {
    CapabilityResultCache cache = CapabilityResultCache::create(1024).take_value();
    const CapabilityPayload miss = cache.lookup(capability_cache_digest(base_fields())).value();
    EXPECT_EQ(miss, nullptr);
}

TEST(CapabilityResultCacheTest, InsertThenLookupRoundTrip) {
    CapabilityResultCache cache = CapabilityResultCache::create(4096).take_value();
    const CacheKeyDigest key = capability_cache_digest(base_fields());
    const CachedCapabilityResult value = text_result(2);

    ASSERT_TRUE(cache.insert(key, value).ok());
    EXPECT_TRUE(cache.contains(key));
    EXPECT_EQ(cache.entry_count(), 1U);

    const CapabilityPayload hit = cache.lookup(key).value();
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->kind, CapabilityKind::kOcr);
    EXPECT_EQ(hit->output_space, CoordinateSpaceId::kOriented);
    ASSERT_EQ(hit->text_regions.size(), 2U);
    EXPECT_EQ(hit->text_regions[0].utf8_text, "text");
    EXPECT_EQ(hit->text_regions[1].bounds.x, 1.0F);
}

TEST(CapabilityResultCacheTest, ReplacementReplacesValueAndAccounting) {
    CapabilityResultCache cache = CapabilityResultCache::create(4096).take_value();
    const CacheKeyDigest key = capability_cache_digest(base_fields());

    ASSERT_TRUE(cache.insert(key, text_result(1)).ok());
    const int64_t one_region_bytes = cache.byte_size();
    ASSERT_TRUE(cache.insert(key, text_result(3)).ok());
    EXPECT_EQ(cache.entry_count(), 1U);
    EXPECT_GT(cache.byte_size(), one_region_bytes);

    const CapabilityPayload hit = cache.lookup(key).value();
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->text_regions.size(), 3U);
}

TEST(CapabilityResultCacheTest, LeastRecentlyUsedEntryIsEvictedFirst) {
    CapabilityResultCache cache = CapabilityResultCache::create(512).take_value();
    CapabilityKeyFields fields_a = base_fields();
    fields_a.model_revision = "a";
    CapabilityKeyFields fields_b = base_fields();
    fields_b.model_revision = "b";
    CapabilityKeyFields fields_c = base_fields();
    fields_c.model_revision = "c";
    const CacheKeyDigest key_a = capability_cache_digest(fields_a);
    const CacheKeyDigest key_b = capability_cache_digest(fields_b);
    const CacheKeyDigest key_c = capability_cache_digest(fields_c);

    ASSERT_TRUE(cache.insert(key_a, text_result(1)).ok());
    ASSERT_TRUE(cache.insert(key_b, text_result(1)).ok());
    // Promote A so B becomes the LRU victim.
    ASSERT_NE(cache.lookup(key_a).value(), nullptr);

    ASSERT_TRUE(cache.insert(key_c, text_result(1)).ok());
    EXPECT_FALSE(cache.contains(key_b));
    EXPECT_TRUE(cache.contains(key_a));
    EXPECT_TRUE(cache.contains(key_c));
}

TEST(CapabilityResultCacheTest, OversizedEntryIsRejectedWithoutStateChange) {
    CapabilityResultCache cache = CapabilityResultCache::create(64).take_value();
    const CacheKeyDigest key = capability_cache_digest(base_fields());
    // 5 text regions cost far more than the 64-byte budget.
    const mirador::Result<void> insert = cache.insert(key, text_result(5));
    EXPECT_EQ(insert.status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_FALSE(cache.contains(key));
    EXPECT_EQ(cache.byte_size(), 0);
    EXPECT_EQ(cache.entry_count(), 0U);
}

TEST(CapabilityResultCacheTest, OversizedReplacementKeepsOldValue) {
    CapabilityResultCache cache = CapabilityResultCache::create(512).take_value();
    const CacheKeyDigest key = capability_cache_digest(base_fields());
    ASSERT_TRUE(cache.insert(key, text_result(1)).ok());
    const CapabilityPayload before = cache.lookup(key).value();
    ASSERT_NE(before, nullptr);

    const mirador::Result<void> insert = cache.insert(key, text_result(50));
    EXPECT_EQ(insert.status().code(), ErrorCode::kBudgetExceeded);

    const CapabilityPayload after = cache.lookup(key).value();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->text_regions.size(), before->text_regions.size());
}

TEST(CapabilityResultCacheTest, MixedKindPayloadsAreRejected) {
    CapabilityResultCache cache = CapabilityResultCache::create(4096).take_value();
    CachedCapabilityResult mixed;
    mixed.kind = CapabilityKind::kOcr;
    mixed.text_regions.push_back(TextRegion{});
    mixed.detection_regions.push_back(DetectionRegion{});
    EXPECT_EQ(cache.insert(capability_cache_digest(base_fields()), mixed).status().code(), ErrorCode::kInvalidArgument);

    CachedCapabilityResult wrong_kind;
    wrong_kind.kind = CapabilityKind::kOcr;
    wrong_kind.detection_regions.push_back(DetectionRegion{});
    EXPECT_EQ(cache.insert(capability_cache_digest(base_fields()), wrong_kind).status().code(),
              ErrorCode::kInvalidArgument);
}

TEST(CapabilityResultCacheTest, EmptyResultIsCachable) {
    CapabilityResultCache cache = CapabilityResultCache::create(96).take_value();
    const CacheKeyDigest key = capability_cache_digest(base_fields());
    CachedCapabilityResult empty;
    empty.kind = CapabilityKind::kDetection;
    ASSERT_TRUE(cache.insert(key, empty).ok());
    const CapabilityPayload hit = cache.lookup(key).value();
    ASSERT_NE(hit, nullptr);
    EXPECT_TRUE(hit->detection_regions.empty());
}

TEST(CapabilityResultCacheTest, EraseAndClearResetState) {
    CapabilityResultCache cache = CapabilityResultCache::create(4096).take_value();
    const CacheKeyDigest key = capability_cache_digest(base_fields());
    ASSERT_TRUE(cache.insert(key, text_result(1)).ok());
    EXPECT_TRUE(cache.erase(key));
    EXPECT_FALSE(cache.erase(key));
    EXPECT_EQ(cache.byte_size(), 0);

    ASSERT_TRUE(cache.insert(key, text_result(1)).ok());
    cache.clear();
    EXPECT_EQ(cache.entry_count(), 0U);
    EXPECT_EQ(cache.lookup(key).value(), nullptr);
}

}  // namespace
