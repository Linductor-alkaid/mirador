#include <mirador/perception_session.hpp>

#include <mirador/backend_info.hpp>
#include <mirador/cache_policy.hpp>
#include <mirador/capability_cache.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/color_convert.hpp>
#include <mirador/crop.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/evidence.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/fingerprint.hpp>
#include <mirador/frame.hpp>
#include <mirador/frame_cache.hpp>
#include <mirador/fusion.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/resize.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/stable_id_tracker.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace mirador {
namespace {

// Stage-boundary cancellation/deadline check shared by every session method.
Status context_status(const ExecutionContext& context) noexcept {
    if (is_cancelled(context)) {
        return {ErrorCode::kCancelled, "session operation cancelled"};
    }
    if (deadline_reached(context)) {
        return {ErrorCode::kTimeout, "session operation deadline exceeded"};
    }
    return {};
}

Status check_frame(const std::string& source_id, const Frame& frame) noexcept {
    if (const Result<void> valid = validate(frame.image); !valid.ok()) {
        return valid.status();
    }
    if (!frame.source_id.empty() && frame.source_id != source_id) {
        return {ErrorCode::kInvalidArgument, "frame source does not belong to this session"};
    }
    return {};
}

/// Raw capture dimensions behind an oriented view (inverse of oriented_size).
std::pair<int32_t, int32_t> raw_dimensions(Rotation rotation, int32_t oriented_width,
                                           int32_t oriented_height) noexcept {
    if (rotation == Rotation::k90 || rotation == Rotation::k270) {
        return {oriented_height, oriented_width};
    }
    return {oriented_width, oriented_height};
}

/// Enclosing integer rect of a finite RectF (floor on the leading edge, ceil
/// on the trailing edge). Callers validate finiteness first.
RectI enclosing_rect(const RectF& rect) noexcept {
    const auto x0 = static_cast<int32_t>(std::floor(rect.x));
    const auto y0 = static_cast<int32_t>(std::floor(rect.y));
    const auto x1 = static_cast<int32_t>(std::ceil(rect.x + rect.width));
    const auto y1 = static_cast<int32_t>(std::ceil(rect.y + rect.height));
    return RectI{x0, y0, x1 - x0, y1 - y0};
}

/// Frame-to-oriented-view rotation transform of one view.
Transform2D frame_to_oriented(const ImageView& oriented) noexcept {
    const auto [raw_width, raw_height] = raw_dimensions(oriented.rotation, oriented.width, oriented.height);
    return make_rotation(oriented.rotation, raw_width, raw_height, CoordinateSpaceId::kFrame,
                         CoordinateSpaceId::kOriented);
}

/// One stage of the prepared-image chain, kept individually so coordinate
/// recovery can invert the chain up to any recorded space (DEC-012). The
/// buffer handed to the backend always counts as kModelInput space.
struct PreparedImage {
    ImageBuffer buffer;            ///< owning pixels handed to the backend
    Transform2D cropped_to_here;   ///< chain from the crop space to the buffer space
    Transform2D oriented_to_here;  ///< chain from the oriented view to the buffer space
    Transform2D frame_to_here;     ///< chain from the raw frame (rotation prefix)
};

/// Deterministic preprocessing chain (kPreprocessPipelineVersion): convert the
/// cropped region to the first backend-accepted format the converter can reach,
/// then downscale so the long edge respects `max_side`. Input is the packed
/// crop produced by the caller; every new allocation is bounded by twice the
/// frame byte size plus slack (RULE-06); scaling never enlarges pixels.
Result<PreparedImage> prepare_chain(ImageBuffer cropped, const RectI& roi, int32_t max_side,
                                    const BackendInfo& backend_info, const ImageView& oriented) {
    const int64_t frame_bytes =
        static_cast<int64_t>(oriented.width) * oriented.height * bytes_per_pixel(oriented.format);
    const int64_t work_budget = 2 * frame_bytes + int64_t{64} * 1024;

    PreparedImage prepared;
    prepared.cropped_to_here = make_identity(CoordinateSpaceId::kCropped, CoordinateSpaceId::kModelInput);
    prepared.oriented_to_here = make_crop(RectF{static_cast<float>(roi.x), static_cast<float>(roi.y),
                                                static_cast<float>(roi.width), static_cast<float>(roi.height)},
                                          CoordinateSpaceId::kOriented, CoordinateSpaceId::kCropped);
    const Result<Transform2D> frame_chain = compose(frame_to_oriented(oriented), prepared.oriented_to_here);
    if (!frame_chain.ok()) {
        return frame_chain.status();
    }
    prepared.frame_to_here = frame_chain.value();

    // Format selection (DEC-012): keep the crop's format when accepted;
    // otherwise take the first accepted format the converter can reach, in the
    // backend's declaration order.
    bool needs_conversion = true;
    for (const PixelFormat accepted : backend_info.accepted_formats) {
        if (accepted == cropped.format()) {
            needs_conversion = false;
            break;
        }
    }
    if (needs_conversion) {
        Status last_error(ErrorCode::kUnsupportedFormat, "no backend-accepted format reachable by convert_color");
        bool converted = false;
        for (const PixelFormat accepted : backend_info.accepted_formats) {
            Result<ImageBuffer> converted_buffer = convert_color(cropped.view(), accepted, work_budget);
            if (converted_buffer.ok()) {
                cropped = converted_buffer.take_value();
                converted = true;
                break;
            }
            last_error = converted_buffer.status();
        }
        if (!converted) {
            return last_error;
        }
    }
    prepared.buffer = std::move(cropped);

    // Long-edge cap: floor-scaled dimensions, at least one pixel; the chain
    // gains a scale step only when pixels actually shrink.
    if (max_side > 0) {
        const int32_t width = prepared.buffer.width();
        const int32_t height = prepared.buffer.height();
        const int32_t long_edge = std::max(width, height);
        if (long_edge > max_side) {
            const auto dst_width =
                std::max<int32_t>(1, static_cast<int32_t>((static_cast<int64_t>(width) * max_side) / long_edge));
            const auto dst_height =
                std::max<int32_t>(1, static_cast<int32_t>((static_cast<int64_t>(height) * max_side) / long_edge));
            Result<ImageBuffer> resized = resize_area(prepared.buffer.view(), dst_width, dst_height, work_budget);
            if (!resized.ok()) {
                return resized.status();
            }
            prepared.cropped_to_here =
                make_scale(static_cast<double>(dst_width) / width, static_cast<double>(dst_height) / height,
                           CoordinateSpaceId::kCropped, CoordinateSpaceId::kModelInput);
            const Result<Transform2D> oriented_chain = compose(prepared.oriented_to_here, prepared.cropped_to_here);
            if (!oriented_chain.ok()) {
                return oriented_chain.status();
            }
            const Result<Transform2D> raw_chain = compose(prepared.frame_to_here, prepared.cropped_to_here);
            if (!raw_chain.ok()) {
                return raw_chain.status();
            }
            prepared.buffer = resized.take_value();
            prepared.oriented_to_here = oriented_chain.value();
            prepared.frame_to_here = raw_chain.value();
        }
    }
    return prepared;
}

TextRegion recovered_region(const TextRegion& region, const Transform2D& recovery) {
    TextRegion recovered;
    recovered.bounds = transform_rect(recovery, region.bounds);
    recovered.utf8_text = region.utf8_text;
    recovered.confidence = region.confidence;
    recovered.polygon = transform_points(recovery, region.polygon);
    return recovered;
}

DetectionRegion recovered_region(const DetectionRegion& region, const Transform2D& recovery) noexcept {
    DetectionRegion recovered;
    recovered.bounds = transform_rect(recovery, region.bounds);
    recovered.class_id = region.class_id;
    recovered.label = region.label;
    recovered.confidence = region.confidence;
    return recovered;
}

/// Coordinate recovery from the prepared (model-input) space into the
/// requested output space: the inverse of the recorded chain portion
/// (DEC-012). kCropped uses the crop-space chain, kOriented the view chain,
/// kFrame the full raw-frame chain.
Result<Transform2D> recovery_to(const PreparedImage& prepared, CoordinateSpaceId output_space) noexcept {
    switch (output_space) {
        case CoordinateSpaceId::kCropped:
            return inverse(prepared.cropped_to_here);
        case CoordinateSpaceId::kOriented:
            return inverse(prepared.oriented_to_here);
        case CoordinateSpaceId::kFrame:
            return inverse(prepared.frame_to_here);
        default:
            return Status(ErrorCode::kInvalidArgument, "output_space is outside the M2 recovery contract");
    }
}

/// Request validation shared by both capabilities (DEC-012 field contract).
Status validate_request(const std::optional<RectF>& roi, CoordinateSpaceId roi_space, int32_t max_side,
                        CoordinateSpaceId output_space) noexcept {
    if (max_side < 0) {
        return {ErrorCode::kInvalidArgument, "max_side must be non-negative"};
    }
    if (roi_space != CoordinateSpaceId::kOriented && roi_space != CoordinateSpaceId::kFrame) {
        return {ErrorCode::kInvalidArgument, "roi_space must be kOriented or kFrame"};
    }
    if (output_space != CoordinateSpaceId::kOriented && output_space != CoordinateSpaceId::kFrame &&
        output_space != CoordinateSpaceId::kCropped) {
        return {ErrorCode::kInvalidArgument, "output_space must be kFrame, kOriented or kCropped"};
    }
    if (roi.has_value()) {
        const RectF& rect = *roi;
        if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
            !std::isfinite(rect.height)) {
            return {ErrorCode::kInvalidArgument, "roi must be finite"};
        }
        if (rect.width < 0.0F || rect.height < 0.0F) {
            return {ErrorCode::kInvalidArgument, "roi dimensions must be non-negative"};
        }
    }
    return {};
}

