#include <mirador/frame_cache.hpp>

#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::FrameCache;
using mirador::FramePayload;

constexpr int64_t kOverhead = FrameCache::kEntryOverheadBytes;
/// Three 8-byte entries fit exactly.
constexpr int64_t kBudget = 3 * (8 + kOverhead);

std::vector<std::byte> payload_of(std::initializer_list<uint8_t> bytes) {
    std::vector<std::byte> payload;
    payload.reserve(bytes.size());
    for (const uint8_t byte : bytes) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    return payload;
}

std::vector<std::byte> filled_payload(const size_t size, const uint8_t fill) {
    return std::vector<std::byte>(size, static_cast<std::byte>(fill));
}

TEST(FrameCache, InsertLookupRoundtrip) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const std::vector<std::byte> value = payload_of({1, 2, 3});
    ASSERT_TRUE(frames.insert(1, value).ok());

    const auto hit = frames.lookup(1);
    ASSERT_TRUE(hit.ok());
    ASSERT_TRUE(hit.value() != nullptr);
    ASSERT_EQ(hit.value()->size(), value.size());
    EXPECT_EQ(*hit.value(), value);
    EXPECT_TRUE(frames.contains(1));
    EXPECT_EQ(frames.entry_count(), 1U);
    EXPECT_EQ(frames.byte_size(), 3 + kOverhead);
}

TEST(FrameCache, MissCarriesNullPayload) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const auto miss = frames.lookup(99);
    EXPECT_TRUE(miss.ok());
    EXPECT_TRUE(miss.value() == nullptr);
    EXPECT_FALSE(frames.contains(99));
}

TEST(FrameCache, LruEvictionFollowsRecency) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const std::vector<std::byte> value = payload_of({1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_TRUE(frames.insert(1, value).ok());
    ASSERT_TRUE(frames.insert(2, value).ok());
    ASSERT_TRUE(frames.insert(3, value).ok());
    EXPECT_EQ(frames.byte_size(), kBudget);

    // Touching key 1 makes key 2 the least recently used entry.
    const auto promoted = frames.lookup(1);
    ASSERT_TRUE(promoted.ok() && promoted.value() != nullptr);
    ASSERT_TRUE(frames.insert(4, value).ok());

    EXPECT_FALSE(frames.contains(2));
    EXPECT_TRUE(frames.contains(1));
    EXPECT_TRUE(frames.contains(3));
    EXPECT_TRUE(frames.contains(4));
    EXPECT_EQ(frames.entry_count(), 3U);
    EXPECT_EQ(frames.byte_size(), kBudget);
}

TEST(FrameCache, EntryLargerThanBudgetIsRejectedWithoutChanges) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const std::vector<std::byte> small = filled_payload(8, 1);
    ASSERT_TRUE(frames.insert(1, small).ok());
    const int64_t used_before = frames.byte_size();

    const auto too_large = frames.insert(2, filled_payload(400, 7));
    ASSERT_FALSE(too_large.ok());
    EXPECT_EQ(too_large.status().code(), ErrorCode::kBudgetExceeded);

    EXPECT_TRUE(frames.contains(1));
    EXPECT_FALSE(frames.contains(2));
    EXPECT_EQ(frames.byte_size(), used_before);
    EXPECT_EQ(frames.entry_count(), 1U);
}

TEST(FrameCache, ReplacementUpdatesValueAndRecency) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const std::vector<std::byte> first = payload_of({1, 1, 1, 1, 1, 1, 1, 1});
    const std::vector<std::byte> second = payload_of({2, 2, 2, 2, 2, 2, 2, 2});
    ASSERT_TRUE(frames.insert(1, first).ok());
    ASSERT_TRUE(frames.insert(2, first).ok());
    ASSERT_TRUE(frames.insert(3, first).ok());

    // Replacing key 1 re-promotes it; key 2 stays the LRU entry.
    ASSERT_TRUE(frames.insert(1, second).ok());
    const auto hit = frames.lookup(1);
    ASSERT_TRUE(hit.ok() && hit.value() != nullptr);
    EXPECT_EQ(*hit.value(), second);
    EXPECT_EQ(frames.byte_size(), kBudget);

    ASSERT_TRUE(frames.insert(4, first).ok());
    EXPECT_FALSE(frames.contains(2));
    EXPECT_TRUE(frames.contains(1));
    EXPECT_TRUE(frames.contains(3));
}

