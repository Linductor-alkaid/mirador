#include "nanotrack_backend.hpp"

#include "ncnn_runtime.hpp"

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>
#include <mirador/tracker_backend.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace mirador::integrations {
namespace {

using mirador::BackendInfo;
using mirador::ExecutionContext;
using mirador::ImageView;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::Result;
using mirador::Status;
using mirador::TrackerInitRequest;
using mirador::TrackerSession;
using mirador::TrackerUpdateRequest;
using mirador::TrackerUpdateResult;

/// Frozen blob names of the model contract (see the class comment): the
/// reviewed port spells them exactly like this in its param files.
constexpr const char* kBackboneInput = "input";
constexpr const char* kBackboneOutput = "output";
constexpr const char* kHeadTemplateInput = "input1";
constexpr const char* kHeadSearchInput = "input2";
constexpr const char* kHeadClsOutput = "output1";
constexpr const char* kHeadBoxOutput = "output2";

/// One initialize/update call's byte budget (RULE-06): every internal
/// allocation request is charged before it happens (or, for tensors that only
/// materialize inside a forward, checked the moment they are handed back); an
/// overflow is a loud kBudgetExceeded, never a silent drop.
class WorkBudget {
public:
    explicit WorkBudget(int64_t limit) noexcept : remaining_(limit) {}

    [[nodiscard]] Result<void> charge(int64_t bytes) noexcept {
        if (bytes < 0 || bytes > remaining_) {
            return Status{mirador::ErrorCode::kBudgetExceeded, "NanoTrack work budget exhausted (" +
                                                                   std::to_string(bytes) + " bytes requested, " +
                                                                   std::to_string(remaining_) + " left)"};
        }
        remaining_ -= bytes;
        return {};
    }

    [[nodiscard]] int64_t remaining() const noexcept { return remaining_; }

private:
    int64_t remaining_;
};

int64_t tensor_bytes(const NcnnTensor& tensor) noexcept {
    return static_cast<int64_t>(tensor.data.size()) * static_cast<int64_t>(sizeof(float));
}

/// The frozen request validation (contract blocks 4/6 and the M7-11
/// orchestration suite's executable spec): the validate(ImageView) rule set,
/// then the accepted_formats gate, then finite/positive/fully-inside bounds.
/// Validation precedes cancellation (frozen ordering).
Result<void> validate_request(const ImageView& image, const RectF& bounds,
                              const std::vector<PixelFormat>& accepted_formats) {
    if (Result<void> checked = mirador::validate(image); !checked.ok()) {
        return checked;
    }
    const bool declared = std::any_of(accepted_formats.begin(), accepted_formats.end(),
                                      [&](PixelFormat format) { return format == image.format; });
    if (!declared) {
        return Status{mirador::ErrorCode::kUnsupportedFormat, "prepared format not declared in accepted_formats"};
    }
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || bounds.width <= 0.0F || bounds.height <= 0.0F || bounds.x < 0.0F ||
        bounds.y < 0.0F || bounds.x + bounds.width > static_cast<float>(image.width) ||
        bounds.y + bounds.height > static_cast<float>(image.height)) {
        return Status{mirador::ErrorCode::kInvalidArgument,
                      "request bounds must be finite, positive and fully inside the prepared image"};
    }
    return {};
}

Status cancellation_status(const ExecutionContext& context, const char* where) {
    if (mirador::is_cancelled(context)) {
        return Status{mirador::ErrorCode::kCancelled, std::string{"cancelled "} + where};
    }
    if (mirador::deadline_reached(context)) {
        return Status{mirador::ErrorCode::kTimeout, std::string{"deadline reached "} + where};
    }
    return Status::success();
}

/// Per-channel mean of the presented pixels (uint64 sums + one double divide
/// each — deterministic); out-of-crop samples are padded with it, the port's
/// mean-pad semantics without its OpenCV dependency.
struct ChannelMeans {
    std::array<double, 3> value = {0.0, 0.0, 0.0};
};

