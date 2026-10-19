// Unit tests for the M8 budgeted OCR contract (M8-02, DEC-025): request
// validation caps with exact boundary values, deterministic line orders,
// report structural invariants (one case per violation), owned-byte
// accounting and the request parameter digest.

#include <mirador/geometry.hpp>
#include <mirador/ocr_batch.hpp>

#include <mirador/status.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::kOcrBatchMaxPriorityBytes;
using mirador::kOcrBatchMaxPriorityEntries;
using mirador::kOcrBatchMaxReportBytes;
using mirador::OcrBatchCompletion;
using mirador::OcrBatchRequest;
using mirador::OcrBatchResult;
using mirador::OcrBatchStopReason;
using mirador::OcrCompletionState;
using mirador::OcrLineRecord;
using mirador::OcrPriorityEntry;
using mirador::OcrSelectionStrategy;
using mirador::RectF;
using mirador::validate_ocr_batch_request;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

OcrLineRecord make_line(int32_t candidate_id, float x, float y, float width, float height, float det_confidence,
                        OcrCompletionState state = OcrCompletionState::kDeferred, std::string text = {},
                        std::optional<float> rec_confidence = std::nullopt, int32_t execution_order = -1) {
    OcrLineRecord line;
    line.candidate_id = candidate_id;
    line.bounds = RectF{x, y, width, height};
    line.det_confidence = det_confidence;
    line.state = state;
    line.utf8_text = std::move(text);
    line.rec_confidence = rec_confidence;
    line.execution_order = execution_order;
    return line;
}

OcrLineRecord recognized_line(int32_t candidate_id, float x, float y, float width, float height, float det_confidence,
                              int32_t execution_order) {
    return make_line(candidate_id, x, y, width, height, det_confidence, OcrCompletionState::kRecognized, "text", 0.75F,
                     execution_order);
}

/// Minimal structurally valid completed report: one recognized line.
OcrBatchResult valid_complete_result() {
    OcrBatchResult result;
    result.lines = {recognized_line(0, 0.0F, 0.0F, 10.0F, 5.0F, 0.8F, 0)};
    result.detected_count = 1;
    result.det_limit = 0;
    result.det_limit_hit = false;
    result.recognized_count = 1;
    result.reused_count = 0;
    result.deferred_count = 0;
    result.rec_forward_count = 1;
    result.stop_reason = OcrBatchStopReason::kCompleted;
    result.completion = OcrBatchCompletion::kComplete;
    return result;
}

OcrBatchRequest valid_request() {
    OcrBatchRequest request;
    request.max_rec_lines = 8;
    return request;
}

// --- Contract constants (DEC-025 section 2) ---------------------------------------

TEST(OcrBatchConstants, DefaultCapsMatchDec025) {
    EXPECT_EQ(kOcrBatchMaxPriorityEntries, 64);
    EXPECT_EQ(kOcrBatchMaxPriorityBytes, int64_t{64} * 1024);
    EXPECT_EQ(kOcrBatchMaxReportBytes, int64_t{16} * 1024 * 1024);
}

TEST(OcrBatchConstants, ReferenceConfigurationExistsButIsNotAppliedImplicitly) {
    EXPECT_EQ(mirador::kOcrBatchReferenceMaxRecLines, 64);
    EXPECT_EQ(mirador::kOcrBatchReferenceFinalizeReserve, std::chrono::milliseconds{500});
    // Nothing applies them implicitly: a default request does not pick them up.
    const OcrBatchRequest request;
    EXPECT_EQ(request.max_rec_lines, 0);
}

// --- validate_ocr_batch_request ----------------------------------------------------

TEST(OcrBatchRequestValidation, ValidRequestPasses) {
    EXPECT_TRUE(validate_ocr_batch_request(valid_request()).ok());
}

