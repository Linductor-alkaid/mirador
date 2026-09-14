// M4-07: runtime-free example walking the M4 loop from design sections 16 and
// 17 and section 24 M4 - Accessibility-provided external regions, stub OCR and
// stub detection evidence are fused deterministically into a SemanticSnapshot,
// rendered as a Set-of-Mark image and tracked across frames with stable ids
// and generation checks. A big UI change allocates fresh ids, bumps the
// generation and the upper layer refuses its stale reference - exactly the
// hybrid-localization story the design demands, without any VLM call, model
// runtime or thread (privacy defaults, sync API).

#include <mirador/backend_info.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/evidence.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/fusion.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/set_of_mark.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using mirador::BackendInfo;
using mirador::ChangeReport;
using mirador::DetectionRegion;
using mirador::DetectorBackend;
using mirador::EvidenceSet;
using mirador::ExecutionContext;
using mirador::ExternalRegion;
using mirador::Frame;
using mirador::FusionOptions;
using mirador::ImageView;
using mirador::OcrBackend;
using mirador::OcrRequest;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::RectI;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::TextRegion;
using mirador::VisualRegion;

constexpr int32_t kWidth = 320;
constexpr int32_t kHeight = 200;
constexpr int64_t kStride = static_cast<int64_t>(kWidth) * 4;