ChannelMeans channel_means(const ImageView& image) {
    ChannelMeans means;
    const int64_t stride = image.row_stride_bytes;
    std::array<uint64_t, 3> sums = {0U, 0U, 0U};
    for (int32_t y = 0; y < image.height; ++y) {
        const std::byte* row = image.data + static_cast<int64_t>(y) * stride;
        for (int32_t x = 0; x < image.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                sums[static_cast<size_t>(c)] += std::to_integer<uint8_t>(row[static_cast<int64_t>(x) * 3 + c]);
            }
        }
    }
    const double count = static_cast<double>(image.width) * static_cast<double>(image.height);
    for (int c = 0; c < 3; ++c) {
        means.value[static_cast<size_t>(c)] = static_cast<double>(sums[static_cast<size_t>(c)]) / count;
    }
    return means;
}

/// One presented-image sample with mean padding outside the view.
float sample_channel(const ImageView& image, int64_t stride, int32_t x, int32_t y, int c, float pad) noexcept {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return pad;
    }
    const std::byte* row = image.data + static_cast<int64_t>(y) * stride;
    return static_cast<float>(std::to_integer<uint8_t>(row[static_cast<int64_t>(x) * 3 + c]));
}

/// Crops the square `side` window centered at (center_x, center_y) from the
/// presented view and resamples it bilinearly to `dst` x `dst` RGB as a planar
/// CHW tensor of packed 0..255 values. Rotation metadata is never read (the
/// view is consumed exactly as presented); non-contiguous strides are honored;
/// input pixels are never modified. Cancellation is polled per destination
/// row. The destination buffer is charged to `budget` up front.
Result<NcnnTensor> crop_resize_rgb(const ImageView& image, float center_x, float center_y, float side, int32_t dst,
                                   const ChannelMeans& means, WorkBudget& budget, const ExecutionContext& context) {
    const int64_t dst_bytes = static_cast<int64_t>(dst) * dst * 3 * static_cast<int64_t>(sizeof(float));
    if (const Result<void> charged = budget.charge(dst_bytes); !charged.ok()) {
        return charged.status();
    }
    NcnnTensor tensor;
    tensor.width = dst;
    tensor.height = dst;
    tensor.channels = 3;
    tensor.data.assign(static_cast<size_t>(dst) * dst * 3U, 0.0F);

    const float scale = side / static_cast<float>(dst);
    const float origin_x = center_x - side / 2.0F;
    const float origin_y = center_y - side / 2.0F;
    const int64_t stride = image.row_stride_bytes;
    const std::array<float, 3> pad = {static_cast<float>(means.value[0]), static_cast<float>(means.value[1]),
                                      static_cast<float>(means.value[2])};
    for (int32_t dy = 0; dy < dst; ++dy) {
        // Periodic cancellation poll (the forward itself is atomic; the loops
        // around it are where the budget of a frame is spent).
        if (dy % 64 == 0) {
            if (const Status status = cancellation_status(context, "while resampling the search crop"); !status.ok()) {
                return status;
            }
        }
        // Bilinear sampling grid: the destination pixel center maps to a
        // source position relative to the crop origin, in image coordinates.
        const float sy = origin_y + (static_cast<float>(dy) + 0.5F) * scale - 0.5F;
        const float y0f = std::floor(sy);
        const float fy = sy - y0f;
        const auto y0 = static_cast<int32_t>(y0f);
        for (int32_t dx = 0; dx < dst; ++dx) {
            const float sx = origin_x + (static_cast<float>(dx) + 0.5F) * scale - 0.5F;
            const float x0f = std::floor(sx);
            const float fx = sx - x0f;
            const auto x0 = static_cast<int32_t>(x0f);
            for (int c = 0; c < 3; ++c) {
                const float p00 = sample_channel(image, stride, x0, y0, c, pad[static_cast<size_t>(c)]);
                const float p10 = sample_channel(image, stride, x0 + 1, y0, c, pad[static_cast<size_t>(c)]);
                const float p01 = sample_channel(image, stride, x0, y0 + 1, c, pad[static_cast<size_t>(c)]);
                const float p11 = sample_channel(image, stride, x0 + 1, y0 + 1, c, pad[static_cast<size_t>(c)]);
                const float top = p00 + (p10 - p00) * fx;
                const float bottom = p01 + (p11 - p01) * fx;
                tensor.data[(static_cast<size_t>(c) * dst + static_cast<size_t>(dy)) * dst + static_cast<size_t>(dx)] =
                    top + (bottom - top) * fy;
            }
        }
    }
    if (const Status status = cancellation_status(context, "after resampling the search crop"); !status.ok()) {
        return status;
    }
    return tensor;
}