/// Resolves the executed pixel ROI in the oriented view from the request ROI.
Result<RectI> executed_roi(const ImageView& oriented, const std::optional<RectF>& roi,
                           CoordinateSpaceId roi_space) noexcept {
    RectF oriented_roi;
    if (!roi.has_value()) {
        oriented_roi = RectF{0.0F, 0.0F, static_cast<float>(oriented.width), static_cast<float>(oriented.height)};
    } else if (roi_space == CoordinateSpaceId::kOriented) {
        oriented_roi = *roi;
    } else {
        oriented_roi = transform_rect(frame_to_oriented(oriented), *roi);
    }

    const RectI requested = enclosing_rect(oriented_roi);
    if (!is_valid(requested)) {
        return Status(ErrorCode::kInvalidArgument, "roi is not a representable rectangle");
    }
    const RectI clipped = intersect(requested, RectI{0, 0, oriented.width, oriented.height});
    if (clipped.width <= 0 || clipped.height <= 0) {
        return Status(ErrorCode::kInvalidArgument, "roi lies outside the frame");
    }
    return clipped;
}

/// Static description of one capability for the shared run engine (DEC-012).
template <typename Derived>
struct CapabilityTraits;

struct OcrTraits {
    using Region = TextRegion;
    using Backend = OcrBackend;
    using Request = OcrRequest;
    static constexpr CapabilityKind kKind = CapabilityKind::kOcr;
    static const char* kName() noexcept { return "run_ocr"; }
    static BackendInfo info(const Backend* backend) { return backend->info(); }
    static Result<std::vector<Region>> execute(Backend* backend, const ImageView& view, const Request& request,
                                               const ExecutionContext& context) {
        return backend->recognize(view, request, context);
    }
    static uint64_t params_digest(const Request& request) noexcept { return ocr_request_params_digest(request); }
    static const std::vector<Region>& regions(const CachedCapabilityResult& cached) noexcept {
        return cached.text_regions;
    }
    static void set_regions(CachedCapabilityResult& cached, std::vector<Region> regions) noexcept {
        cached.text_regions = std::move(regions);
    }
};

