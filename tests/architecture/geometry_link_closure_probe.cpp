// M3-02: probe executable that forces the mirador-geometry translation units
// into the link closure so the NEEDED-entry check covers the whole geometry
// module, including the LineDetector SPI dispatch through the abstract base.
#include <mirador/line_detector.hpp>

namespace {

class ProbeDetector final : public mirador::LineDetector {
public:
    [[nodiscard]] mirador::BackendInfo info() const override { return {}; }

    mirador::Result<mirador::LineSegmentSet> detect(const mirador::ImageView&, const mirador::LineDetectRequest&,
                                                    const mirador::ExecutionContext&) override {
        mirador::LineSegmentSet set;
        set.segments.push_back(mirador::LineSegment{{0.0F, 0.0F}, {10.0F, 0.0F}, 1.0F});
        return set;
    }
};

}  // namespace

int main() {
    ProbeDetector detector;
    mirador::LineDetector* spi = &detector;  // force vtable dispatch into the link closure
    const auto detected = spi->detect(mirador::ImageView{}, mirador::LineDetectRequest{}, {});
    if (!detected.ok() || detected.value().segments.size() != 1) {
        return 1;
    }
    const auto filtered = mirador::filter_segments(detected.value().segments, mirador::LineFilterParams{});
    if (!filtered.ok() || filtered.value().size() != 1) {
        return 2;
    }
    const auto merged = mirador::merge_collinear(filtered.value(), mirador::CollinearMergeParams{});
    return merged.ok() && merged.value().size() == 1 ? 0 : 3;
}
