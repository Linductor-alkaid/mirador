// M2-06: runtime-free example walking the M2 loop from design sections 9, 12
// and 18 - submit frames to a PerceptionSession, analyze change, run a stub
// OCR Backend through the synchronous SPI, recover coordinates, and reuse the
// capability result from the bounded cache on the next identical request. The
// backend is a deterministic stub (no model, no runtime): it "reads" one text
// line from the prepared image and honors the DEC-012 field contract.
// Everything is in-memory, offline and thread-free (privacy defaults).

#include <mirador/backend_info.hpp>
#include <mirador/cache_policy.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace {

using mirador::ExecutionContext;
using mirador::Frame;
using mirador::ImageView;
using mirador::OcrBackend;
using mirador::OcrRequest;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::Rotation;
using mirador::TextRegion;

constexpr int32_t kWidth = 320;
constexpr int32_t kHeight = 200;
constexpr int64_t kStride = static_cast<int64_t>(kWidth) * 4;

/// Deterministic synthetic "notification shade": a gradient panel whose value
/// `tint` shifts the lower half. No randomness, no external resources.
void paint_screen(std::vector<std::byte>& bytes, uint8_t tint) {
    for (int32_t y = 0; y < kHeight; ++y) {
        std::byte* row = bytes.data() + static_cast<int64_t>(y) * kStride;
        for (int32_t x = 0; x < kWidth; ++x) {
            const int32_t gradient = (x * 100 / (kWidth - 1) + (y >= 100 ? tint : 0)) % 256;
            row[x * 4 + 0] = static_cast<std::byte>(gradient);
            row[x * 4 + 1] = static_cast<std::byte>(gradient / 2);
            row[x * 4 + 2] = static_cast<std::byte>(255 - gradient);
            row[x * 4 + 3] = std::byte{255};
        }
    }
}

Frame frame_over(const std::vector<std::byte>& bytes, uint64_t sequence) {
    Frame frame;
    frame.image.data = bytes.data();
    frame.image.width = kWidth;
    frame.image.height = kHeight;
    frame.image.row_stride_bytes = kStride;
    frame.image.format = PixelFormat::kRgba8;
    frame.image.rotation = Rotation::k0;
    frame.sequence = sequence;
    frame.source_id = "demo-shade";
    frame.owner = std::make_shared<const std::vector<std::byte>>(bytes);
    return frame;
}

/// Stub OCR backend: reports one text line covering the central half of
/// whatever prepared view it receives, so coordinate recovery is observable
/// for any ROI/resize the session applies. Deterministic by construction.
class StubOcrBackend final : public OcrBackend {
public:
    [[nodiscard]] mirador::BackendInfo info() const override { return info_; }

    mirador::Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest& request,
                                                       const ExecutionContext& /*context*/) override {
        if (request.min_confidence > 0.8F) {
            return std::vector<TextRegion>{};  // the stub's only line scores 0.8
        }
        TextRegion line;
        line.bounds = RectF{static_cast<float>(prepared.width) / 4.0F, static_cast<float>(prepared.height) / 4.0F,
                            static_cast<float>(prepared.width) / 2.0F, static_cast<float>(prepared.height) / 2.0F};
        line.utf8_text = "demo line";
        line.confidence = 0.8F;
        return std::vector<TextRegion>{std::move(line)};
    }

private:
    mirador::BackendInfo info_{"stub-ocr", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
};

}  // namespace

int main() {
    PerceptionSessionOptions options;
    options.source_id = "demo-shade";
    PerceptionSession session = PerceptionSession::create(options).take_value();
    StubOcrBackend backend;

    std::vector<std::byte> frame_bytes(static_cast<size_t>(kStride) * kHeight, std::byte{0});
    paint_screen(frame_bytes, 0);
    const Frame first = frame_over(frame_bytes, 1);

    // 1. Submit the first frame: the session reports kFirstFrame (everything
    //    is new) and keeps only a compact signature of the frame.
    const mirador::ChangeReport first_change = session.analyze_change(first).value();
    std::printf("frame 1: classification=%d reason=%d\n", static_cast<int>(first_change.classification),
                static_cast<int>(first_change.reason));

    // 2. Run OCR over the lower half (where the tint lands). The stub sees the
    //    320x100 crop; the session recovers the line back into the frame.
    OcrRequest request;
    request.roi = RectF{0.0F, 100.0F, static_cast<float>(kWidth), 100.0F};
    const std::vector<TextRegion> first_run = session.run_ocr(first, &backend, request).value();
    std::printf("frame 1 ocr: %zu line(s), backend calls=%d, line at (%.1f, %.1f) %.1fx%.1f\n", first_run.size(), 1,
                first_run[0].bounds.x, first_run[0].bounds.y, first_run[0].bounds.width, first_run[0].bounds.height);

    // 3. The identical request is served from the capability cache: no second
    //    backend execution, same recovered regions.
    const std::vector<TextRegion> cached_run = session.run_ocr(first, &backend, request).value();
    std::printf("cached ocr: %zu line(s), cache entries=%zu bytes=%lld\n", cached_run.size(),
                session.result_cache().entry_count(), static_cast<long long>(session.result_cache().byte_size()));

    // 4. The tint changes only in the already-covered ROI; a kRefresh forces
    //    one fresh execution over the same key.
    paint_screen(frame_bytes, 40);
    const Frame second = frame_over(frame_bytes, 2);
    const mirador::ChangeReport second_change = session.analyze_change(second).value();
    std::printf("frame 2: classification=%d reason=%d\n", static_cast<int>(second_change.classification),
                static_cast<int>(second_change.reason));
    request.cache_policy = mirador::CachePolicy::kRefresh;
    const std::vector<TextRegion> refreshed = session.run_ocr(second, &backend, request).value();
    std::printf("refreshed ocr: %zu line(s), cache entries=%zu\n", refreshed.size(),
                session.result_cache().entry_count());
    return 0;
}