/// The port's cosine window: separable Hann over the score grid.
std::vector<float> cosine_window(int32_t score_size) {
    std::vector<float> window(static_cast<size_t>(score_size) * static_cast<size_t>(score_size), 0.0F);
    std::vector<float> hanning(static_cast<size_t>(score_size), 0.0F);
    for (int32_t i = 0; i < score_size; ++i) {
        hanning[static_cast<size_t>(i)] =
            0.5F - 0.5F * std::cos(2.0F * std::numbers::pi_v<float> * static_cast<float>(i) /
                                   static_cast<float>(score_size - 1));
    }
    for (int32_t r = 0; r < score_size; ++r) {
        for (int32_t c = 0; c < score_size; ++c) {
            window[static_cast<size_t>(r) * static_cast<size_t>(score_size) + static_cast<size_t>(c)] =
                hanning[static_cast<size_t>(r)] * hanning[static_cast<size_t>(c)];
        }
    }
    return window;
}

float sigmoid(float x) noexcept {
    return 1.0F / (1.0F + std::exp(-x));
}

/// The runtime wrapper reports unknown blobs as kInvalidArgument — correct for
/// its own caller, which inside this backend is the backend itself. A model
/// that violates the frozen blob contract is never the SPI caller's error, so
/// forward results are remapped: the fusion side must read this as
/// kBackendFailure (rebuild the session), not as "your request was bad".
template <typename T>
Result<T> remap_model_contract_error(Result<T>&& result) {
    if (!result.ok() && result.status().code() == mirador::ErrorCode::kInvalidArgument) {
        return Status{mirador::ErrorCode::kBackendFailure,
                      "NanoTrack model violates the frozen blob contract: " + result.status().message()};
    }
    return std::move(result);
}

/// Geometric search-crop derivation shared by initialize and update (the
/// port's s_z / scale_z / s_x chain): the crop side covers the reference size
/// plus context, expressed in prepared-image pixels.
struct CropGeometry {
    float crop_side = 0.0F;      ///< s_x: the search crop side, prepared px
    float scale_z = 1.0F;        ///< prepared px -> template-crop px factor
    float template_side = 0.0F;  ///< s_z: the context target side, prepared px
};

CropGeometry crop_geometry(float ref_width, float ref_height, const NanoTrackerOptions& options) noexcept {
    const float wc = ref_width + options.context_amount * (ref_width + ref_height);
    const float hc = ref_height + options.context_amount * (ref_width + ref_height);
    const float template_side = std::sqrt(wc * hc);
    CropGeometry geometry;
    geometry.template_side = template_side;
    geometry.scale_z = static_cast<float>(options.exemplar_size) / template_side;
    const float d_search = static_cast<float>(options.instance_size - options.exemplar_size) / 2.0F;
    geometry.crop_side = template_side + 2.0F * (d_search / geometry.scale_z);
    return geometry;
}

/// A decoded frame: everything update() needs to answer and to commit.
struct DecodedFrame {
    RectF bounds;  ///< prepared-image pixel space, clamped inside
    float confidence = 0.0F;
    float new_ref_width = 0.0F;   ///< committed only on success
    float new_ref_height = 0.0F;  ///< committed only on success
};

/// The winner of the frozen response scan: first row-major maximum over
/// penalty * sigmoid(foreground) blended with the cosine window. Cells with a
/// non-finite or non-positive predicted box are structurally broken model
/// output and are skipped; a response map without a single usable cell is
/// kBackendFailure.
struct ResponsePeak {
    size_t cell = 0;
    float penalty = 0.0F;
};

