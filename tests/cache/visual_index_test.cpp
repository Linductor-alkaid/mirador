// M3-10: unit tests for the bounded layered visual index (design section 12,
// DEC-014).

#include <mirador/visual_index.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>
#include "mirador/status.hpp"
#include "mirador/visual_fingerprint.hpp"

namespace {

using mirador::ErrorCode;
using mirador::VisualEvidenceKind;
using mirador::VisualIndex;
using mirador::VisualPatchFingerprint;
using mirador::VisualQueryParams;

constexpr int32_t kSide = 16;
constexpr int64_t kEntryBytes = static_cast<int64_t>(kSide) * kSide + VisualIndex::kEntryOverheadBytes;

VisualPatchFingerprint make_fingerprint(uint64_t content_hash, uint64_t dhash, uint8_t base, uint8_t amplitude,
                                        uint8_t period) {
    VisualPatchFingerprint fp;
    fp.content_hash = content_hash;
    fp.dhash = dhash;
    fp.thumb_width = kSide;
    fp.thumb_height = kSide;
    fp.thumbnail_gray.resize(static_cast<size_t>(kSide) * kSide);
    for (size_t i = 0; i < fp.thumbnail_gray.size(); ++i) {
        const size_t row = i / static_cast<size_t>(kSide);
        const size_t col = i % static_cast<size_t>(kSide);
        fp.thumbnail_gray[i] = static_cast<std::byte>(base + ((row / period) * (col / period)) % amplitude);
    }
    return fp;
}

TEST(VisualIndex, ExactMatchDominates) {
    auto index_result = VisualIndex::create(kEntryBytes * 4, kSide);
    ASSERT_TRUE(index_result.ok());
    VisualIndex index = index_result.take_value();
    const VisualPatchFingerprint stored = make_fingerprint(0xAAAA, 0x1234, 100, 8, 2);
    ASSERT_TRUE(index.insert(1, stored).ok());

    VisualQueryParams const params;
    const auto hits = index.query(stored, params);
    ASSERT_TRUE(hits.ok()) << hits.status().message();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].entry_id, 1U);
    EXPECT_EQ(hits.value()[0].evidence, VisualEvidenceKind::kExactContent);
    EXPECT_DOUBLE_EQ(hits.value()[0].similarity, 1.0);
}

TEST(VisualIndex, PerceptualLayerMatchesSmallHashDistance) {
    auto index_result = VisualIndex::create(kEntryBytes * 4, kSide);
    ASSERT_TRUE(index_result.ok());
    VisualIndex index = index_result.take_value();
    ASSERT_TRUE(index.insert(7, make_fingerprint(0x1111, 0, 100, 8, 2)).ok());

    VisualQueryParams params;
    VisualPatchFingerprint const probe = make_fingerprint(0x2222, 1, 100, 8, 2);  // hamming distance 1
    auto hits = index.query(probe, params);
    ASSERT_TRUE(hits.ok());
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].evidence, VisualEvidenceKind::kPerceptualHash);
    EXPECT_DOUBLE_EQ(hits.value()[0].similarity, 63.0 / 64.0);

    params.perceptual_similarity_threshold = 0.99;  // 63/64 = 0.984: below the bar
    params.template_ncc_threshold = 1.0;            // disable the template layer
    hits = index.query(probe, params);
    ASSERT_TRUE(hits.ok());
    EXPECT_TRUE(hits.value().empty());
}

TEST(VisualIndex, TemplateLayerMatchesHighCorrelationOnly) {
    auto index_result = VisualIndex::create(kEntryBytes * 4, kSide);
    ASSERT_TRUE(index_result.ok());
    VisualIndex index = index_result.take_value();
    // Stored dHash differs everywhere, so only the thumbnail NCC can match.
    ASSERT_TRUE(index.insert(3, make_fingerprint(0x3333, 0, 120, 6, 2)).ok());

    VisualQueryParams params;
    // Same texture as the entry (NCC = 1.0) but a dHash far above the
    // perceptual bar, so only the template layer can accept it.
    VisualPatchFingerprint const probe = make_fingerprint(0x4444, ~0ULL, 120, 6, 2);
    auto hits = index.query(probe, params);
    ASSERT_TRUE(hits.ok());
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].evidence, VisualEvidenceKind::kTemplate);
    EXPECT_GE(hits.value()[0].similarity, params.template_ncc_threshold);

    params.template_ncc_threshold = 1.0;  // exact NCC only: the layers collapse to exact content
    hits = index.query(probe, params);
    ASSERT_TRUE(hits.ok());
    EXPECT_TRUE(hits.value().empty());
}

TEST(VisualIndex, OrderingByEvidenceThenSimilarity) {
    auto index_result = VisualIndex::create(kEntryBytes * 8, kSide);
    ASSERT_TRUE(index_result.ok());
    VisualIndex index = index_result.take_value();
    // Entry 10: perceptual hit (distance 1). Entry 11: template-only hit.
    ASSERT_TRUE(index.insert(10, make_fingerprint(0x10, 0, 120, 6, 2)).ok());
    ASSERT_TRUE(index.insert(11, make_fingerprint(0x11, ~0ULL, 120, 6, 2)).ok());

    VisualQueryParams params;
    params.perceptual_similarity_threshold = 0.9;
    params.template_ncc_threshold = 0.9;
    const VisualPatchFingerprint probe = make_fingerprint(0x99, 1, 120, 6, 2);
    const auto hits = index.query(probe, params);
    ASSERT_TRUE(hits.ok());
    ASSERT_GE(hits.value().size(), 2U);
    EXPECT_EQ(hits.value()[0].evidence, VisualEvidenceKind::kPerceptualHash);  // layer priority first
    EXPECT_EQ(hits.value()[1].evidence, VisualEvidenceKind::kTemplate);
}

