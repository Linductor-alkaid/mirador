#include <mirador/text_normalize.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mirador {
namespace {

/// Whitespace classification for trim/collapse: ASCII whitespace plus the
/// ideographic space U+3000 (which stays a fold target under
/// kFoldFullwidthAscii as well).
bool is_whitespace(uint32_t code_point) noexcept {
    return code_point == 0x20 || code_point == 0x09 || code_point == 0x0A || code_point == 0x0B || code_point == 0x0C ||
           code_point == 0x0D || code_point == 0x3000;
}

/// Decodes one UTF-8 sequence. Returns the code point and its byte length;
/// for an invalid sequence the lead byte is returned with length 1 so it
/// passes through unchanged.
struct DecodedSequence {
    uint32_t code_point = 0;
    uint8_t length = 1;
};

[[nodiscard]] DecodedSequence decode_utf8(std::string_view text, size_t offset) noexcept {
    const auto lead = static_cast<uint8_t>(text[offset]);
    if (lead < 0x80) {
        return {lead, 1};
    }
    uint8_t expected = 0;
    uint32_t code_point = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        expected = 2;
        code_point = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        expected = 3;
        code_point = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        expected = 4;
        code_point = lead & 0x07;
    } else {
        return {lead, 1};  // stray continuation or invalid lead
    }
    if (offset + expected > text.size()) {
        return {lead, 1};
    }
    for (uint8_t i = 1; i < expected; ++i) {
        const auto continuation = static_cast<uint8_t>(text[offset + i]);
        if ((continuation & 0xC0) != 0x80) {
            return {lead, 1};
        }
        code_point = (code_point << 6) | (continuation & 0x3F);
    }
    return {code_point, expected};
}

/// Applies the character-level transforms; `dropped` marks control removal.
bool transform_code_point(uint32_t code_point, uint32_t flags, uint32_t& out, bool& dropped) noexcept {
    dropped = false;
    out = code_point;
    if ((flags & static_cast<uint32_t>(TextNormalizeFlags::kFoldFullwidthAscii)) != 0) {
        if (code_point >= 0xFF01 && code_point <= 0xFF5E) {
            out = code_point - 0xFEE0;
        } else if (code_point == 0x3000) {
            out = 0x20;
        }
    }
    if ((flags & static_cast<uint32_t>(TextNormalizeFlags::kStripControl)) != 0) {
        const bool c0 = out < 0x20 && out != 0x09 && out != 0x0A && out != 0x0D;
        if (c0 || out == 0x7F || (out >= 0x80 && out <= 0x9F)) {
            dropped = true;
            return false;
        }
    }
    return true;
}

}  // namespace

