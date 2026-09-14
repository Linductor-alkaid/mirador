#ifndef MIRADOR_TEXT_NORMALIZE_HPP
#define MIRADOR_TEXT_NORMALIZE_HPP

#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mirador {

/// Merges OCR line boxes that belong to the same visual line (design section
/// 13, DEC-014 reference adaptation): two lines merge when their vertical
/// centers differ by at most `max_center_offset_ratio * min(height)` and their
/// horizontal gap (negative when they overlap) is at most
/// `max_gap_ratio * min(height)`. Merging is transitive until fixpoint. Merged
/// lines carry the union bounds, the concatenated text (left to right, joined
/// with `text_separator`) and the minimum confidence; `polygon` is dropped
/// (empty) in the merged result. Output is ordered by (bounds.y, bounds.x);
/// the input is never modified (RULE-04).
///
/// Errors: kInvalidArgument for negative ratios, NaN/inf bounds or
/// confidences; kBudgetExceeded when `lines` exceeds `max_lines`. Never
/// throws.
struct LineMergeParams {
    double max_center_offset_ratio = 0.5;
    double max_gap_ratio = 1.0;
    int32_t max_lines = 4096;
    std::string text_separator;  ///< e.g. "" for CJK, " " for spaced scripts
};

[[nodiscard]] Result<std::vector<TextRegion>> merge_text_lines(std::span<const TextRegion> lines,
                                                               const LineMergeParams& params);

/// Whitespace and control-character normalization of OCR text (design section
/// 13, DEC-014 reference adaptation). UTF-8 aware: multi-byte sequences are
/// decoded for classification and invalid bytes pass through unchanged;
/// non-transformed code points keep their original bytes. Unicode NFC/NFKC is
/// explicitly out of scope.
enum class TextNormalizeFlags : uint32_t {
    kNone = 0,
    /// Strip leading/trailing ASCII whitespace. Fold U+3000 first with
    /// kFoldFullwidthAscii when ideographic spaces matter.
    kTrim = 1u << 0,
    /// Fold internal ASCII whitespace runs to a single space.
    kCollapseWhitespace = 1u << 1,
    /// Remove C0 control characters except \t, \n, \r, plus DEL and C1.
    kStripControl = 1u << 2,
    /// Fold fullwidth ASCII forms U+FF01..FF5E to U+0021..U+007E and
    /// U+3000 (ideographic space) to a plain space.
    kFoldFullwidthAscii = 1u << 3,
};

[[nodiscard]] std::string normalize_text(std::string_view text, uint32_t flags);

}  // namespace mirador

#endif  // MIRADOR_TEXT_NORMALIZE_HPP