TEST(OcrBatchRequestValidation, RecLineBudgetMustBePositive) {
    OcrBatchRequest zero = valid_request();
    zero.max_rec_lines = 0;
    EXPECT_EQ(validate_ocr_batch_request(zero).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest negative = valid_request();
    negative.max_rec_lines = -1;
    EXPECT_EQ(validate_ocr_batch_request(negative).status().code(), ErrorCode::kInvalidArgument);
}

TEST(OcrBatchRequestValidation, PriorityEntryCapBoundaries) {
    OcrBatchRequest zero = valid_request();
    zero.max_priority_entries = 0;
    EXPECT_EQ(validate_ocr_batch_request(zero).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest negative = valid_request();
    negative.max_priority_entries = -1;
    EXPECT_EQ(validate_ocr_batch_request(negative).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest at_cap = valid_request();
    at_cap.max_priority_entries = kOcrBatchMaxPriorityEntries;  // 64, the default
    EXPECT_TRUE(validate_ocr_batch_request(at_cap).ok());

    OcrBatchRequest over_cap = valid_request();
    over_cap.max_priority_entries = kOcrBatchMaxPriorityEntries + 1;
    EXPECT_EQ(validate_ocr_batch_request(over_cap).status().code(), ErrorCode::kInvalidArgument);
}

TEST(OcrBatchRequestValidation, PriorityByteCapBoundaries) {
    OcrBatchRequest zero = valid_request();
    zero.max_priority_bytes = 0;
    EXPECT_EQ(validate_ocr_batch_request(zero).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest at_cap = valid_request();
    at_cap.max_priority_bytes = kOcrBatchMaxPriorityBytes;
    EXPECT_TRUE(validate_ocr_batch_request(at_cap).ok());

    OcrBatchRequest over_cap = valid_request();
    over_cap.max_priority_bytes = kOcrBatchMaxPriorityBytes + 1;
    EXPECT_EQ(validate_ocr_batch_request(over_cap).status().code(), ErrorCode::kInvalidArgument);
}

TEST(OcrBatchRequestValidation, ReportByteCapBoundaries) {
    OcrBatchRequest zero = valid_request();
    zero.max_report_bytes = 0;
    EXPECT_EQ(validate_ocr_batch_request(zero).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest at_cap = valid_request();
    at_cap.max_report_bytes = kOcrBatchMaxReportBytes;
    EXPECT_TRUE(validate_ocr_batch_request(at_cap).ok());

    OcrBatchRequest over_cap = valid_request();
    over_cap.max_report_bytes = kOcrBatchMaxReportBytes + 1;
    EXPECT_EQ(validate_ocr_batch_request(over_cap).status().code(), ErrorCode::kInvalidArgument);
}

TEST(OcrBatchRequestValidation, NegativeDurationsAreRejectedZeroIsAllowed) {
    OcrBatchRequest negative_soft = valid_request();
    negative_soft.soft_budget = std::chrono::milliseconds{-1};
    EXPECT_EQ(validate_ocr_batch_request(negative_soft).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest zero_soft = valid_request();
    zero_soft.soft_budget = std::chrono::milliseconds{0};
    EXPECT_TRUE(validate_ocr_batch_request(zero_soft).ok());

    OcrBatchRequest negative_reserve = valid_request();
    negative_reserve.finalize_reserve = std::chrono::milliseconds{-1};
    EXPECT_EQ(validate_ocr_batch_request(negative_reserve).status().code(), ErrorCode::kInvalidArgument);
}

TEST(OcrBatchRequestValidation, EntryCountOverCapIsBudgetExceeded) {
    // Default caps: 64 entries fit, 65 exceed the element budget.
    OcrBatchRequest at_cap = valid_request();
    at_cap.priorities.assign(static_cast<size_t>(kOcrBatchMaxPriorityEntries), OcrPriorityEntry{});
    EXPECT_TRUE(validate_ocr_batch_request(at_cap).ok());

    OcrBatchRequest over_cap = valid_request();
    over_cap.priorities.assign(static_cast<size_t>(kOcrBatchMaxPriorityEntries) + 1U, OcrPriorityEntry{});
    EXPECT_EQ(validate_ocr_batch_request(over_cap).status().code(), ErrorCode::kBudgetExceeded);

    // A tightened entry cap binds the same way.
    OcrBatchRequest tightened = valid_request();
    tightened.max_priority_entries = 2;
    tightened.priorities.assign(3U, OcrPriorityEntry{});
    EXPECT_EQ(validate_ocr_batch_request(tightened).status().code(), ErrorCode::kBudgetExceeded);
}

TEST(OcrBatchRequestValidation, EntryBytesOverCapIsBudgetExceeded) {
    const auto entry_bytes = static_cast<int64_t>(sizeof(OcrPriorityEntry));

    // Exactly the cap passes (strictly-greater comparison).
    OcrBatchRequest at_cap = valid_request();
    at_cap.max_priority_bytes = entry_bytes;
    at_cap.priorities.assign(1U, OcrPriorityEntry{});
    EXPECT_TRUE(validate_ocr_batch_request(at_cap).ok());

    // One entry more exceeds the byte budget even though the count fits.
    OcrBatchRequest over_cap = valid_request();
    over_cap.max_priority_bytes = entry_bytes;
    over_cap.priorities.assign(2U, OcrPriorityEntry{});
    EXPECT_EQ(validate_ocr_batch_request(over_cap).status().code(), ErrorCode::kBudgetExceeded);
}

TEST(OcrBatchRequestValidation, NonFinitePriorityFieldsAreRejected) {
    OcrBatchRequest nan_roi = valid_request();
    nan_roi.priorities = {OcrPriorityEntry{RectF{kNaN, 0.0F, 1.0F, 1.0F}, 0.5F}};
    EXPECT_EQ(validate_ocr_batch_request(nan_roi).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest infinite_width = valid_request();
    infinite_width.priorities = {OcrPriorityEntry{RectF{0.0F, 0.0F, kInf, 1.0F}, 0.5F}};
    EXPECT_EQ(validate_ocr_batch_request(infinite_width).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest nan_priority = valid_request();
    nan_priority.priorities = {OcrPriorityEntry{RectF{0.0F, 0.0F, 1.0F, 1.0F}, kNaN}};
    EXPECT_EQ(validate_ocr_batch_request(nan_priority).status().code(), ErrorCode::kInvalidArgument);
}

TEST(OcrBatchRequestValidation, NegativePriorityDimensionsAreRejectedZeroIsAllowed) {
    OcrBatchRequest negative_width = valid_request();
    negative_width.priorities = {OcrPriorityEntry{RectF{0.0F, 0.0F, -1.0F, 1.0F}, 0.5F}};
    EXPECT_EQ(validate_ocr_batch_request(negative_width).status().code(), ErrorCode::kInvalidArgument);

    OcrBatchRequest negative_height = valid_request();
    negative_height.priorities = {OcrPriorityEntry{RectF{0.0F, 0.0F, 1.0F, -2.5F}, 0.5F}};
    EXPECT_EQ(validate_ocr_batch_request(negative_height).status().code(), ErrorCode::kInvalidArgument);

    // Degenerate (zero-area) entries are structurally legal; they simply never
    // intersect anything (asserted through priority resolution below).
    OcrBatchRequest degenerate = valid_request();
    degenerate.priorities = {OcrPriorityEntry{RectF{5.0F, 5.0F, 0.0F, 10.0F}, 0.5F}};
    EXPECT_TRUE(validate_ocr_batch_request(degenerate).ok());
}

// --- resolve_ocr_line_priority ------------------------------------------------------

TEST(OcrLinePriorityResolution, NoEntriesResolveToZero) {
    const RectF bounds{0.0F, 0.0F, 10.0F, 10.0F};
    EXPECT_EQ(mirador::resolve_ocr_line_priority(bounds, {}), 0.0F);
}

TEST(OcrLinePriorityResolution, NonIntersectingEntriesResolveToZero) {
    const RectF bounds{0.0F, 0.0F, 10.0F, 10.0F};
    const std::vector<OcrPriorityEntry> entries = {
        OcrPriorityEntry{RectF{20.0F, 0.0F, 5.0F, 5.0F}, 0.9F},
        OcrPriorityEntry{RectF{0.0F, -10.0F, 5.0F, 5.0F}, 0.8F},
    };
    EXPECT_EQ(mirador::resolve_ocr_line_priority(bounds, entries), 0.0F);
}

TEST(OcrLinePriorityResolution, PositiveAreaIntersectionPicksHighestPriority) {
    const RectF bounds{0.0F, 0.0F, 10.0F, 10.0F};
    const std::vector<OcrPriorityEntry> entries = {
        OcrPriorityEntry{RectF{8.0F, 8.0F, 4.0F, 4.0F}, 0.4F},    // 2x2 overlap corner
        OcrPriorityEntry{RectF{2.0F, 2.0F, 20.0F, 20.0F}, 0.9F},  // fully contains bounds
        OcrPriorityEntry{RectF{-5.0F, -5.0F, 6.0F, 6.0F}, 0.1F},  // 1x1 overlap corner
    };
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, entries), 0.9F);
}

TEST(OcrLinePriorityResolution, BoundaryContactWithoutAreaIsNotAnIntersection) {
    const RectF bounds{0.0F, 0.0F, 10.0F, 10.0F};
    // Each entry only touches one edge of the bounds; the shared area is zero.
    const std::vector<OcrPriorityEntry> touching = {
        OcrPriorityEntry{RectF{10.0F, 0.0F, 5.0F, 5.0F}, 0.9F},   // right edge
        OcrPriorityEntry{RectF{0.0F, 10.0F, 5.0F, 5.0F}, 0.8F},   // bottom edge
        OcrPriorityEntry{RectF{-5.0F, 0.0F, 5.0F, 10.0F}, 0.7F},  // left edge
        OcrPriorityEntry{RectF{0.0F, -5.0F, 10.0F, 5.0F}, 0.6F},  // top edge
        OcrPriorityEntry{RectF{5.0F, 5.0F, 0.0F, 10.0F}, 0.5F},   // zero-width stripe inside
    };
    EXPECT_EQ(mirador::resolve_ocr_line_priority(bounds, touching), 0.0F);
}

TEST(OcrLinePriorityResolution, ResultIsIndependentOfEntryOrder) {
    const RectF bounds{0.0F, 0.0F, 10.0F, 10.0F};
    const std::vector<OcrPriorityEntry> first = {
        OcrPriorityEntry{RectF{8.0F, 8.0F, 4.0F, 4.0F}, 0.4F},
        OcrPriorityEntry{RectF{2.0F, 2.0F, 4.0F, 4.0F}, 0.9F},
        OcrPriorityEntry{RectF{-5.0F, -5.0F, 6.0F, 6.0F}, 0.1F},
    };
    std::vector<OcrPriorityEntry> second = first;
    std::reverse(second.begin(), second.end());
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, first),
                    mirador::resolve_ocr_line_priority(bounds, second));
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, first), 0.9F);
}

TEST(OcrLinePriorityResolution, NegativePriorityIsMeaningfulAndNaNRanksLast) {
    // A negative priority is not clamped: a matched entry ranks below
    // unprioritized lines on purpose, and 0 is returned exclusively when no
    // entry applies. NaN (ordered as "never first") ranks last as -inf.
    const RectF bounds{0.0F, 0.0F, 10.0F, 10.0F};

    // A lone matching negative entry resolves to itself, distinguishable from
    // an unprioritized line (which resolves to 0).
    const std::vector<OcrPriorityEntry> negative = {OcrPriorityEntry{RectF{0.0F, 0.0F, 5.0F, 5.0F}, -5.0F}};
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, negative), -5.0F);
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, {}), 0.0F);

    // Among matched entries the highest wins, negatives included.
    const std::vector<OcrPriorityEntry> negatives = {OcrPriorityEntry{RectF{0.0F, 0.0F, 5.0F, 5.0F}, -5.0F},
                                                     OcrPriorityEntry{RectF{2.0F, 2.0F, 4.0F, 4.0F}, -1.0F}};
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, negatives), -1.0F);
    std::vector<OcrPriorityEntry> reversed = negatives;
    std::reverse(reversed.begin(), reversed.end());
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, reversed), -1.0F);

    // A NaN entry resolves to -inf, and never masks a higher finite priority.
    const std::vector<OcrPriorityEntry> nan = {OcrPriorityEntry{RectF{0.0F, 0.0F, 5.0F, 5.0F}, kNaN}};
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, nan), -kInf);
    const std::vector<OcrPriorityEntry> mixed = {OcrPriorityEntry{RectF{0.0F, 0.0F, 5.0F, 5.0F}, kNaN},
                                                 OcrPriorityEntry{RectF{0.0F, 0.0F, 5.0F, 5.0F}, 0.3F}};
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, mixed), 0.3F);

    // A non-intersecting entry never applies, negative priority or not.
    const std::vector<OcrPriorityEntry> negative_elsewhere = {OcrPriorityEntry{RectF{20.0F, 20.0F, 5.0F, 5.0F}, -5.0F}};
    EXPECT_FLOAT_EQ(mirador::resolve_ocr_line_priority(bounds, negative_elsewhere), 0.0F);
}