TEST(FrameCache, TooLargeReplacementKeepsOldValue) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const std::vector<std::byte> first = payload_of({1, 1, 1, 1, 1, 1, 1, 1});
    ASSERT_TRUE(frames.insert(1, first).ok());
    ASSERT_TRUE(frames.insert(2, first).ok());

    const auto rejected = frames.insert(1, filled_payload(200, 9));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), ErrorCode::kBudgetExceeded);

    const auto hit = frames.lookup(1);
    ASSERT_TRUE(hit.ok() && hit.value() != nullptr);
    EXPECT_EQ(*hit.value(), first);
    EXPECT_EQ(frames.byte_size(), 2 * (8 + kOverhead));
    EXPECT_EQ(frames.entry_count(), 2U);
}

TEST(FrameCache, ReplacementEvictsToStayWithinBudget) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const std::vector<std::byte> small = filled_payload(8, 1);
    ASSERT_TRUE(frames.insert(1, small).ok());
    ASSERT_TRUE(frames.insert(2, small).ok());
    ASSERT_TRUE(frames.insert(3, small).ok());

    // The 80-byte replacement of key 1 only fits after evicting key 2 (LRU).
    ASSERT_TRUE(frames.insert(1, filled_payload(80, 5)).ok());
    EXPECT_FALSE(frames.contains(2));
    EXPECT_TRUE(frames.contains(1));
    EXPECT_TRUE(frames.contains(3));
    EXPECT_EQ(frames.byte_size(), 80 + kOverhead + 8 + kOverhead);
    EXPECT_EQ(frames.entry_count(), 2U);
}

TEST(FrameCache, EmptyValueIsStoredAndDistinctFromMiss) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    ASSERT_TRUE(frames.insert(1, std::span<const std::byte>{}).ok());
    EXPECT_EQ(frames.byte_size(), kOverhead);

    const auto hit = frames.lookup(1);
    ASSERT_TRUE(hit.ok());
    ASSERT_TRUE(hit.value() != nullptr);
    EXPECT_TRUE(hit.value()->empty());

    const auto miss = frames.lookup(2);
    ASSERT_TRUE(miss.ok());
    EXPECT_TRUE(miss.value() == nullptr);
}

TEST(FrameCache, EraseAndClearReleaseBudget) {
    auto cache = FrameCache::create(kBudget);
    ASSERT_TRUE(cache.ok());
    FrameCache frames = cache.take_value();

    const std::vector<std::byte> value = filled_payload(8, 3);
    ASSERT_TRUE(frames.insert(1, value).ok());
    ASSERT_TRUE(frames.insert(2, value).ok());

    EXPECT_TRUE(frames.erase(1));
    EXPECT_FALSE(frames.erase(1));
    EXPECT_FALSE(frames.contains(1));
    EXPECT_EQ(frames.byte_size(), 8 + kOverhead);
    EXPECT_EQ(frames.entry_count(), 1U);

    frames.clear();
    EXPECT_EQ(frames.entry_count(), 0U);
    EXPECT_EQ(frames.byte_size(), 0);
    EXPECT_FALSE(frames.contains(2));
}

TEST(FrameCache, CreateRejectsNonPositiveBudgets) {
    const auto zero = FrameCache::create(0);
    ASSERT_FALSE(zero.ok());
    EXPECT_EQ(zero.status().code(), ErrorCode::kInvalidArgument);

    const auto negative = FrameCache::create(-1);
    ASSERT_FALSE(negative.ok());
    EXPECT_EQ(negative.status().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
