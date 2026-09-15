#ifndef MIRADOR_EVIDENCE_HPP
#define MIRADOR_EVIDENCE_HPP

#include <mirador/detector_backend.hpp>
#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/transform.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mirador {

/// Caller-provided structured region (design section 8, RULE-11): the property
/// bag carries platform semantics (`interactive`, `role`, `enabled`) that
/// vision must never infer by itself. Bounds live in `space` coordinates.
struct ExternalRegion {
    RectF bounds;
    std::string text;         ///< accessible name/label; empty when none
    std::string role;         ///< platform role (e.g. "button"); never vision-inferred
    std::string description;  ///< caller-provided context, passed through
    float confidence = 1.0F;  ///< platform facts are certain by default
    bool interactive = false;
    bool enabled = true;

    /// Component equality (test convenience).
    [[nodiscard]] friend bool operator==(const ExternalRegion& lhs, const ExternalRegion& rhs) noexcept {
        return lhs.bounds == rhs.bounds && lhs.text == rhs.text && lhs.role == rhs.role &&
               lhs.description == rhs.description && lhs.confidence == rhs.confidence &&
               lhs.interactive == rhs.interactive && lhs.enabled == rhs.enabled;
    }
};

/// Kind tag of one evidence item (design section 16 input taxonomy).
enum class EvidenceKind : uint8_t {
    kExternal,
    kText,
    kDetection,
    kTemplate,
};

/// One piece of region evidence inside an `EvidenceSet`. Items are stored
/// id-ordered; only the payload selected by `kind` is meaningful. The unused
/// payloads stay default-constructed — bounded overhead per item, traded for
/// a simple value type (M4 budget note in the module README). Pure aggregate:
/// accessors are the free functions below (repo convention, M3 lint).
struct EvidenceItem {
    uint64_t evidence_id = 0;
    EvidenceKind kind = EvidenceKind::kExternal;
    CoordinateSpaceId space = CoordinateSpaceId::kOriented;
    /// RegionSource bit of this item (single bit: kExternal/kOcr/kDetector/
    /// kTemplate), additionally `kCache` when replayed from a cache.
    uint32_t source = 0;
    ExternalRegion external;
    TextRegion text;
    DetectionRegion detection;
    /// Template evidence (visual-index candidate): matched entry, its query
    /// patch location in `space` coordinates and the match similarity.
    uint64_t template_entry_id = 0;
    RectF template_bounds;
    double template_similarity = 0.0;

    /// Component equality (test convenience).
    [[nodiscard]] friend bool operator==(const EvidenceItem& lhs, const EvidenceItem& rhs) noexcept {
        return lhs.evidence_id == rhs.evidence_id && lhs.kind == rhs.kind && lhs.space == rhs.space &&
               lhs.source == rhs.source && lhs.external == rhs.external && lhs.text == rhs.text &&
               lhs.detection == rhs.detection && lhs.template_entry_id == rhs.template_entry_id &&
               lhs.template_bounds == rhs.template_bounds && lhs.template_similarity == rhs.template_similarity;
    }
};

/// Geometry of `item` regardless of kind.
[[nodiscard]] const RectF& evidence_bounds(const EvidenceItem& item) noexcept;
/// Best-effort confidence: item confidence, template similarity clamped to
/// [0, 1].
[[nodiscard]] float evidence_confidence(const EvidenceItem& item) noexcept;
/// Text payload: external/text text, detection label, empty for template.
[[nodiscard]] std::string_view evidence_text_or_label(const EvidenceItem& item) noexcept;

/// Ordered collection of region evidence feeding one fusion (design sections
/// 16 and 25). Evidence ids are assigned sequentially from 1 in add order, so
/// output ordering and traces are fully deterministic. Items may live in
/// kFrame, kOriented or kDisplay space; kDisplay items require the fusion
/// call to carry `FusionOptions::display_transform` (DEC-016) — the set
/// itself is a pure container and does not check that pairing. Bounded
/// (RULE-06): at
/// most `kMaxItems` items; further adds fail with kBudgetExceeded. Never
/// throws.
class EvidenceSet {
public:
    static constexpr size_t kMaxItems = 4096;

    EvidenceSet() noexcept = default;

    /// Adds one external region; returns its evidence id. Errors:
    /// kInvalidArgument for a non-finite/negative-size bounds, kBudgetExceeded
    /// when the set is full. Never throws.
    [[nodiscard]] Result<uint64_t> add_external(const ExternalRegion& region,
                                                CoordinateSpaceId space = CoordinateSpaceId::kOriented) noexcept;
    /// Adds one OCR text region. `cached` marks evidence replayed from the
    /// capability cache (source mask gains kCache).
    [[nodiscard]] Result<uint64_t> add_text(const TextRegion& region,
                                            CoordinateSpaceId space = CoordinateSpaceId::kOriented,
                                            bool cached = false) noexcept;
    /// Adds one detection region (same contract as `add_text`).
    [[nodiscard]] Result<uint64_t> add_detection(const DetectionRegion& region,
                                                 CoordinateSpaceId space = CoordinateSpaceId::kOriented,
                                                 bool cached = false) noexcept;
    /// Adds one visual-index template candidate: matched `entry_id` found at
    /// `bounds` with `similarity` (clamped into [0, 1] for confidence).
    [[nodiscard]] Result<uint64_t> add_template(uint64_t entry_id, const RectF& bounds, double similarity,
                                                CoordinateSpaceId space = CoordinateSpaceId::kOriented,
                                                bool cached = false) noexcept;

    /// Items in ascending evidence-id order.
    [[nodiscard]] const std::vector<EvidenceItem>& items() const noexcept { return items_; }
    [[nodiscard]] size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

private:
    [[nodiscard]] Result<uint64_t> push(EvidenceItem item) noexcept;

    std::vector<EvidenceItem> items_;
    uint64_t next_id_ = 1;
};

}  // namespace mirador

#endif  // MIRADOR_EVIDENCE_HPP