// --- ocr_line_reading_less ----------------------------------------------------------

std::vector<OcrLineRecord> reading_fixture() {
    return {
        // y=0 group: x decides between (x=0) and (x=5).
        make_line(3, 0.0F, 0.0F, 20.0F, 5.0F, 0.1F),
        make_line(5, 5.0F, 0.0F, 20.0F, 9.0F, 0.0F),
        // x=0/y=0 group: width decides (20 vs 10), longer first.
        make_line(4, 0.0F, 0.0F, 20.0F, 3.0F, 0.9F),
        make_line(1, 0.0F, 0.0F, 10.0F, 9.0F, 0.2F),
        // Same origin and size: det confidence descending, NaN last.
        make_line(7, 0.0F, 2.0F, 50.0F, 1.0F, 0.7F),
        make_line(6, 0.0F, 2.0F, 50.0F, 1.0F, 0.7F),
        make_line(2, 0.0F, 2.0F, 50.0F, 1.0F, kNaN),
        // Everything equal: candidate id decides.
        make_line(10, 4.0F, 2.0F, 3.0F, 3.0F, 0.5F),
        make_line(9, 4.0F, 2.0F, 3.0F, 3.0F, 0.5F),
    };
}

TEST(OcrLineReadingOrder, SortsByYThenXThenWidthThenHeightThenDetThenId) {
    std::vector<OcrLineRecord> lines = reading_fixture();
    std::sort(lines.begin(), lines.end(), mirador::ocr_line_reading_less);
    // Expected reading order, justified key by key:
    // (0,0,w20,h5) < (0,0,w20,h3)  — height 5 > 3, taller first
    //              < (0,0,w10,h9)  — width 20 > 10, longer first
    //              < (5,0,...)     — x ascending
    // then y=2 row: det 0.7 (id 6 < 7), then det NaN, then the id tie pair.
    const std::vector<int32_t> expected_ids = {3, 4, 1, 5, 6, 7, 2, 9, 10};
    ASSERT_EQ(lines.size(), expected_ids.size());
    for (size_t index = 0; index < expected_ids.size(); ++index) {
        EXPECT_EQ(lines[index].candidate_id, expected_ids[index]) << "position " << index;
    }
}