/// Deterministic synthetic screen: a gradient with a tint-controlled lower
/// half, plus one filled rectangle simulating the visual button chrome.
void paint_screen(std::vector<std::byte>& bytes, uint8_t tint, const RectI& button) {
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
    for (int32_t y = button.y; y < button.y + button.height; ++y) {
        std::byte* row = bytes.data() + static_cast<int64_t>(y) * kStride;
        for (int32_t x = button.x; x < button.x + button.width; ++x) {
            row[x * 4 + 0] = std::byte{220};
            row[x * 4 + 1] = std::byte{120};
            row[x * 4 + 2] = std::byte{60};
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
    frame.source_id = "demo-phone";
    frame.owner = std::make_shared<const std::vector<std::byte>>(bytes);
    return frame;
}

/// Stub OCR backend: reports one "确定" line over the central band of the
/// prepared view (where the demo button sits). `active_` simulates the text
/// disappearing from the screen; a disabled stub returns no lines.
class StubOcrBackend final : public OcrBackend {
public:
    explicit StubOcrBackend(bool active) : active_(active) {}

    [[nodiscard]] BackendInfo info() const override { return info_; }

    mirador::Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest&,
                                                       const ExecutionContext&) override {
        std::vector<TextRegion> lines;
        if (!active_) {
            return lines;
        }
        TextRegion line;
        line.bounds = RectF{static_cast<float>(prepared.width) / 4.0F, static_cast<float>(prepared.height) / 4.0F,
                            static_cast<float>(prepared.width) / 2.0F, static_cast<float>(prepared.height) / 2.0F};
        line.utf8_text = "确定";
        line.confidence = 0.9F;
        lines.push_back(std::move(line));
        return lines;
    }

private:
    bool active_ = true;
    BackendInfo info_{"stub-ocr", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
};

/// Stub detector backend: reports the same central box as an icon proposal.
class StubDetectorBackend final : public DetectorBackend {
public:
    explicit StubDetectorBackend(bool active) : active_(active) {}

    [[nodiscard]] BackendInfo info() const override { return info_; }

    mirador::Result<std::vector<DetectionRegion>> detect(const ImageView& prepared, const mirador::DetectionRequest&,
                                                         const ExecutionContext&) override {
        std::vector<DetectionRegion> boxes;
        if (!active_) {
            return boxes;
        }
        DetectionRegion box;
        box.bounds = RectF{static_cast<float>(prepared.width) / 3.0F, static_cast<float>(prepared.height) / 3.0F,
                           static_cast<float>(prepared.width) / 3.0F, static_cast<float>(prepared.height) / 3.0F};
        box.class_id = 7;
        box.label = "icon";
        box.confidence = 0.85F;
        boxes.push_back(std::move(box));
        return boxes;
    }

private:
    bool active_ = true;
    BackendInfo info_{"stub-detector", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
};

ExternalRegion make_button(const RectF& bounds) {
    ExternalRegion button;
    button.bounds = bounds;
    button.text = "确定";
    button.role = "button";
    button.interactive = true;
    button.confidence = 1.0F;
    return button;
}

ExternalRegion make_status() {
    ExternalRegion status;
    status.bounds = RectF{0.0F, 180.0F, 80.0F, 16.0F};
    status.text = "已连接";
    status.role = "text_view";
    status.confidence = 1.0F;
    return status;
}

/// Aggregates one frame's evidence: platform regions first, then the executed
/// capability results (the caller decides which backends run and when).
mirador::Result<EvidenceSet> collect_evidence(const ExternalRegion& button, const ExternalRegion& status,
                                              const std::vector<TextRegion>& lines,
                                              const std::vector<DetectionRegion>& boxes) {
    EvidenceSet evidence;
    if (mirador::Result<uint64_t> added = evidence.add_external(button); !added.ok()) {
        return added.status();
    }
    if (mirador::Result<uint64_t> added = evidence.add_external(status); !added.ok()) {
        return added.status();
    }
    for (const TextRegion& line : lines) {
        if (mirador::Result<uint64_t> added = evidence.add_text(line); !added.ok()) {
            return added.status();
        }
    }
    for (const DetectionRegion& box : boxes) {
        if (mirador::Result<uint64_t> added = evidence.add_detection(box); !added.ok()) {
            return added.status();
        }
    }
    return evidence;
}

std::string source_names(uint32_t mask) {
    std::string names;
    const auto append = [&names](const char* name) {
        if (!names.empty()) {
            names += "+";
        }
        names += name;
    };
    if (mirador::has_source(mask, mirador::RegionSource::kExternal)) {
        append("external");
    }
    if (mirador::has_source(mask, mirador::RegionSource::kOcr)) {
        append("ocr");
    }
    if (mirador::has_source(mask, mirador::RegionSource::kDetector)) {
        append("detector");
    }
    return names;
}

void print_snapshot(const SemanticSnapshot& snapshot) {
    std::printf("  generation=%llu frame=%llu regions=%zu\n", static_cast<unsigned long long>(snapshot.generation),
                static_cast<unsigned long long>(snapshot.frame_sequence), snapshot.regions.size());
    for (const VisualRegion& region : snapshot.regions) {
        std::printf("    id=%llu [%s] conf=%.2f bounds=(%.0f,%.0f %.0fx%.0f) anchor=(%.0f,%.0f)\n",
                    static_cast<unsigned long long>(region.stable_id), source_names(region.source_mask).c_str(),
                    static_cast<double>(region.confidence), static_cast<double>(region.bounds.x),
                    static_cast<double>(region.bounds.y), static_cast<double>(region.bounds.width),
                    static_cast<double>(region.bounds.height), static_cast<double>(region.anchor.x),
                    static_cast<double>(region.anchor.y));
    }
}

/// Simulates the SoM contract: the upper layer sends the marked image to its
/// VLM, reads back {generation, mark_id} and resolves it to an anchor only
/// when the generation still matches (design section 17).
void resolve_mark(const SemanticSnapshot& snapshot, const mirador::SetOfMarkResult& marks, uint32_t mark_id,
                  uint64_t claimed_generation) {
    if (!mirador::is_generation_current(snapshot, claimed_generation)) {
        std::printf("  mark %u refused: generation %llu is stale (snapshot is at %llu)\n", mark_id,
                    static_cast<unsigned long long>(claimed_generation),
                    static_cast<unsigned long long>(snapshot.generation));
        return;
    }
    for (const mirador::SoMMark& mark : marks.marks) {
        if (mark.mark_id == mark_id) {
            const VisualRegion* region = mirador::find_region(snapshot, mark.stable_id);
            if (region != nullptr) {
                std::printf("  mark %u -> stable_id %llu, click anchor (%.0f, %.0f)\n", mark_id,
                            static_cast<unsigned long long>(mark.stable_id), static_cast<double>(region->anchor.x),
                            static_cast<double>(region->anchor.y));
            }
            return;
        }
    }
}

}  // namespace

int main() {
    PerceptionSessionOptions option_bag;
    option_bag.source_id = "demo-phone";
    PerceptionSession session = PerceptionSession::create(option_bag).take_value();

    std::vector<std::byte> frame_bytes(static_cast<size_t>(kStride) * kHeight, std::byte{0});

    // -- Frame 1: fresh screen. External regions + OCR + detection fuse into
    //    one button cluster (platform semantics travel in the property bag,
    //    never inferred from pixels), the status bar stays separate.
    paint_screen(frame_bytes, 0, RectI{80, 50, 160, 100});
    const Frame first = frame_over(frame_bytes, 1);
    StubOcrBackend ocr(true);
    StubDetectorBackend detector(true);

    const ChangeReport change1 = session.analyze_change(first).value();
    std::printf("frame 1: classification=%d reason=%d\n", static_cast<int>(change1.classification),
                static_cast<int>(change1.reason));
    const OcrRequest ocr_request;
    const mirador::DetectionRequest detection_request;
    const mirador::Result<EvidenceSet> evidence1 =
        collect_evidence(make_button(RectF{80.0F, 50.0F, 160.0F, 100.0F}), make_status(),
                         session.run_ocr(first, &ocr, ocr_request).value(),
                         session.run_detector(first, &detector, detection_request).value());
    const FusionOptions fusion_options;
    const SemanticSnapshot snapshot1 = session.fuse(first, evidence1.value(), fusion_options).value();
    print_snapshot(snapshot1);
    const mirador::SetOfMarkResult marks1 = mirador::render_set_of_mark(first.image, snapshot1).value();
    std::printf("  som: %zu mark(s), marked image %dx%d\n", marks1.marks.size(), marks1.image.view().width,
                marks1.image.view().height);
    resolve_mark(snapshot1, marks1, 1, snapshot1.generation);

    // -- Frame 2: identical pixels. The change gate short-circuits: no
    //    backend runs and the published snapshot is reused as-is.
    const Frame second = frame_over(frame_bytes, 2);
    const ChangeReport change2 = session.analyze_change(second).value();
    std::printf("frame 2: classification=%d -> reuse snapshot (0 backend calls)\n",
                static_cast<int>(change2.classification));
    const SemanticSnapshot reused = *session.latest_snapshot();
    std::printf("  reused generation=%llu regions=%zu\n", static_cast<unsigned long long>(reused.generation),
                reused.regions.size());

    // -- Frame 3: the pixels change visibly (a global tint plus the button
    //    moving a little). Fusion re-runs on fresh evidence; the tracker
    //    keeps the cluster's stable id and the generation stays, so
    //    in-flight references remain valid - the upper layer just clicks the
    //    new anchor.
    paint_screen(frame_bytes, 60, RectI{88, 58, 160, 100});
    const Frame third = frame_over(frame_bytes, 3);
    const ChangeReport change3 = session.analyze_change(third).value();
    std::printf("frame 3: classification=%d changed_regions=%zu\n", static_cast<int>(change3.classification),
                change3.changed_regions.size());
    const mirador::Result<EvidenceSet> evidence3 =
        collect_evidence(make_button(RectF{88.0F, 58.0F, 160.0F, 100.0F}), make_status(),
                         session.run_ocr(third, &ocr, ocr_request).value(),
                         session.run_detector(third, &detector, detection_request).value());
    const SemanticSnapshot snapshot3 = session.fuse(third, evidence3.value(), fusion_options).value();
    print_snapshot(snapshot3);
    const mirador::SetOfMarkResult marks3 = mirador::render_set_of_mark(third.image, snapshot3).value();
    resolve_mark(snapshot3, marks3, 1, snapshot3.generation);
    std::printf("  stale reference (generation 1, id 1) still current: %s\n",
                mirador::is_generation_current(snapshot3, 1) ? "yes" : "no");

    // -- Frame 4: the screen changes wholesale (app switch). OCR and
    //    detection report nothing; two fresh icons far from every previous
    //    region are all the evidence. Nothing is retained: new ids,
    //    generation bump, stale references refused.
    paint_screen(frame_bytes, 200, RectI{0, 0, 0, 0});
    const Frame fourth = frame_over(frame_bytes, 4);
    const ChangeReport change4 = session.analyze_change(fourth).value();
    std::printf("frame 4: classification=%d\n", static_cast<int>(change4.classification));
    EvidenceSet evidence4;
    ExternalRegion icon_a = make_button(RectF{40.0F, 40.0F, 48.0F, 48.0F});
    icon_a.text.clear();
    icon_a.role = "icon";
    ExternalRegion icon_b = make_button(RectF{260.0F, 20.0F, 48.0F, 48.0F});
    icon_b.text.clear();
    icon_b.role = "icon";
    static_cast<void>(evidence4.add_external(icon_a));
    static_cast<void>(evidence4.add_external(icon_b));
    const SemanticSnapshot snapshot4 = session.fuse(fourth, evidence4, fusion_options).value();
    print_snapshot(snapshot4);
    std::printf("  frame-1 reference (generation 1, id 1) current on new snapshot: %s\n",
                mirador::is_generation_current(snapshot4, 1) ? "yes" : "no");
    const VisualRegion* stale = mirador::find_region(snapshot4, 1);
    std::printf("  id 1 present on new snapshot: %s\n", stale != nullptr ? "yes" : "no");
    return 0;
}