struct DetectionTraits {
    using Region = DetectionRegion;
    using Backend = DetectorBackend;
    using Request = DetectionRequest;
    static constexpr CapabilityKind kKind = CapabilityKind::kDetection;
    static const char* kName() noexcept { return "run_detector"; }
    static BackendInfo info(const Backend* backend) { return backend->info(); }
    static Result<std::vector<Region>> execute(Backend* backend, const ImageView& view, const Request& request,
                                               const ExecutionContext& context) {
        return backend->detect(view, request, context);
    }
    static uint64_t params_digest(const Request& request) noexcept { return detection_request_params_digest(request); }
    static const std::vector<Region>& regions(const CachedCapabilityResult& cached) noexcept {
        return cached.detection_regions;
    }
    static void set_regions(CachedCapabilityResult& cached, std::vector<Region> regions) noexcept {
        cached.detection_regions = std::move(regions);
    }
};

/// Shared engine: preflight, cache-aware keying, preprocessing, backend
/// execution and coordinate recovery (design sections 10 and 12, DEC-012).
/// Not noexcept: callers wrap it in try/catch and map allocation failure to
/// kBudgetExceeded.
template <typename Traits>
Result<std::vector<typename Traits::Region>> run_capability(PerceptionSession& session, const Frame& frame,
                                                            typename Traits::Backend* backend,
                                                            const typename Traits::Request& request,
                                                            const ExecutionContext& context) {
    if (const Status stage = context_status(context); !stage.ok()) {
        return stage;
    }
    if (backend == nullptr) {
        return Status(ErrorCode::kBackendUnavailable, "no backend provided");
    }
    if (const Status frame_status = check_frame(session.source_id(), frame); !frame_status.ok()) {
        return frame_status;
    }
    if (const Status request_status =
            validate_request(request.roi, request.roi_space, request.max_side, request.output_space);
        !request_status.ok()) {
        return request_status;
    }
    const BackendInfo info = Traits::info(backend);
    if (const Result<void> info_status = validate(info); !info_status.ok()) {
        return info_status.status();
    }

    const ImageView& oriented = frame.image;
    const Result<RectI> roi = executed_roi(oriented, request.roi, request.roi_space);
    if (!roi.ok()) {
        return roi.status();
    }

    // Content fingerprint of the executed region at source resolution: the
    // crop is taken once and reused by the preprocessing chain.
    const int64_t frame_bytes =
        static_cast<int64_t>(oriented.width) * oriented.height * bytes_per_pixel(oriented.format);
    const int64_t work_budget = 2 * frame_bytes + int64_t{64} * 1024;
    Result<ImageBuffer> cropped = crop(oriented, roi.value(), work_budget);
    if (!cropped.ok()) {
        return cropped.status();
    }
    ImageBuffer cropped_buffer = cropped.take_value();
    const Result<uint64_t> region_fingerprint = fingerprint(cropped_buffer.view());
    if (!region_fingerprint.ok()) {
        return region_fingerprint.status();
    }

    CapabilityKeyFields fields;
    fields.image_fingerprint = region_fingerprint.value();
    fields.source_id = session.source_id();
    fields.roi = roi.value();
    fields.preprocessing_version = kPreprocessPipelineVersion;
    fields.kind = Traits::kKind;
    fields.output_space = request.output_space;
    fields.backend_name = info.name;
    fields.implementation_version = info.implementation_version;
    fields.model_id = info.model_id;
    fields.model_revision = info.model_revision;
    fields.request_params_digest = Traits::params_digest(request);
    const CacheKeyDigest key = capability_cache_digest(fields);

    // Cache read (kRefresh bypasses; kReadOnly reads without storing).
    if (request.cache_policy != CachePolicy::kRefresh) {
        const Result<CapabilityPayload> cached = session.result_cache().lookup(key);
        if (!cached.ok()) {
            return cached.status();
        }
        if (cached.value() != nullptr) {
            return Traits::regions(*cached.value());
        }
    }

    const Result<PreparedImage> prepared =
        prepare_chain(std::move(cropped_buffer), roi.value(), request.max_side, info, oriented);
    if (!prepared.ok()) {
        return prepared.status();
    }

    if (const Status stage = context_status(context); !stage.ok()) {
        return stage;
    }
    const Result<std::vector<typename Traits::Region>> executed =
        Traits::execute(backend, prepared.value().buffer.view(), request, context);
    if (!executed.ok()) {
        return executed.status();
    }

    const Result<Transform2D> recovery = recovery_to(prepared.value(), request.output_space);
    if (!recovery.ok()) {
        return recovery.status();
    }
    std::vector<typename Traits::Region> recovered_regions;
    recovered_regions.reserve(executed.value().size());
    for (const typename Traits::Region& region : executed.value()) {
        recovered_regions.push_back(recovered_region(region, recovery.value()));
    }

    // Cache write (kReadOnly skips). Insert failures are surfaced explicitly:
    // a result too large for the budget must not vanish silently (RULE-06).
    if (request.cache_policy != CachePolicy::kReadOnly) {
        CachedCapabilityResult stored;
        stored.kind = Traits::kKind;
        stored.output_space = request.output_space;
        Traits::set_regions(stored, recovered_regions);
        if (const Result<void> inserted = session.result_cache().insert(key, stored); !inserted.ok()) {
            return inserted.status();
        }
    }
    return recovered_regions;
}

}  // namespace