Result<ResponsePeak> scan_response_peak(const NcnnTensor& cls, const NcnnTensor& box, int32_t grid,
                                        float ref_width_crop, float ref_height_crop, const NanoTrackerOptions& options,
                                        const std::vector<float>& window, const ExecutionContext& context) {
    const size_t cells = static_cast<size_t>(grid) * static_cast<size_t>(grid);
    const float* cls_foreground = cls.data.data() + cells;  // channel 1 = foreground logit
    const float* left = box.data.data();
    const float* top = box.data.data() + cells;
    const float* right = box.data.data() + cells * 2U;
    const float* bottom = box.data.data() + cells * 3U;

    // Reference size in search-crop pixels (the penalty compares like with
    // like; the port scales its state into crop units the same way).
    const float ref_pad = (ref_width_crop + ref_height_crop) * 0.5F;
    const float ref_size = std::sqrt((ref_width_crop + ref_pad) * (ref_height_crop + ref_pad));
    const float ref_ratio = ref_width_crop / ref_height_crop;

    bool found = false;
    float best_response = -1.0F;
    ResponsePeak peak;
    for (int32_t r = 0; r < grid; ++r) {
        if (r % 64 == 0) {
            if (const Status status = cancellation_status(context, "while decoding the response maps"); !status.ok()) {
                return status;
            }
        }
        for (int32_t c = 0; c < grid; ++c) {
            const size_t cell = static_cast<size_t>(r) * static_cast<size_t>(grid) + static_cast<size_t>(c);
            // The predicted ltrb box for this cell, in search-crop pixels.
            const float px1 = static_cast<float>(c) * static_cast<float>(options.total_stride) - left[cell];
            const float py1 = static_cast<float>(r) * static_cast<float>(options.total_stride) - top[cell];
            const float px2 = static_cast<float>(c) * static_cast<float>(options.total_stride) + right[cell];
            const float py2 = static_cast<float>(r) * static_cast<float>(options.total_stride) + bottom[cell];
            const float pw = px2 - px1;
            const float ph = py2 - py1;
            if (!std::isfinite(pw) || !std::isfinite(ph) || pw <= 0.0F || ph <= 0.0F) {
                continue;  // broken cell output; never feeds the peak search
            }
            const float score = sigmoid(cls_foreground[cell]);
            const float pad = (pw + ph) * 0.5F;
            const float size_change = std::sqrt((pw + pad) * (ph + pad)) / ref_size;
            const float ratio_change = ref_ratio / (pw / ph);
            // max(t, 1/t) on both, exactly like the port's sz/ratio penalties;
            // with positive sizes both factors are >= 1, so penalty <= 1.
            const float s = std::max(size_change, 1.0F / size_change);
            const float q = std::max(ratio_change, 1.0F / ratio_change);
            const float penalty = std::exp(-(s * q - 1.0F) * options.penalty_k);
            const float response =
                penalty * score * (1.0F - options.window_influence) + window[cell] * options.window_influence;
            if (!std::isfinite(response)) {
                return Status{mirador::ErrorCode::kBackendFailure, "NanoTrack head produced a non-finite response"};
            }
            // First maximum in row-major scan order: deterministic total order.
            if (!found || response > best_response) {
                found = true;
                best_response = response;
                peak.cell = cell;
                peak.penalty = penalty;
            }
        }
    }
    if (!found) {
        return Status{mirador::ErrorCode::kBackendFailure,
                      "NanoTrack head produced no usable cell in the response maps"};
    }
    if (const Status status = cancellation_status(context, "after decoding the response maps"); !status.ok()) {
        return status;
    }
    return peak;
}

