#include <mirador/evidence.hpp>

#include <mirador/geometry.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>

namespace mirador {
namespace {

Status validate_bounds(const RectF& bounds) noexcept {
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height)) {
        return {ErrorCode::kInvalidArgument, "evidence bounds must be finite"};
    }
    if (bounds.width < 0.0F || bounds.height < 0.0F) {
        return {ErrorCode::kInvalidArgument, "evidence bounds size must be non-negative"};
    }
    return {};
}

Status validate_space(CoordinateSpaceId space) noexcept {
    if (space != CoordinateSpaceId::kFrame && space != CoordinateSpaceId::kOriented &&
        space != CoordinateSpaceId::kDisplay) {
        return {ErrorCode::kInvalidArgument, "evidence space must be kFrame, kOriented or kDisplay (DEC-016)"};
    }
    return {};
}

uint32_t source_mask(RegionSource origin, bool cached) noexcept {
    const uint32_t mask = static_cast<uint32_t>(origin) | (cached ? static_cast<uint32_t>(RegionSource::kCache) : 0U);
    return mask;
}

}  // namespace

const RectF& evidence_bounds(const EvidenceItem& item) noexcept {
    switch (item.kind) {
        case EvidenceKind::kExternal:
            return item.external.bounds;
        case EvidenceKind::kText:
            return item.text.bounds;
        case EvidenceKind::kDetection:
            return item.detection.bounds;
        case EvidenceKind::kTemplate:
            return item.template_bounds;
    }
    return item.external.bounds;
}

float evidence_confidence(const EvidenceItem& item) noexcept {
    switch (item.kind) {
        case EvidenceKind::kExternal:
            return item.external.confidence;
        case EvidenceKind::kText:
            return item.text.confidence;
        case EvidenceKind::kDetection:
            return item.detection.confidence;
        case EvidenceKind::kTemplate:
            return static_cast<float>(std::clamp(item.template_similarity, 0.0, 1.0));
    }
    return 0.0F;
}

std::string_view evidence_text_or_label(const EvidenceItem& item) noexcept {
    switch (item.kind) {
        case EvidenceKind::kExternal:
            return item.external.text;
        case EvidenceKind::kText:
            return item.text.utf8_text;
        case EvidenceKind::kDetection:
            return item.detection.label;
        case EvidenceKind::kTemplate:
            return {};
    }
    return {};
}

Result<uint64_t> EvidenceSet::add_external(const ExternalRegion& region, CoordinateSpaceId space) noexcept {
    if (const Status bounds = validate_bounds(region.bounds); !bounds.ok()) {
        return bounds;
    }
    if (const Status space_status = validate_space(space); !space_status.ok()) {
        return space_status;
    }
    EvidenceItem item;
    item.kind = EvidenceKind::kExternal;
    item.space = space;
    item.source = source_mask(RegionSource::kExternal, false);
    item.external = region;
    return push(std::move(item));
}

Result<uint64_t> EvidenceSet::add_text(const TextRegion& region, CoordinateSpaceId space, bool cached) noexcept {
    if (const Status bounds = validate_bounds(region.bounds); !bounds.ok()) {
        return bounds;
    }
    if (const Status space_status = validate_space(space); !space_status.ok()) {
        return space_status;
    }
    EvidenceItem item;
    item.kind = EvidenceKind::kText;
    item.space = space;
    item.source = source_mask(RegionSource::kOcr, cached);
    item.text = region;
    return push(std::move(item));
}

Result<uint64_t> EvidenceSet::add_detection(const DetectionRegion& region, CoordinateSpaceId space,
                                            bool cached) noexcept {
    if (const Status bounds = validate_bounds(region.bounds); !bounds.ok()) {
        return bounds;
    }
    if (const Status space_status = validate_space(space); !space_status.ok()) {
        return space_status;
    }
    EvidenceItem item;
    item.kind = EvidenceKind::kDetection;
    item.space = space;
    item.source = source_mask(RegionSource::kDetector, cached);
    item.detection = region;
    return push(std::move(item));
}

Result<uint64_t> EvidenceSet::add_template(uint64_t entry_id, const RectF& bounds, double similarity,
                                           CoordinateSpaceId space, bool cached) noexcept {
    if (const Status bounds_status = validate_bounds(bounds); !bounds_status.ok()) {
        return bounds_status;
    }
    if (const Status space_status = validate_space(space); !space_status.ok()) {
        return space_status;
    }
    if (!std::isfinite(similarity)) {
        return Status(ErrorCode::kInvalidArgument, "template similarity must be finite");
    }
    EvidenceItem item;
    item.kind = EvidenceKind::kTemplate;
    item.space = space;
    item.source = source_mask(RegionSource::kTemplate, cached);
    item.template_entry_id = entry_id;
    item.template_bounds = bounds;
    item.template_similarity = similarity;
    return push(std::move(item));
}

Result<uint64_t> EvidenceSet::push(EvidenceItem item) noexcept {
    try {
        if (items_.size() >= kMaxItems) {
            return Status(ErrorCode::kBudgetExceeded, "evidence set reached kMaxItems");
        }
        const uint64_t id = next_id_;
        ++next_id_;
        item.evidence_id = id;
        items_.push_back(std::move(item));
        return id;
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "evidence set allocation failed");
    }
}

}  // namespace mirador