PerceptionSession::PerceptionSession(PerceptionSessionOptions options, FrameCache frame_cache,
                                     CapabilityResultCache result_cache) noexcept
    : options_(std::move(options)), frame_cache_(std::move(frame_cache)), result_cache_(std::move(result_cache)) {}

Result<PerceptionSession> PerceptionSession::create(PerceptionSessionOptions options) noexcept {
    if (options.frame_cache_bytes <= 0 || options.result_cache_bytes <= 0) {
        return Status(ErrorCode::kInvalidArgument, "session cache budgets must be positive");
    }
    Result<FrameCache> frame_cache = FrameCache::create(options.frame_cache_bytes);
    if (!frame_cache.ok()) {
        return frame_cache.status();
    }
    Result<CapabilityResultCache> result_cache = CapabilityResultCache::create(options.result_cache_bytes);
    if (!result_cache.ok()) {
        return result_cache.status();
    }
    return PerceptionSession(std::move(options), frame_cache.take_value(), result_cache.take_value());
}

Result<ChangeReport> PerceptionSession::analyze_change(const Frame& frame, const ChangeDetectionParams& params,
                                                       const ExecutionContext& context) noexcept {
    try {
        if (const Status stage = context_status(context); !stage.ok()) {
            return stage;
        }
        if (const Status frame_status = check_frame(options_.source_id, frame); !frame_status.ok()) {
            return frame_status;
        }
        Result<ChangeSignature> signature = make_change_signature(frame.image, params);
        if (!signature.ok()) {
            return signature.status();
        }

        if (!previous_signature_.has_value()) {
            const uint64_t first_fingerprint = signature.value().fingerprint;
            previous_signature_ = signature.take_value();
            ChangeReport report;
            report.classification = ChangeClassification::kGlobal;
            report.reason = ChangeReason::kFirstFrame;
            report.frame_similarity = 1.0;
            report.previous_fingerprint = 0;
            report.current_fingerprint = first_fingerprint;
            report.thresholds = ChangeThresholds{params.fingerprint_similarity_threshold, params.block_diff_threshold,
                                                 params.global_area_ratio};
            last_change_ = report;
            return report;
        }

        const Result<ChangeReport> report = detect_change(*previous_signature_, signature.value(), params);
        if (report.ok()) {
            previous_signature_ = signature.take_value();
            last_change_ = report.value();
        }
        return report;
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "analyze_change: internal allocation failed");
    }
}