/// Frozen reference decode (the class comment's numbered contract): response
/// maps -> penalties/window -> first row-major maximum -> ltrb box mapped
/// back through the search crop. Allocates nothing; the per-cell values are
/// computed in the scan helper above.
Result<DecodedFrame> decode_update(const NcnnTensor& cls, const NcnnTensor& box, const ImageView& prepared,
                                   float prior_center_x, float prior_center_y, float ref_width, float ref_height,
                                   const CropGeometry& geometry, const NanoTrackerOptions& options,
                                   const std::vector<float>& window, const ExecutionContext& context) {
    // Frozen head output shapes (model contract): 2 and 4 channels over the
    // square score grid; anything else is a broken model, never a decode
    // guess -> kBackendFailure.
    const int32_t grid = options.score_size;
    const size_t cells = static_cast<size_t>(grid) * static_cast<size_t>(grid);
    if (cls.channels != 2 || cls.width != grid || cls.height != grid || cls.data.size() != cells * 2U ||
        box.channels != 4 || box.width != grid || box.height != grid || box.data.size() != cells * 4U) {
        return Status{mirador::ErrorCode::kBackendFailure,
                      "NanoTrack head outputs must be 2x and 4x score_size x score_size response maps"};
    }

    auto peak = scan_response_peak(cls, box, grid, ref_width * geometry.scale_z, ref_height * geometry.scale_z, options,
                                   window, context);
    if (!peak.ok()) {
        return peak.status();
    }

    const float* cls_foreground = cls.data.data() + cells;  // channel 1 = foreground logit
    const float* left = box.data.data();
    const float* top = box.data.data() + cells;
    const float* right = box.data.data() + cells * 2U;
    const float* bottom = box.data.data() + cells * 3U;

    // Winner cell: grid position, ltrb box, crop-space center and size.
    const auto win_r = static_cast<int32_t>(peak.value().cell / static_cast<size_t>(grid));
    const auto win_c = static_cast<int32_t>(peak.value().cell % static_cast<size_t>(grid));
    const float gx = static_cast<float>(win_c) * static_cast<float>(options.total_stride);
    const float gy = static_cast<float>(win_r) * static_cast<float>(options.total_stride);
    const float pred_x1 = gx - left[peak.value().cell];
    const float pred_y1 = gy - top[peak.value().cell];
    const float pred_x2 = gx + right[peak.value().cell];
    const float pred_y2 = gy + bottom[peak.value().cell];
    const float pred_center_x = (pred_x1 + pred_x2) / 2.0F;
    const float pred_center_y = (pred_y1 + pred_y2) / 2.0F;
    const float pred_w_crop = pred_x2 - pred_x1;
    const float pred_h_crop = pred_y2 - pred_y1;
    if (!std::isfinite(pred_center_x) || !std::isfinite(pred_center_y)) {
        return Status{mirador::ErrorCode::kBackendFailure, "NanoTrack head produced a non-finite box at the peak"};
    }

    // Map back through the search crop into prepared-image pixel space.
    const float half_instance = static_cast<float>(options.instance_size) / 2.0F;
    const float center_x = prior_center_x + (pred_center_x - half_instance) / geometry.scale_z;
    const float center_y = prior_center_y + (pred_center_y - half_instance) / geometry.scale_z;
    const float width = pred_w_crop / geometry.scale_z;
    const float height = pred_h_crop / geometry.scale_z;

    // Implementations report bounds fully inside the prepared image (contract
    // block 6, DEC-021): clamp, and fail loudly when nothing remains.
    RectF mapped;
    mapped.x = std::max(0.0F, center_x - width / 2.0F);
    mapped.y = std::max(0.0F, center_y - height / 2.0F);
    mapped.width = std::min(static_cast<float>(prepared.width), center_x + width / 2.0F) - mapped.x;
    mapped.height = std::min(static_cast<float>(prepared.height), center_y + height / 2.0F) - mapped.y;
    if (!std::isfinite(mapped.x) || !std::isfinite(mapped.y) || mapped.width <= 0.0F || mapped.height <= 0.0F) {
        return Status{mirador::ErrorCode::kBackendFailure,
                      "NanoTrack decode produced empty bounds outside the prepared image"};
    }

    const float confidence = sigmoid(cls_foreground[peak.value().cell]);
    // Canonical single reference-size update (the port's incidental double
    // application of the learning rate is normalized to this one step). The
    // convex combination of positive sizes stays positive.
    const float rate = peak.value().penalty * confidence * options.size_learning_rate;
    DecodedFrame frame;
    frame.bounds = mapped;
    frame.confidence = confidence;
    frame.new_ref_width = ref_width + (width - ref_width) * rate;
    frame.new_ref_height = ref_height + (height - ref_height) * rate;
    return frame;
}