TEST(OcrLineReadingOrder, IsStrictTotalOrderOverTheFixture) {
    const std::vector<OcrLineRecord> lines = reading_fixture();
    for (size_t a = 0; a < lines.size(); ++a) {
        EXPECT_FALSE(mirador::ocr_line_reading_less(lines[a], lines[a]))
            << "irreflexivity, id " << lines[a].candidate_id;
        for (size_t b = a + 1; b < lines.size(); ++b) {
            const bool ab = mirador::ocr_line_reading_less(lines[a], lines[b]);
            const bool ba = mirador::ocr_line_reading_less(lines[b], lines[a]);
            EXPECT_NE(ab, ba) << "asymmetry between ids " << lines[a].candidate_id << " and " << lines[b].candidate_id;
        }
    }
}

TEST(OcrLineReadingOrder, IsDeterministicForEqualInputs) {
    const std::vector<OcrLineRecord> first = reading_fixture();
    const std::vector<OcrLineRecord> second = reading_fixture();
    for (size_t a = 0; a < first.size(); ++a) {
        for (size_t b = 0; b < first.size(); ++b) {
            EXPECT_EQ(mirador::ocr_line_reading_less(first[a], first[b]),
                      mirador::ocr_line_reading_less(second[a], second[b]));
        }
    }
}