TEST(VisualIndex, CandidateCapAndDeterministicOutput) {
    auto index_result = VisualIndex::create(kEntryBytes * 8, kSide);
    ASSERT_TRUE(index_result.ok());
    VisualIndex index = index_result.take_value();
    for (uint64_t id = 0; id < 5; ++id) {
        ASSERT_TRUE(index.insert(id, make_fingerprint(0x100 + id, 1, 90, 8, 2)).ok());
    }
    VisualQueryParams params;
    params.perceptual_similarity_threshold = 0.9;
    params.max_candidates = 3;
    const auto first = index.query(make_fingerprint(0x777, 1, 90, 8, 2), params);
    const auto second = index.query(make_fingerprint(0x777, 1, 90, 8, 2), params);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(first.value().size(), 3U);
    EXPECT_EQ(first.value(), second.value());
    for (size_t i = 1; i < first.value().size(); ++i) {
        EXPECT_LE(first.value()[i - 1].similarity, first.value()[i].similarity);
        EXPECT_EQ(first.value()[i].evidence, VisualEvidenceKind::kPerceptualHash);
    }
}

TEST(VisualIndex, BudgetEvictionAndPromotion) {
    auto index_result = VisualIndex::create(kEntryBytes * 2, kSide);
    ASSERT_TRUE(index_result.ok());
    VisualIndex index = index_result.take_value();
    ASSERT_TRUE(index.insert(1, make_fingerprint(1, 0, 80, 8, 2)).ok());
    // A foreign texture and a far-apart dHash keep entry 2 out of the query below.
    ASSERT_TRUE(index.insert(2, make_fingerprint(2, ~0ULL, 10, 30, 1)).ok());
    EXPECT_EQ(index.entry_count(), 2U);
    EXPECT_EQ(index.byte_size(), kEntryBytes * 2);

    // Query entry 1: it is promoted above entry 2.
    VisualQueryParams const params;
    const auto hits = index.query(make_fingerprint(1, 0, 80, 8, 2), params);
    ASSERT_TRUE(hits.ok());
    // Insert entry 3: entry 2 (least recently used) is evicted, entry 1 survives.
    ASSERT_TRUE(index.insert(3, make_fingerprint(3, 0, 80, 8, 2)).ok());
    EXPECT_TRUE(index.contains(1));
    EXPECT_FALSE(index.contains(2));
    EXPECT_TRUE(index.contains(3));
    EXPECT_EQ(index.byte_size(), kEntryBytes * 2);
    EXPECT_EQ(index.max_bytes(), kEntryBytes * 2);

    // Replacement reuses one slot.
    ASSERT_TRUE(index.insert(3, make_fingerprint(3, 0, 80, 8, 2)).ok());
    EXPECT_EQ(index.entry_count(), 2U);

    // Erase accounting.
    EXPECT_TRUE(index.erase(3));
    EXPECT_FALSE(index.erase(3));
    EXPECT_EQ(index.byte_size(), kEntryBytes);
}

TEST(VisualIndex, ExplicitErrors) {
    ASSERT_EQ(VisualIndex::create(0, kSide).status().code(), ErrorCode::kInvalidArgument);
    ASSERT_EQ(VisualIndex::create(kEntryBytes * 2, 4).status().code(), ErrorCode::kInvalidArgument);

    auto index_result = VisualIndex::create(kEntryBytes, kSide);
    ASSERT_TRUE(index_result.ok());
    VisualIndex index = index_result.take_value();

    VisualPatchFingerprint foreign = make_fingerprint(1, 0, 80, 8, 2);
    foreign.thumb_width = 8;
    ASSERT_EQ(index.insert(1, foreign).status().code(), ErrorCode::kInvalidArgument);
    VisualQueryParams const params;
    ASSERT_EQ(index.query(foreign, params).status().code(), ErrorCode::kInvalidArgument);

    // Entry alone above the budget.
    auto tiny_result = VisualIndex::create(kEntryBytes - 1, kSide);
    ASSERT_TRUE(tiny_result.ok());
    VisualIndex tiny = tiny_result.take_value();
    ASSERT_EQ(tiny.insert(1, make_fingerprint(1, 0, 80, 8, 2)).status().code(), ErrorCode::kBudgetExceeded);
    EXPECT_EQ(tiny.entry_count(), 0U);  // untouched

    VisualQueryParams bad_threshold;
    bad_threshold.perceptual_similarity_threshold = 1.5;
    ASSERT_EQ(index.query(make_fingerprint(1, 0, 80, 8, 2), bad_threshold).status().code(),
              ErrorCode::kInvalidArgument);
    bad_threshold = VisualQueryParams{};
    bad_threshold.max_candidates = 0;
    ASSERT_EQ(index.query(make_fingerprint(1, 0, 80, 8, 2), bad_threshold).status().code(),
              ErrorCode::kInvalidArgument);
}

}  // namespace