/// One tracked target's backend-side state (contract block 2): the template
/// feature (the stored initialize forward) and the reference size the
/// penalties and the next search crop derive from. Destroying the session
/// discards all of it; sessions never share state with each other.
class NanoTrackerSession final : public TrackerSession {
public:
    // The runtimes are the owning backend's shared state, held by reference:
    // the tracker contract (block 1) keeps the backend alive for every
    // session's lifetime, and NcnnRuntime is move-only, so sessions can neither
    // own nor copy it.
    NanoTrackerSession(NcnnTensor template_feature, float ref_width, float ref_height, ChannelMeans pad_means,
                       NanoTrackerOptions options, const NcnnRuntime& backbone, const NcnnRuntime& head,
                       std::vector<float> window)
        : template_feature_(std::move(template_feature)),
          ref_width_(ref_width),
          ref_height_(ref_height),
          cached_means_(pad_means),
          options_(std::move(options)),
          backbone_(backbone),
          head_(head),
          window_(std::move(window)) {}

    Result<TrackerUpdateResult> update(const ImageView& prepared_image, const TrackerUpdateRequest& request,
                                       const ExecutionContext& context) override {
        // 1. Structural validation precedes cancellation (frozen ordering).
        if (const Result<void> checked = validate_request(prepared_image, request.prior_bounds, kAcceptedFormats);
            !checked.ok()) {
            return checked.status();
        }
        // 2. Cancellation and deadline at entry, then polled during work.
        if (Status status = cancellation_status(context, "before the update forward"); !status.ok()) {
            return status;
        }

        WorkBudget budget(options_.work_budget_bytes);
        const float prior_center_x = request.prior_bounds.x + request.prior_bounds.width / 2.0F;
        const float prior_center_y = request.prior_bounds.y + request.prior_bounds.height / 2.0F;
        const CropGeometry geometry = crop_geometry(ref_width_, ref_height_, options_);

        // 3. Search crop around the caller's prior (the position estimate is
        // the caller's, every frame; the session contributes appearance only).
        auto search = crop_resize_rgb(prepared_image, prior_center_x, prior_center_y, geometry.crop_side,
                                      options_.instance_size, cached_means_, budget, context);
        if (!search.ok()) {
            return search.status();
        }

        // 4. Backbone forward on the search crop.
        auto feature = backbone_.run(kBackboneInput, search.value(), kBackboneOutput, context);
        if (!feature.ok()) {
            return remap_model_contract_error(std::move(feature)).status();
        }
        if (const Result<void> charged = budget.charge(tensor_bytes(feature.value())); !charged.ok()) {
            return charged.status();
        }

        // 5. Head forward on the stored template feature + the search feature.
        // The contract-fixed output sizes and the call-local copy of the
        // template feature are charged up front (RULE-06).
        const size_t cells = static_cast<size_t>(options_.score_size) * static_cast<size_t>(options_.score_size);
        if (const Result<void> charged =
                budget.charge(static_cast<int64_t>(cells * 6U) * static_cast<int64_t>(sizeof(float)) +
                              tensor_bytes(template_feature_));
            !charged.ok()) {
            return charged.status();
        }
        auto head = head_.run_multi({{kHeadTemplateInput, template_feature_}, {kHeadSearchInput, feature.value()}},
                                    {kHeadClsOutput, kHeadBoxOutput}, context);
        if (!head.ok()) {
            return remap_model_contract_error(std::move(head)).status();
        }
        if (head.value().size() != 2U) {
            return Status{mirador::ErrorCode::kBackendFailure, "NanoTrack head must expose both response maps"};
        }

        // 6. Frozen decode. Everything below computes into locals; the session
        // state commits only after the last error path (all-or-nothing).
        auto decoded = decode_update(head.value()[0], head.value()[1], prepared_image, prior_center_x, prior_center_y,
                                     ref_width_, ref_height_, geometry, options_, window_, context);
        if (!decoded.ok()) {
            return decoded.status();
        }

        // 7. Commit-on-success: the per-frame result and the reference size
        // move together or not at all.
        ref_width_ = decoded.value().new_ref_width;
        ref_height_ = decoded.value().new_ref_height;
        TrackerUpdateResult result;
        result.bounds = decoded.value().bounds;
        result.confidence = decoded.value().confidence;
        return result;
    }

private:
    // Accepted formats mirror BackendInfo::accepted_formats (the kRgb8 gate).
    static inline const std::vector<PixelFormat> kAcceptedFormats = {PixelFormat::kRgb8};