// --- ocr_line_executes_before --------------------------------------------------------

TEST(OcrLineExecutionOrder, PriorityDominatesStrategyAndReadingOrder) {
    const OcrLineRecord reading_first = make_line(0, 0.0F, 0.0F, 10.0F, 5.0F, 0.9F);
    const OcrLineRecord reading_later = make_line(1, 20.0F, 40.0F, 10.0F, 5.0F, 0.1F);
    for (const OcrSelectionStrategy strategy : {OcrSelectionStrategy::kReadingOrder, OcrSelectionStrategy::kDetScore}) {
        // Higher priority wins even against the reading order.
        EXPECT_TRUE(mirador::ocr_line_executes_before(reading_later, reading_first, 0.2F, 0.1F, strategy));
        EXPECT_FALSE(mirador::ocr_line_executes_before(reading_later, reading_first, 0.1F, 0.2F, strategy));
        // Equal priorities fall through to the strategy/reading keys.
        EXPECT_TRUE(mirador::ocr_line_executes_before(reading_first, reading_later, 0.2F, 0.2F, strategy));
    }
}

TEST(OcrLineExecutionOrder, NanPriorityIsNeverFirst) {
    const OcrLineRecord reading_first = make_line(0, 0.0F, 0.0F, 10.0F, 5.0F, 0.9F);
    const OcrLineRecord reading_later = make_line(1, 20.0F, 40.0F, 10.0F, 5.0F, 0.1F);
    EXPECT_TRUE(mirador::ocr_line_executes_before(reading_later, reading_first, 0.0F, kNaN,
                                                  OcrSelectionStrategy::kReadingOrder));
    // Two NaN priorities are equal ("never first" vs "never first") and fall
    // through to the strategy key.
    EXPECT_TRUE(mirador::ocr_line_executes_before(reading_first, reading_later, kNaN, kNaN,
                                                  OcrSelectionStrategy::kReadingOrder));
}

TEST(OcrLineExecutionOrder, DetScoreStrategyOrdersByConfidenceThenReading) {
    const OcrLineRecord low_score_reading_first = make_line(0, 0.0F, 0.0F, 10.0F, 5.0F, 0.2F);
    const OcrLineRecord high_score_reading_later = make_line(1, 20.0F, 40.0F, 10.0F, 5.0F, 0.9F);
    // kReadingOrder ignores det confidence entirely.
    EXPECT_TRUE(mirador::ocr_line_executes_before(low_score_reading_first, high_score_reading_later, 0.5F, 0.5F,
                                                  OcrSelectionStrategy::kReadingOrder));
    // kDetScore promotes the higher score.
    EXPECT_TRUE(mirador::ocr_line_executes_before(high_score_reading_later, low_score_reading_first, 0.5F, 0.5F,
                                                  OcrSelectionStrategy::kDetScore));
    // A det tie falls back to reading order.
    const OcrLineRecord tie_later = make_line(2, 20.0F, 40.0F, 10.0F, 5.0F, 0.2F);
    EXPECT_TRUE(mirador::ocr_line_executes_before(low_score_reading_first, tie_later, 0.5F, 0.5F,
                                                  OcrSelectionStrategy::kDetScore));
    // NaN det confidence orders last under kDetScore.
    const OcrLineRecord nan_score = make_line(3, 20.0F, 40.0F, 10.0F, 5.0F, kNaN);
    EXPECT_TRUE(mirador::ocr_line_executes_before(low_score_reading_first, nan_score, 0.5F, 0.5F,
                                                  OcrSelectionStrategy::kDetScore));
}