Result<std::vector<TextRegion>> PerceptionSession::run_ocr(const Frame& frame, OcrBackend* backend,
                                                           const OcrRequest& request,
                                                           const ExecutionContext& context) noexcept {
    try {
        return run_capability<OcrTraits>(*this, frame, backend, request, context);
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "run_ocr: internal allocation failed");
    }
}

Result<std::vector<DetectionRegion>> PerceptionSession::run_detector(const Frame& frame, DetectorBackend* backend,
                                                                     const DetectionRequest& request,
                                                                     const ExecutionContext& context) noexcept {
    try {
        return run_capability<DetectionTraits>(*this, frame, backend, request, context);
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "run_detector: internal allocation failed");
    }
}

Result<SemanticSnapshot> PerceptionSession::fuse(const Frame& frame, const EvidenceSet& evidence,
                                                 const FusionOptions& options,
                                                 const ExecutionContext& context) noexcept {
    try {
        if (const Status stage = context_status(context); !stage.ok()) {
            return stage;
        }
        if (const Status frame_status = check_frame(options_.source_id, frame); !frame_status.ok()) {
            return frame_status;
        }
        const Result<FusionOutput> fused = fuse_evidence(evidence, frame, options, context);
        if (!fused.ok()) {
            return fused.status();
        }
        const Result<StableIdReport> ids = tracker_.advance(fused.value().regions, options.stable_id, context);
        if (!ids.ok()) {
            return ids.status();
        }

        SemanticSnapshot snapshot;
        snapshot.frame_sequence = frame.sequence;
        snapshot.coordinate_space = options.target_space;
        snapshot.regions = std::move(fused.value().regions);
        for (size_t index = 0; index < snapshot.regions.size(); ++index) {
            snapshot.regions[index].stable_id = ids.value().assignments[index].stable_id;
        }
        snapshot.change = last_change_;

        // Generation policy (DEC-010): first publication is generation 1; a
        // later fuse keeps the generation unless the tracker bumped.
        if (generation_ == 0) {
            generation_ = 1;
        } else if (ids.value().generation_bump) {
            ++generation_;
        }
        snapshot.generation = generation_;

        latest_snapshot_ = std::make_shared<const SemanticSnapshot>(snapshot);
        return snapshot;
    } catch (...) {  // NOLINT(bugprone-catching-exceptions): allocation failure is a budget error
        return Status(ErrorCode::kBudgetExceeded, "fuse: internal allocation failed");
    }
}

}  // namespace mirador