    NcnnTensor template_feature_;
    float ref_width_ = 0.0F;
    float ref_height_ = 0.0F;
    /// Mean-pad source; computed once per session from the initialize frame
    /// (the port keeps its frame mean the same way). Only the pad values come
    /// from here — every in-bounds sample reads the current frame.
    ChannelMeans cached_means_;
    NanoTrackerOptions options_;
    const NcnnRuntime& backbone_;
    const NcnnRuntime& head_;
    std::vector<float> window_;
};

BackendInfo make_info(const NanoTrackModelIdentity& identity) {
    BackendInfo info;
    info.name = "nanotrack-ncnn-reference";
    info.implementation_version = "0.1.0";
    info.model_id = identity.model_id;
    info.model_revision = identity.model_revision;
    info.accepted_formats = {PixelFormat::kRgb8};
    info.thread_safe = false;
    return info;
}

}  // namespace

Result<NanoTrackerBackend> NanoTrackerBackend::create(const NanoTrackerOptions& options) {
    if (options.backbone_param_path.empty() || options.backbone_bin_path.empty() || options.head_param_path.empty() ||
        options.head_bin_path.empty()) {
        return Status{mirador::ErrorCode::kInvalidArgument, "backbone and head param/bin paths are required"};
    }
    if (options.num_threads != 1) {
        return Status{mirador::ErrorCode::kInvalidArgument,
                      "num_threads must be 1: the tracker SPI requires per-sequence bit determinism"};
    }
    if (options.exemplar_size < 1 || options.instance_size < options.exemplar_size) {
        return Status{mirador::ErrorCode::kInvalidArgument,
                      "exemplar_size must be >= 1 and instance_size >= exemplar_size"};
    }
    if (options.score_size < 2 || options.score_size > 128) {
        return Status{mirador::ErrorCode::kInvalidArgument, "score_size must be in [2, 128]"};
    }
    if (options.total_stride < 1) {
        return Status{mirador::ErrorCode::kInvalidArgument, "total_stride must be >= 1"};
    }
    if (!std::isfinite(options.context_amount) || options.context_amount < 0.0F || options.context_amount >= 1.0F) {
        return Status{mirador::ErrorCode::kInvalidArgument, "context_amount must be in [0, 1)"};
    }
    if (!std::isfinite(options.penalty_k) || options.penalty_k < 0.0F) {
        return Status{mirador::ErrorCode::kInvalidArgument, "penalty_k must be finite and >= 0"};
    }
    if (!std::isfinite(options.window_influence) || options.window_influence < 0.0F ||
        options.window_influence > 1.0F) {
        return Status{mirador::ErrorCode::kInvalidArgument, "window_influence must be in [0, 1]"};
    }
    if (!std::isfinite(options.size_learning_rate) || options.size_learning_rate < 0.0F ||
        options.size_learning_rate > 1.0F) {
        return Status{mirador::ErrorCode::kInvalidArgument, "size_learning_rate must be in [0, 1]"};
    }
    if (options.work_budget_bytes <= 0) {
        return Status{mirador::ErrorCode::kInvalidArgument, "work_budget_bytes must be > 0"};
    }

    NcnnRuntimeOptions backbone_options;
    backbone_options.param_path = options.backbone_param_path;
    backbone_options.bin_path = options.backbone_bin_path;
    backbone_options.num_threads = options.num_threads;
    auto backbone = NcnnRuntime::create(backbone_options);
    if (!backbone.ok()) {
        return backbone.status();
    }
    NcnnRuntimeOptions head_options;
    head_options.param_path = options.head_param_path;
    head_options.bin_path = options.head_bin_path;
    head_options.num_threads = options.num_threads;
    auto head = NcnnRuntime::create(head_options);
    if (!head.ok()) {
        return head.status();
    }

    NanoTrackerBackend backend;
    backend.options_ = options;
    backend.backbone_ = std::move(backbone).take_value();
    backend.head_ = std::move(head).take_value();
    backend.info_ = make_info(options.identity);
    return backend;
}