TEST(OcrLineExecutionOrder, FullyEqualLinesFallBackToReadingOrderTieBreaks) {
    const OcrLineRecord first = make_line(4, 0.0F, 0.0F, 10.0F, 5.0F, 0.5F);
    const OcrLineRecord second = make_line(9, 0.0F, 0.0F, 10.0F, 5.0F, 0.5F);
    EXPECT_TRUE(mirador::ocr_line_executes_before(first, second, 0.5F, 0.5F, OcrSelectionStrategy::kDetScore));
    EXPECT_FALSE(mirador::ocr_line_executes_before(second, first, 0.5F, 0.5F, OcrSelectionStrategy::kDetScore));
    // Bitwise-stable for equal inputs: repeated calls agree.
    EXPECT_EQ(mirador::ocr_line_executes_before(first, second, 0.5F, 0.5F, OcrSelectionStrategy::kReadingOrder),
              mirador::ocr_line_executes_before(first, second, 0.5F, 0.5F, OcrSelectionStrategy::kReadingOrder));
}

// --- ocr_batch_result_owned_bytes ----------------------------------------------------

TEST(OcrBatchReportBytes, EmptyReportIsExactlyTheResultHeader) {
    const OcrBatchResult result;
    EXPECT_EQ(mirador::ocr_batch_result_owned_bytes(result), static_cast<int64_t>(sizeof(OcrBatchResult)));
}

TEST(OcrBatchReportBytes, SumsLineRecordsAndTextCapacities) {
    OcrBatchResult result;
    const OcrLineRecord short_text = recognized_line(0, 0.0F, 0.0F, 1.0F, 1.0F, 0.5F, 0);
    OcrLineRecord long_text = recognized_line(1, 0.0F, 2.0F, 1.0F, 1.0F, 0.5F, 1);
    long_text.utf8_text.reserve(1000);
    result.lines = {short_text, long_text};

    auto expected = static_cast<int64_t>(sizeof(OcrBatchResult));
    for (const OcrLineRecord& line : result.lines) {
        expected += static_cast<int64_t>(sizeof(OcrLineRecord));
        expected += static_cast<int64_t>(line.utf8_text.capacity());
    }
    EXPECT_EQ(mirador::ocr_batch_result_owned_bytes(result), expected);
    // Deterministic: a second evaluation gives the same number.
    EXPECT_EQ(mirador::ocr_batch_result_owned_bytes(result), expected);
}

TEST(OcrBatchReportBytes, UnusedVectorCapacityIsNotCounted) {
    OcrBatchResult result;
    result.lines.reserve(64);
    result.lines.push_back(recognized_line(0, 0.0F, 0.0F, 1.0F, 1.0F, 0.5F, 0));
    const int64_t expected = static_cast<int64_t>(sizeof(OcrBatchResult)) +
                             static_cast<int64_t>(sizeof(OcrLineRecord)) +
                             static_cast<int64_t>(result.lines[0].utf8_text.capacity());
    EXPECT_EQ(mirador::ocr_batch_result_owned_bytes(result), expected);
}

// --- ocr_batch_result_consistent ------------------------------------------------------

TEST(OcrBatchReportConsistency, EmptyCompletedReportIsConsistent) {
    const OcrBatchResult result;  // zero lines, kCompleted, kComplete, zero counters
    EXPECT_TRUE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, ValidCompleteReportIsConsistent) {
    EXPECT_TRUE(mirador::ocr_batch_result_consistent(valid_complete_result()));
}

TEST(OcrBatchReportConsistency, ValidPartialReportWithPermutedExecutionOrdersIsConsistent) {
    OcrBatchResult result;
    // Recognized out of reading order: execution orders are a permutation.
    result.lines = {recognized_line(0, 0.0F, 20.0F, 10.0F, 5.0F, 0.6F, 1),
                    make_line(1, 0.0F, 0.0F, 10.0F, 5.0F, 0.9F),  // deferred geometry only
                    recognized_line(2, 0.0F, 10.0F, 10.0F, 5.0F, 0.7F, 0)};
    result.detected_count = 3;
    result.det_limit = 12;
    result.det_limit_hit = true;
    result.recognized_count = 2;
    result.deferred_count = 1;
    result.rec_forward_count = 2;
    result.stop_reason = OcrBatchStopReason::kRecLineLimit;
    result.completion = OcrBatchCompletion::kPartial;
    EXPECT_TRUE(mirador::ocr_batch_result_consistent(result));

    // The soft-budget stop is coherent in the same shape.
    OcrBatchResult soft = result;
    soft.stop_reason = OcrBatchStopReason::kSoftBudget;
    EXPECT_TRUE(mirador::ocr_batch_result_consistent(soft));
}

