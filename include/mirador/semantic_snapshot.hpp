#ifndef MIRADOR_SEMANTIC_SNAPSHOT_HPP
#define MIRADOR_SEMANTIC_SNAPSHOT_HPP

#include <mirador/change_detection.hpp>
#include <mirador/geometry.hpp>
#include <mirador/transform.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mirador {

/// Provenance bitmask of a fused region (design section 8). Bits compose: a
/// region merged from an Accessibility box and OCR text reports
/// kExternal | kOcr, and a region whose evidence was served from the
/// capability cache additionally reports kCache.
enum class RegionSource : uint32_t {
    kNone = 0,
    kExternal = 1u << 0,   ///< caller-provided structured region (e.g. Accessibility)
    kOcr = 1u << 1,        ///< OCR text evidence
    kDetector = 1u << 2,   ///< detector/UI-proposer evidence
    kCache = 1u << 3,      ///< evidence replayed from the capability cache
    kTemplate = 1u << 4,   ///< visual-index template-match evidence
};

[[nodiscard]] constexpr uint32_t operator|(RegionSource lhs, RegionSource rhs) noexcept {
    return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}
[[nodiscard]] constexpr uint32_t operator|(uint32_t lhs, RegionSource rhs) noexcept {
    return lhs | static_cast<uint32_t>(rhs);
}
[[nodiscard]] constexpr uint32_t operator|(RegionSource lhs, uint32_t rhs) noexcept {
    return static_cast<uint32_t>(lhs) | rhs;
}
[[nodiscard]] constexpr uint32_t operator&(RegionSource lhs, RegionSource rhs) noexcept {
    return static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs);
}
[[nodiscard]] constexpr uint32_t operator&(uint32_t lhs, RegionSource rhs) noexcept {
    return lhs & static_cast<uint32_t>(rhs);
}
[[nodiscard]] constexpr bool has_source(uint32_t mask, RegionSource source) noexcept {
    return (mask & static_cast<uint32_t>(source)) != 0;
}

/// One fused semantic region (design section 8). `stable_id` is unique within
/// the producing tracking session only (RULE-09); `evidence_ids` lists the
/// contributing `EvidenceSet` items in ascending order; the anchor is the
/// union-bounds center and stays a pure visual fact — platform semantics such
/// as `clickable` are never inferred here (RULE-11).
struct VisualRegion {
    uint64_t stable_id = 0;
    RectF bounds;
    PointF anchor;
    std::string text;        ///< concatenated text evidence, "\n"-joined in evidence order
    std::string label;       ///< detector label or external role when available
    std::string description; ///< caller/backends may attach prose; fusion leaves it empty
    uint32_t source_mask = 0;
    float confidence = 0.0F;
    std::vector<uint64_t> evidence_ids;

    /// Component equality (test convenience).
    [[nodiscard]] friend bool operator==(const VisualRegion& lhs, const VisualRegion& rhs) noexcept {
        return lhs.stable_id == rhs.stable_id && lhs.bounds == rhs.bounds && lhs.anchor == rhs.anchor &&
               lhs.text == rhs.text && lhs.label == rhs.label && lhs.description == rhs.description &&
               lhs.source_mask == rhs.source_mask && lhs.confidence == rhs.confidence &&
               lhs.evidence_ids == rhs.evidence_ids;
    }
};

/// Immutable fusion output for one frame (design section 8): fused regions in
/// one coordinate space, the snapshot generation and the change decision the
/// session observed. Published snapshots are never mutated; a new fusion
/// produces a new snapshot object. Consumers must carry `generation` with any
/// region reference and reject stale ones (RULE-09, design section 16).
struct SemanticSnapshot {
    uint64_t frame_sequence = 0;
    uint64_t generation = 0;
    CoordinateSpaceId coordinate_space = CoordinateSpaceId::kOriented;
    std::vector<VisualRegion> regions;
    ChangeReport change;

    /// Component equality (test convenience).
    [[nodiscard]] friend bool operator==(const SemanticSnapshot& lhs, const SemanticSnapshot& rhs) noexcept {
        return lhs.frame_sequence == rhs.frame_sequence && lhs.generation == rhs.generation &&
               lhs.coordinate_space == rhs.coordinate_space && lhs.regions == rhs.regions &&
               lhs.change.classification == rhs.change.classification && lhs.change.reason == rhs.change.reason;
    }
};

/// Returns the region with `stable_id`, or nullptr when absent. Linear scan;
/// snapshots are small by budget.
[[nodiscard]] const VisualRegion* find_region(const SemanticSnapshot& snapshot, uint64_t stable_id) noexcept;

/// True when `generation` matches the snapshot generation, i.e. references
/// taken from this snapshot are still current (design section 16). A snapshot
/// is its own generation, so passing `snapshot.generation` is always true.
[[nodiscard]] bool is_generation_current(const SemanticSnapshot& snapshot, uint64_t generation) noexcept;

}  // namespace mirador

#endif  // MIRADOR_SEMANTIC_SNAPSHOT_HPP