BackendInfo NanoTrackerBackend::info() const {
    return info_;
}

mirador::Result<std::unique_ptr<mirador::TrackerSession>> NanoTrackerBackend::initialize(
    const ImageView& prepared_image, const TrackerInitRequest& request, const ExecutionContext& context) {
    // 1. Structural validation precedes cancellation (frozen ordering).
    if (const Result<void> checked = validate_request(prepared_image, request.initial_bounds, info_.accepted_formats);
        !checked.ok()) {
        return checked.status();
    }
    // 2. Cancellation and deadline at entry, then polled during work.
    if (Status status = cancellation_status(context, "before the initialize forward"); !status.ok()) {
        return status;
    }

    WorkBudget budget(options_.work_budget_bytes);
    const float center_x = request.initial_bounds.x + request.initial_bounds.width / 2.0F;
    const float center_y = request.initial_bounds.y + request.initial_bounds.height / 2.0F;
    // The template crop covers the target plus context, resampled to the
    // exemplar side (the port's z-crop).
    const CropGeometry geometry = crop_geometry(request.initial_bounds.width, request.initial_bounds.height, options_);
    const ChannelMeans means = channel_means(prepared_image);
    auto crop = crop_resize_rgb(prepared_image, center_x, center_y, geometry.template_side, options_.exemplar_size,
                                means, budget, context);
    if (!crop.ok()) {
        return crop.status();
    }

    // Template branch forward; its feature is the session's appearance state.
    auto feature = backbone_.run(kBackboneInput, crop.value(), kBackboneOutput, context);
    if (!feature.ok()) {
        return remap_model_contract_error(std::move(feature)).status();
    }
    if (feature.value().channels < 1 || feature.value().width < 1 || feature.value().height < 1 ||
        feature.value().data.size() != static_cast<size_t>(feature.value().channels) *
                                           static_cast<size_t>(feature.value().width) *
                                           static_cast<size_t>(feature.value().height)) {
        return Status{mirador::ErrorCode::kBackendFailure, "NanoTrack backbone produced no usable template feature"};
    }
    if (const Result<void> charged = budget.charge(tensor_bytes(feature.value())); !charged.ok()) {
        return charged.status();
    }

    // Template construction succeeded: only now does a session (and with it
    // any state) exist. A later initialize builds a NEW session, never
    // restarts this one (contract block 1). The window allocation is charged
    // before it happens (RULE-06).
    if (const Result<void> charged =
            budget.charge(static_cast<int64_t>(options_.score_size) * static_cast<int64_t>(options_.score_size) *
                          static_cast<int64_t>(sizeof(float)));
        !charged.ok()) {
        return charged.status();
    }
    auto session = std::make_unique<NanoTrackerSession>(std::move(feature).take_value(), request.initial_bounds.width,
                                                        request.initial_bounds.height, means, options_, backbone_,
                                                        head_, cosine_window(options_.score_size));
    return {std::move(session)};
}

}  // namespace mirador::integrations
