// M3-11: runtime-free example walking the visual-index line from design
// sections 12 and 24 M3 - icon state reuse without re-running any model. Two
// synthetic toggle icons (filled disc = on, ring = off) are normalized into
// VisualPatchFingerprints by mirador::image and stored in the bounded
// mirador::cache VisualIndex. Later frames present the same icons under
// deterministic compression-like noise; the layered query recovers them via
// exact, perceptual or template evidence so the caller can reuse the known
// state instead of re-recognizing. The scenario is a desktop/system UI, not
// an Android screen; everything is in-memory and deterministic.

#include <mirador/image_view.hpp>
#include <mirador/patch_fingerprint.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/visual_fingerprint.hpp>
#include <mirador/visual_index.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using mirador::ImageView;
using mirador::PatchFingerprintParams;
using mirador::PixelFormat;
using mirador::VisualCandidate;
using mirador::VisualEvidenceKind;
using mirador::VisualPatchFingerprint;
using mirador::VisualQueryParams;

constexpr int32_t kSide = 32;

/// Paints a toggle icon into a packed gray buffer: `on` fills the disc,
/// `!on` leaves a ring. Deterministic bytes only.
std::vector<std::byte> paint_icon(bool on) {
    std::vector<std::byte> bytes(static_cast<size_t>(kSide) * kSide, std::byte{24});  // panel background
    const auto at = [&](int32_t x, int32_t y) -> std::byte& {
        return bytes[static_cast<size_t>(y) * kSide + x];
    };
    const int32_t center = kSide / 2;
    for (int32_t y = 4; y < kSide - 4; ++y) {
        for (int32_t x = 4; x < kSide - 4; ++x) {
            const int32_t dx = x - center;
            const int32_t dy = y - center;
            const int32_t radius_sq = dx * dx + dy * dy;
            if (radius_sq <= 121) {  // disc radius 11
                at(x, y) = static_cast<std::byte>(on ? 220 : 220);
                if (!on && radius_sq >= 64) {
                    at(x, y) = static_cast<std::byte>(24);  // hollow the ring
                }
                if (on && radius_sq >= 64) {
                    at(x, y) = static_cast<std::byte>(120);  // dimmed inner disc
                }
            }
        }
    }
    return bytes;
}

/// Applies a fixed, compression-like perturbation: every 7th pixel shifts by
/// +3/-3 depending on position. Deterministic, bounded, no randomness.
void perturb(std::vector<std::byte>& bytes) {
    for (size_t i = 0; i < bytes.size(); i += 7) {
        const int delta = (i % 14 == 0) ? 3 : -3;
        const int value = std::to_integer<int>(bytes[i]) + delta;
        bytes[i] = static_cast<std::byte>(value < 0 ? 0 : (value > 255 ? 255 : value));
    }
}

ImageView view_of(std::vector<std::byte>& bytes) {
    ImageView view;
    view.data = bytes.data();
    view.width = kSide;
    view.height = kSide;
    view.row_stride_bytes = kSide;
    view.format = PixelFormat::kGray8;
    return view;
}

const char* evidence_name(VisualEvidenceKind kind) {
    switch (kind) {
        case VisualEvidenceKind::kExactContent:
            return "exact";
        case VisualEvidenceKind::kPerceptualHash:
            return "perceptual";
        case VisualEvidenceKind::kTemplate:
            return "template";
    }
    return "?";
}

}  // namespace

int main() {
    // Enrollment: one entry per known icon state.
    auto index_result = mirador::VisualIndex::create(64 * 1024, kSide);
    if (!index_result.ok()) {
        std::printf("index creation failed\n");
        return 1;
    }
    mirador::VisualIndex index = index_result.take_value();  // queries promote entries (non-const)
    PatchFingerprintParams fingerprint_params;
    fingerprint_params.thumb_side = kSide;

    std::vector<std::byte> icon_on = paint_icon(true);
    std::vector<std::byte> icon_off = paint_icon(false);
    const auto on_fingerprint = mirador::make_visual_patch_fingerprint(view_of(icon_on), fingerprint_params, 1 << 20);
    const auto off_fingerprint = mirador::make_visual_patch_fingerprint(view_of(icon_off), fingerprint_params, 1 << 20);
    if (!on_fingerprint.ok() || !off_fingerprint.ok()) {
        std::printf("fingerprinting failed\n");
        return 1;
    }
    if (!index.insert(1001 /* "toggle-on" */, on_fingerprint.value()).ok() ||
        !index.insert(1002 /* "toggle-off" */, off_fingerprint.value()).ok()) {
        std::printf("insert failed\n");
        return 1;
    }

    // A later frame: the same icons under deterministic noise.
    perturb(icon_on);
    perturb(icon_off);
    const auto probe_on = mirador::make_visual_patch_fingerprint(view_of(icon_on), fingerprint_params, 1 << 20);
    const auto probe_off = mirador::make_visual_patch_fingerprint(view_of(icon_off), fingerprint_params, 1 << 20);
    if (!probe_on.ok() || !probe_off.ok()) {
        std::printf("probe fingerprinting failed\n");
        return 1;
    }

    VisualQueryParams query_params;
    query_params.perceptual_similarity_threshold = 0.9;
    query_params.template_ncc_threshold = 0.9;
    query_params.max_candidates = 4;

    // Reuse policy (caller-side, design section 12): only a clear winner at
    // or above 0.95 similarity counts as the same icon; anything weaker stays
    // ambiguous and would be re-recognized by the real pipeline.
    constexpr double kReusePolicyThreshold = 0.95;
    for (const auto& probe : {std::make_pair(1001, probe_on.value()), std::make_pair(1002, probe_off.value())}) {
        const auto hits = index.query(probe.second, query_params);
        if (!hits.ok()) {
            std::printf("query failed: %s\n", hits.status().message().c_str());
            return 1;
        }
        std::printf("probe for state %llu: %zu candidate(s)\n", static_cast<unsigned long long>(probe.first),
                    hits.value().size());
        for (const VisualCandidate& candidate : hits.value()) {
            std::printf("  entry %llu via %s evidence (similarity %.3f)\n",
                        static_cast<unsigned long long>(candidate.entry_id), evidence_name(candidate.evidence),
                        candidate.similarity);
        }
        if (!hits.value().empty() && hits.value()[0].similarity >= kReusePolicyThreshold &&
            (hits.value().size() == 1 || hits.value()[1].similarity < kReusePolicyThreshold)) {
            std::printf("  policy: reuse enrolled state %llu, skip re-recognition\n",
                        static_cast<unsigned long long>(hits.value()[0].entry_id));
        } else {
            std::printf("  policy: ambiguous, re-recognize this icon\n");
        }
    }
    std::printf("icon tour done: %zu enrolled entr(ies), %lld/%lld bytes\n", index.entry_count(),
                static_cast<long long>(index.byte_size()), static_cast<long long>(index.max_bytes()));
    return 0;
}
