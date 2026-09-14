// M3-07: unit tests for the text reference components line merge and
// normalization (design section 13, DEC-014 reference adaptation scope).

#include <mirador/text_normalize.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

namespace {

using mirador::ErrorCode;
using mirador::LineMergeParams;
using mirador::RectF;
using mirador::TextNormalizeFlags;
using mirador::TextRegion;

TextRegion make_line(float x, float y, float w, float h, std::string text, float confidence = 0.9F) {
    TextRegion line;
    line.bounds = RectF{x, y, w, h};
    line.utf8_text = std::move(text);
    line.confidence = confidence;
    return line;
}

TEST(MergeTextLines, JoinsSameRowWithSeparatorAndMinConfidence) {
    const std::vector<TextRegion> lines{
        make_line(0.0F, 10.0F, 20.0F, 8.0F, "Hel", 0.9F),
        make_line(22.0F, 10.5F, 20.0F, 8.0F, "lo", 0.6F),  // gap 2, height 8, same row
    };
    LineMergeParams params;
    params.text_separator = "";
    const auto merged = mirador::merge_text_lines(lines, params);
    ASSERT_TRUE(merged.ok()) << merged.status().message();
    ASSERT_EQ(merged.value().size(), 1U);
    EXPECT_EQ(merged.value()[0].utf8_text, "Hello");
    EXPECT_FLOAT_EQ(merged.value()[0].confidence, 0.6F);
    EXPECT_EQ(merged.value()[0].bounds, (RectF{0.0F, 10.0F, 42.0F, 8.5F}));
    EXPECT_TRUE(merged.value()[0].polygon.empty());  // merged polygons are dropped
}

TEST(MergeTextLines, KeepsRowsApartWhenGapOrOffsetExceedsTolerance) {
    const std::vector<TextRegion> lines{
        make_line(0.0F, 10.0F, 20.0F, 8.0F, "a"),
        make_line(40.0F, 10.0F, 20.0F, 8.0F, "b"),  // gap 20 > 1.0 * 8
        make_line(70.0F, 30.0F, 20.0F, 8.0F, "c"),  // different row
    };
    const auto merged = mirador::merge_text_lines(lines, LineMergeParams{});
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 3U);
}

TEST(MergeTextLines, SortsOutputByYThenXAndPreservesInput) {
    const std::vector<TextRegion> lines{
        make_line(30.0F, 20.0F, 5.0F, 5.0F, "3"),
        make_line(10.0F, 5.0F, 5.0F, 5.0F, "1"),
        make_line(10.0F, 20.0F, 5.0F, 5.0F, "2"),
    };
    const std::vector<TextRegion> copy = lines;
    const auto merged = mirador::merge_text_lines(lines, LineMergeParams{});
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 3U);
    EXPECT_EQ(merged.value()[0].utf8_text, "1");
    EXPECT_EQ(merged.value()[1].utf8_text, "2");
    EXPECT_EQ(merged.value()[2].utf8_text, "3");
    EXPECT_EQ(lines, copy);  // RULE-04
}

TEST(MergeTextLines, TransitiveChainMergesIntoOne) {
    const std::vector<TextRegion> lines{
        make_line(0.0F, 0.0F, 8.0F, 8.0F, "a", 0.9F),
        make_line(10.0F, 0.0F, 8.0F, 8.0F, "b", 0.8F),
        make_line(20.0F, 0.0F, 8.0F, 8.0F, "c", 0.7F),
    };
    LineMergeParams params;
    params.text_separator = " ";
    const auto merged = mirador::merge_text_lines(lines, params);
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.value().size(), 1U);
    EXPECT_EQ(merged.value()[0].utf8_text, "a b c");
    EXPECT_FLOAT_EQ(merged.value()[0].confidence, 0.7F);
}

TEST(MergeTextLines, ExplicitErrors) {
    ASSERT_TRUE(mirador::merge_text_lines({}, LineMergeParams{}).ok());

    LineMergeParams bad_ratio;
    bad_ratio.max_gap_ratio = -1.0;
    ASSERT_EQ(mirador::merge_text_lines({}, bad_ratio).status().code(), ErrorCode::kInvalidArgument);

    std::vector<TextRegion> nan_line{make_line(0.0F, 0.0F, 4.0F, 4.0F, "x",
                                               std::numeric_limits<float>::quiet_NaN())};
    ASSERT_EQ(mirador::merge_text_lines(nan_line, LineMergeParams{}).status().code(), ErrorCode::kInvalidArgument);

    LineMergeParams capped;
    capped.max_lines = 1;
    const std::vector<TextRegion> two{make_line(0.0F, 0.0F, 4.0F, 4.0F, "a"),
                                      make_line(0.0F, 20.0F, 4.0F, 4.0F, "b")};
    ASSERT_EQ(mirador::merge_text_lines(two, capped).status().code(), ErrorCode::kBudgetExceeded);
}

TEST(NormalizeText, TrimsCollapsesAndStripsControl) {
    const auto trimmed = mirador::normalize_text("  hello\tworld \n", static_cast<uint32_t>(TextNormalizeFlags::kTrim));
    EXPECT_EQ(trimmed, "hello\tworld");

    const auto collapsed = mirador::normalize_text("a \t\n b", static_cast<uint32_t>(TextNormalizeFlags::kCollapseWhitespace));
    EXPECT_EQ(collapsed, "a b");

    // Control characters other than \t\n\r disappear; they never merge bytes.
    const std::string with_control = std::string("a") + char(1) + char(0x7F) + "b";
    const auto stripped =
        mirador::normalize_text(with_control, static_cast<uint32_t>(TextNormalizeFlags::kStripControl));
    EXPECT_EQ(stripped, "ab");

    const auto all = mirador::normalize_text(
        "  " + with_control + " x ", static_cast<uint32_t>(TextNormalizeFlags::kTrim) |
                                          static_cast<uint32_t>(TextNormalizeFlags::kCollapseWhitespace) |
                                          static_cast<uint32_t>(TextNormalizeFlags::kStripControl));
    EXPECT_EQ(all, "ab x");
}

TEST(NormalizeText, FoldsFullwidthAsciiAndIdeographicSpace) {
    // "Ｈｅｌｌｏ　１２３" — fullwidth forms plus U+3000.
    const std::string fullwidth = "Ｈｅｌｌｏ　１２３";
    const auto folded = mirador::normalize_text(fullwidth, static_cast<uint32_t>(TextNormalizeFlags::kFoldFullwidthAscii));
    EXPECT_EQ(folded, "Hello 123");

    const auto trimmed_fold = mirador::normalize_text("\xEF\xBC\xA8\xE3\x80\x80x",
                                                      static_cast<uint32_t>(TextNormalizeFlags::kFoldFullwidthAscii) |
                                                          static_cast<uint32_t>(TextNormalizeFlags::kTrim));
    EXPECT_EQ(trimmed_fold, "H x");  // folded, internal space kept
}

TEST(NormalizeText, PreservesNonAsciiAndInvalidBytes) {
    const std::string chinese = "中文文本";
    EXPECT_EQ(mirador::normalize_text(chinese, static_cast<uint32_t>(TextNormalizeFlags::kFoldFullwidthAscii)),
              chinese);

    const std::string invalid = std::string("a") + char(0xFF) + char(0xFE) + "b";
    EXPECT_EQ(mirador::normalize_text(invalid, static_cast<uint32_t>(TextNormalizeFlags::kStripControl)), invalid);

    EXPECT_EQ(mirador::normalize_text("unchanged", 0), "unchanged");
}

}  // namespace