TEST(OcrBatchReportConsistency, DetectedCountMismatchFails) {
    OcrBatchResult result = valid_complete_result();
    result.detected_count = 2;  // one line present
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, BrokenCountConservationFails) {
    OcrBatchResult result = valid_complete_result();
    result.deferred_count = 1;  // 1 != 1 + 0 + 1
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, NonZeroReusedCountFails) {
    // M8 freeze: the reuse channel never produces lines, so the counter must
    // stay 0 even when the per-line states would nominally allow it.
    OcrBatchResult result = valid_complete_result();
    result.reused_count = 1;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, RecForwardCountMismatchFails) {
    OcrBatchResult result = valid_complete_result();
    result.rec_forward_count = 2;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));

    OcrBatchResult zero = valid_complete_result();
    zero.rec_forward_count = 0;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(zero));
}

TEST(OcrBatchReportConsistency, DetLimitHitWithoutPositiveLimitFails) {
    OcrBatchResult result = valid_complete_result();
    result.det_limit_hit = true;  // det_limit stays 0
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, RecognizedLineWithoutScoreFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines[0].rec_confidence.reset();
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, RecognizedLineWithNegativeExecutionOrderFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines[0].execution_order = -1;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, DeferredLineWithTextFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines[0].state = OcrCompletionState::kDeferred;
    result.lines[0].utf8_text = "leaked";
    result.lines[0].rec_confidence.reset();
    result.lines[0].execution_order = -1;
    result.recognized_count = 0;
    result.deferred_count = 1;
    result.rec_forward_count = 0;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, DeferredLineWithScoreFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines[0].state = OcrCompletionState::kDeferred;
    result.lines[0].utf8_text.clear();
    result.recognized_count = 0;
    result.deferred_count = 1;
    result.rec_forward_count = 0;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, DeferredLineWithExecutionOrderFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines[0].state = OcrCompletionState::kDeferred;
    result.lines[0].utf8_text.clear();
    result.lines[0].rec_confidence.reset();
    result.lines[0].execution_order = 0;
    result.recognized_count = 0;
    result.deferred_count = 1;
    result.rec_forward_count = 0;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, DuplicateCandidateIdFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines = {recognized_line(0, 0.0F, 0.0F, 10.0F, 5.0F, 0.8F, 0),
                    recognized_line(0, 0.0F, 10.0F, 10.0F, 5.0F, 0.7F, 1)};
    result.detected_count = 2;
    result.recognized_count = 2;
    result.rec_forward_count = 2;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, NegativeCandidateIdFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines[0].candidate_id = -1;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, NonPermutationExecutionOrdersFail) {
    OcrBatchResult duplicated = valid_complete_result();
    duplicated.lines = {recognized_line(0, 0.0F, 0.0F, 10.0F, 5.0F, 0.8F, 0),
                        recognized_line(1, 0.0F, 10.0F, 10.0F, 5.0F, 0.7F, 0)};
    duplicated.detected_count = 2;
    duplicated.recognized_count = 2;
    duplicated.rec_forward_count = 2;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(duplicated));

    OcrBatchResult gapped = duplicated;
    gapped.lines[1].execution_order = 2;  // {0, 2} instead of {0, 1}
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(gapped));
}

TEST(OcrBatchReportConsistency, PerLineStateCountsMustMatchCounters) {
    OcrBatchResult result = valid_complete_result();
    result.lines = {recognized_line(0, 0.0F, 0.0F, 10.0F, 5.0F, 0.8F, 0),
                    make_line(1, 0.0F, 10.0F, 10.0F, 5.0F, 0.7F)};  // deferred line
    result.detected_count = 2;
    result.deferred_count = 1;  // counters now claim 1 recognized + 1 deferred
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, CompletedStopWithDeferredLinesFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines.push_back(make_line(1, 0.0F, 10.0F, 10.0F, 5.0F, 0.7F));
    result.detected_count = 2;
    result.deferred_count = 1;  // kCompleted but a line was deferred
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, CompletedStopWithPartialCompletionFails) {
    OcrBatchResult result = valid_complete_result();
    result.completion = OcrBatchCompletion::kPartial;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, LineLimitStopWithoutDeferredLinesFails) {
    OcrBatchResult result = valid_complete_result();
    result.stop_reason = OcrBatchStopReason::kRecLineLimit;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, LineLimitStopWithCompleteCompletionFails) {
    OcrBatchResult result = valid_complete_result();
    result.lines.push_back(make_line(1, 0.0F, 10.0F, 10.0F, 5.0F, 0.7F));
    result.detected_count = 2;
    result.deferred_count = 1;
    result.stop_reason = OcrBatchStopReason::kRecLineLimit;  // completion stays kComplete
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(result));
}