Result<std::vector<TextRegion>> merge_text_lines(std::span<const TextRegion> lines, const LineMergeParams& params) {
    if (lines.size() > static_cast<size_t>(params.max_lines)) {
        return Status(ErrorCode::kBudgetExceeded, "too many lines for merging");
    }
    if (!(params.max_center_offset_ratio >= 0.0) || !std::isfinite(params.max_center_offset_ratio) ||
        !(params.max_gap_ratio >= 0.0) || !std::isfinite(params.max_gap_ratio)) {
        return Status(ErrorCode::kInvalidArgument, "merge ratios must be finite and non-negative");
    }
    for (const TextRegion& line : lines) {
        if (!std::isfinite(line.bounds.x) || !std::isfinite(line.bounds.y) || !std::isfinite(line.bounds.width) ||
            !std::isfinite(line.bounds.height) || !std::isfinite(line.confidence)) {
            return Status(ErrorCode::kInvalidArgument, "lines must carry finite bounds and confidence");
        }
    }

    // Deterministic processing order: top row, then left column.
    std::vector<size_t> order(lines.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [lines](size_t a, size_t b) {
        if (lines[a].bounds.y != lines[b].bounds.y) {
            return lines[a].bounds.y < lines[b].bounds.y;
        }
        return lines[a].bounds.x < lines[b].bounds.x;
    });

    const auto same_line = [params](const TextRegion& a, const TextRegion& b) {
        const double min_height = std::min<double>(a.bounds.height, b.bounds.height);
        if (min_height <= 0.0) {
            return false;
        }
        const double center_a = a.bounds.y + a.bounds.height / 2.0;
        const double center_b = b.bounds.y + b.bounds.height / 2.0;
        if (std::fabs(center_a - center_b) > params.max_center_offset_ratio * min_height) {
            return false;
        }
        const double left_edge = std::max<double>(a.bounds.x, b.bounds.x);
        const double right_edge = std::min<double>(a.bounds.x + a.bounds.width, b.bounds.x + b.bounds.width);
        const double gap = left_edge >= right_edge ? left_edge - right_edge : 0.0;  // overlap merges
        return gap <= params.max_gap_ratio * min_height;
    };

    std::vector<bool> consumed(lines.size(), false);
    std::vector<TextRegion> merged;
    for (const size_t seed : order) {
        if (consumed[seed]) {
            continue;
        }
        consumed[seed] = true;
        TextRegion current = lines[seed];
        bool grew = true;
        while (grew) {
            grew = false;
            for (const size_t candidate : order) {
                if (consumed[candidate]) {
                    continue;
                }
                if (!same_line(current, lines[candidate])) {
                    continue;
                }
                const TextRegion& other = lines[candidate];
                const float left = std::min(current.bounds.x, other.bounds.x);
                const float top = std::min(current.bounds.y, other.bounds.y);
                const float right =
                    std::max(current.bounds.x + current.bounds.width, other.bounds.x + other.bounds.width);
                const float bottom =
                    std::max(current.bounds.y + current.bounds.height, other.bounds.y + other.bounds.height);
                current.bounds = RectF{left, top, right - left, bottom - top};
                current.utf8_text += params.text_separator;
                current.utf8_text += other.utf8_text;
                current.confidence = std::min(current.confidence, other.confidence);
                current.polygon.clear();
                consumed[candidate] = true;
                grew = true;
            }
        }
        merged.push_back(std::move(current));
    }
    return merged;
}

std::string normalize_text(std::string_view text, uint32_t flags) {
    std::string out;
    out.reserve(text.size());
    size_t offset = 0;
    // Character pass: decode, transform or drop.
    while (offset < text.size()) {
        const DecodedSequence decoded = decode_utf8(text, offset);
        uint32_t transformed = 0;
        bool dropped = false;
        const bool keep = transform_code_point(decoded.code_point, flags, transformed, dropped);
        if (keep) {
            if (transformed == decoded.code_point) {
                out.append(text.substr(offset, decoded.length));  // original bytes
            } else {
                out.push_back(static_cast<char>(transformed));  // folds stay ASCII
            }
        }
        offset += decoded.length;
    }

    if ((flags & static_cast<uint32_t>(TextNormalizeFlags::kCollapseWhitespace)) != 0) {
        std::string collapsed;
        collapsed.reserve(out.size());
        bool in_whitespace = false;
        for (const char byte : out) {
            const bool whitespace =
                (static_cast<unsigned char>(byte) < 0x80) && is_whitespace(static_cast<uint8_t>(byte));
            // U+3000 was folded to a space when the fold flag is set; raw
            // multi-byte whitespace stays untouched by design (documented).
            if (whitespace) {
                in_whitespace = true;
                continue;
            }
            if (in_whitespace && !collapsed.empty()) {
                collapsed.push_back(' ');
            }
            in_whitespace = false;
            collapsed.push_back(byte);
        }
        out = std::move(collapsed);
    }

    if ((flags & static_cast<uint32_t>(TextNormalizeFlags::kTrim)) != 0) {
        const auto is_space = [](unsigned char byte) { return byte == 0x20 || (byte >= 0x09 && byte <= 0x0D); };
        size_t begin = 0;
        size_t end = out.size();
        while (begin < end && is_space(static_cast<unsigned char>(out[begin]))) {
            ++begin;
        }
        while (end > begin && is_space(static_cast<unsigned char>(out[end - 1]))) {
            --end;
        }
        out = out.substr(begin, end - begin);
    }
    return out;
}

}  // namespace mirador