TEST(OcrBatchReportConsistency, SoftBudgetStopNeedsDeferredLinesAndPartialCompletion) {
    OcrBatchResult without_deferred = valid_complete_result();
    without_deferred.stop_reason = OcrBatchStopReason::kSoftBudget;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(without_deferred));

    OcrBatchResult complete_completion = valid_complete_result();
    complete_completion.lines.push_back(make_line(1, 0.0F, 10.0F, 10.0F, 5.0F, 0.7F));
    complete_completion.detected_count = 2;
    complete_completion.deferred_count = 1;
    complete_completion.stop_reason = OcrBatchStopReason::kSoftBudget;
    EXPECT_FALSE(mirador::ocr_batch_result_consistent(complete_completion));
}

// --- ocr_batch_request_params_digest ---------------------------------------------------

TEST(OcrBatchRequestDigest, EqualRequestsHaveEqualDigests) {
    const OcrBatchRequest first = valid_request();
    const OcrBatchRequest second = valid_request();
    EXPECT_EQ(mirador::ocr_batch_request_params_digest(first), mirador::ocr_batch_request_params_digest(second));
    // Repeated evaluation of the same object is stable, defaults included.
    EXPECT_EQ(mirador::ocr_batch_request_params_digest(first), mirador::ocr_batch_request_params_digest(first));
    const OcrBatchRequest empty;
    EXPECT_EQ(mirador::ocr_batch_request_params_digest(empty), mirador::ocr_batch_request_params_digest(empty));
}

TEST(OcrBatchRequestDigest, EverySelectionRelevantFieldChangesTheDigest) {
    const OcrBatchRequest base = valid_request();
    const uint64_t base_digest = mirador::ocr_batch_request_params_digest(base);

    OcrBatchRequest language = base;
    language.language_hint = "zh-Hans";
    EXPECT_NE(mirador::ocr_batch_request_params_digest(language), base_digest);

    OcrBatchRequest confidence = base;
    confidence.min_confidence = 0.5F;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(confidence), base_digest);

    OcrBatchRequest params = base;
    params.backend_params = "{\"beam\":4}";
    EXPECT_NE(mirador::ocr_batch_request_params_digest(params), base_digest);

    OcrBatchRequest lines = base;
    lines.max_rec_lines = base.max_rec_lines + 1;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(lines), base_digest);

    OcrBatchRequest strategy = base;
    strategy.selection = OcrSelectionStrategy::kDetScore;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(strategy), base_digest);

    OcrBatchRequest with_priority = base;
    with_priority.priorities = {OcrPriorityEntry{RectF{0.0F, 0.0F, 5.0F, 5.0F}, 0.5F},
                                OcrPriorityEntry{RectF{10.0F, 0.0F, 5.0F, 5.0F}, 0.2F}};
    const uint64_t with_priority_digest = mirador::ocr_batch_request_params_digest(with_priority);
    EXPECT_NE(with_priority_digest, base_digest);

    OcrBatchRequest moved_roi = with_priority;
    moved_roi.priorities[0].roi.x += 1.0F;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(moved_roi), with_priority_digest);

    OcrBatchRequest changed_value = with_priority;
    changed_value.priorities[0].priority = 0.6F;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(changed_value), with_priority_digest);

    OcrBatchRequest extra_entry = with_priority;
    extra_entry.priorities.push_back(OcrPriorityEntry{RectF{1.0F, 1.0F, 1.0F, 1.0F}, 0.1F});
    EXPECT_NE(mirador::ocr_batch_request_params_digest(extra_entry), with_priority_digest);

    // The digest is a sequential feed: permuting entries changes it.
    OcrBatchRequest swapped = with_priority;
    std::swap(swapped.priorities[0], swapped.priorities[1]);
    EXPECT_NE(mirador::ocr_batch_request_params_digest(swapped), with_priority_digest);

    OcrBatchRequest soft = base;
    soft.soft_budget = std::chrono::milliseconds{200};
    const uint64_t soft_digest = mirador::ocr_batch_request_params_digest(soft);
    EXPECT_NE(soft_digest, base_digest);

    OcrBatchRequest soft_value = soft;
    soft_value.soft_budget = std::chrono::milliseconds{300};
    EXPECT_NE(mirador::ocr_batch_request_params_digest(soft_value), soft_digest);

    OcrBatchRequest reserve = base;
    reserve.finalize_reserve = std::chrono::milliseconds{500};
    EXPECT_NE(mirador::ocr_batch_request_params_digest(reserve), base_digest);

    OcrBatchRequest entries_cap = base;
    entries_cap.max_priority_entries = 32;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(entries_cap), base_digest);

    OcrBatchRequest bytes_cap = base;
    bytes_cap.max_priority_bytes = 1024;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(bytes_cap), base_digest);

    OcrBatchRequest report_cap = base;
    report_cap.max_report_bytes = int64_t{1024} * 1024;
    EXPECT_NE(mirador::ocr_batch_request_params_digest(report_cap), base_digest);
}

}  // namespace
