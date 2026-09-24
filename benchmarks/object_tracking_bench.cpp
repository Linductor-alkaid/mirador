// M7-09 (DEC-019 section 4): synthetic verification harness and A/B/C/D method
// matrix for the cross-frame object tracker (object-tracking design section 8).
// The harness is the caller-composed frame pipeline over the frozen M7-03..08
// pool primitives: per frame it runs detect_change (M1), the StableIdTracker
// advance (DEC-010, with tracking-confirmed associations for method D), the
// generation trigger and motion compensation (M7-07), the change gate (M7-03),
// cascade redetection over a synthetic oracle Detector (M7-08; RULE-12 — the
// trigger policy lives here in the harness), neighborhood verification and
// evidence commits (M7-05/M7-06), caller adoption and the lag sweep.
//
// Methods (design section 8 matrix): A appearance only (E1+E2, no change-gate
// short-circuit — every non-terminated track is verified every frame), B
// appearance + position gate (no compensation), C = B + global motion
// compensation, D = C + candidate semantics and the StableIdTracker
// confirmed-association passthrough (M7-06).
//
// Scenes (evaluation-scenes IDs; synthetic in-memory renders from seeded
// integer hashes — bit-deterministic, no wall clock in any decision, latency
// metrics are frame counts, RULE-03): linux-static-page, linux-scroll,
// linux-dialog, linux-theme-switch, linux-similar-icons, linux-partial-anim.
// Every persistent widget sits on a rigid block-aligned card so the M1 block
// diff sees sub-block widget motion; the scroll step (48 px) is deliberately
// beyond the frozen verification-ROI reach (~36 px) so B and C separate.
//
// Built-in negative assertions: zero Detector calls on kNone frames, pool
// byte_size() <= pool_budget_bytes after every frame (RULE-06), per-cell
// results bit-identical across repetitions of the same seed; scene
// classifications are asserted so a render drift cannot distort the numbers.
// Timing figures are only meaningful on the machine that ran the harness
// (DEC-011, DOD-05: no real-scene claims).
#include <mirador/change_detection.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/object_tracker.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/shift_estimation.hpp>
#include <mirador/stable_id_tracker.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using mirador::ChangeClassification;
using mirador::ChangeDetectionParams;
using mirador::ChangeReport;
using mirador::detect_change;
using mirador::estimate_global_shift;
using mirador::EvidenceGrade;
using mirador::ImageView;
using mirador::ObjectTracker;
using mirador::ObjectTrackerOptions;
using mirador::PixelFormat;
using mirador::PointF;
using mirador::PositionScenario;
using mirador::RectF;
using mirador::RectI;
using mirador::Result;
using mirador::ShiftEstimate;
using mirador::StableIdTracker;
using mirador::TargetTrack;
using mirador::TrackAdoption;
using mirador::TrackSemantics;
using mirador::TrackState;
using mirador::TrackStructureDescriptors;
using mirador::TrackVerification;
using mirador::VisualRegion;

// ---- frame and widget geometry ----------------------------------------------

constexpr int32_t kWidth = 1280;
constexpr int32_t kHeight = 720;
constexpr int64_t kStride = int64_t{kWidth} * 4;
constexpr int32_t kObjW = 64;
constexpr int32_t kObjH = 32;
constexpr int32_t kCardW = 160;  // one M1 block (frame px); the card fill drives the block diff
constexpr int32_t kCardH = 90;
constexpr int32_t kScrollStep = 48;  // frame px per scroll frame (beyond the ~36 px ROI reach)
constexpr int32_t kScrollUntil = 6;  // frames 1..6 scroll
constexpr int32_t kDialogOpenAt = 10;
constexpr int32_t kDialogCloseAt = 25;
constexpr int32_t kThemeAt = 12;
constexpr int32_t kTooltipFrom = 12;  // similar-icons popup frames [12, 18)
constexpr int32_t kTooltipTo = 18;
constexpr int32_t kAnimFrom = 8;  // spinner-on frames [8, 12)
constexpr int32_t kAnimTo = 12;
constexpr int kRepsDefault = 3;
constexpr int kDialogCell = 48;  // modal ambience checkerboard cell (frame px)
constexpr int kDialogAmp = 24;
constexpr int kFpsHint = 60;  // synthetic fps behind the per-minute Detector rate

// ---- deterministic content hashing (seeded; no unseeded randomness) ----------

uint64_t mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint32_t hash_cell(const int32_t x, const int32_t y, const uint64_t seed) {
    return static_cast<uint32_t>(
        mix64(static_cast<uint64_t>(x) * 0x2545f4914f6cdd1dULL + static_cast<uint64_t>(y) * 0x9e3779b1ULL + seed) >>
        33);
}

// ---- frame buffer ------------------------------------------------------------

struct RgbaFrame {
    std::vector<std::byte> bytes;
    ImageView view;
};

RgbaFrame make_frame() {
    RgbaFrame frame;
    frame.bytes.assign(static_cast<size_t>(kStride) * kHeight, std::byte{0});
    frame.view.data = frame.bytes.data();
    frame.view.width = kWidth;
    frame.view.height = kHeight;
    frame.view.row_stride_bytes = kStride;
    frame.view.format = PixelFormat::kRgba8;
    return frame;
}

void set_px(RgbaFrame& frame, const int32_t x, const int32_t y, const int value) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) {
        return;
    }
    const auto clamped = static_cast<uint8_t>(std::clamp(value, 0, 255));
    std::byte* row = frame.bytes.data() + static_cast<int64_t>(y) * kStride;
    const int64_t base = static_cast<int64_t>(x) * 4;
    row[base + 0] = static_cast<std::byte>(clamped);
    row[base + 1] = static_cast<std::byte>(clamped);
    row[base + 2] = static_cast<std::byte>(clamped);
    row[base + 3] = static_cast<std::byte>(255);
}

int luma_at(const ImageView& view, const int32_t x, const int32_t y) {
    const std::byte* row = view.data + static_cast<int64_t>(y) * view.row_stride_bytes;
    const int64_t base = static_cast<int64_t>(x) * 4;
    const int r = std::to_integer<int>(row[base + 0]);
    const int g = std::to_integer<int>(row[base + 1]);
    const int b = std::to_integer<int>(row[base + 2]);
    return (77 * r + 150 * g + 29 * b) >> 8;
}

// ---- widget and overlay rasterizers -------------------------------------------

void paint_background(RgbaFrame& frame) {
    for (int32_t y = 0; y < kHeight; ++y) {
        for (int32_t x = 0; x < kWidth; ++x) {
            set_px(frame, x, y, 24 + (x * 180) / (kWidth - 1));
        }
    }
}

// One UI widget: a 2 px bright border (the E2 edge carrier) over a seeded 4-px
// band texture. The twin-icon variants add one full-width marker bar each
// (same bands, different marker): similar enough for the E1 channel to prefer
// the twin over a degraded true patch, different enough to stay below the
// impostor threshold so the semantics veto and a clean recapture can work.
void draw_widget(RgbaFrame& frame, const RectF& rect, const uint64_t seed, const int variant) {
    const auto x0 = static_cast<int32_t>(rect.x);
    const auto y0 = static_cast<int32_t>(rect.y);
    const auto x1 = static_cast<int32_t>(rect.x + rect.width);
    const auto y1 = static_cast<int32_t>(rect.y + rect.height);
    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
            const bool border = x < x0 + 2 || x >= x1 - 2 || y < y0 + 2 || y >= y1 - 2;
            if (border) {
                set_px(frame, x, y, 235);
                continue;
            }
            const int32_t rx = (x - x0) / 2;
            const int32_t ry = (y - y0) / 2;
            int value = 40 + static_cast<int>(hash_cell(rx, ry, seed) % 6U) * 36;
            if (variant == 1 && y >= y0 + 10 && y < y0 + 14) {
                value = 240;
            }
            if (variant == 2 && y >= y1 - 14 && y < y1 - 10) {
                value = 16;
            }
            set_px(frame, x, y, value);
        }
    }
}

void draw_panel(RgbaFrame& frame, const RectI& rect) {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            const bool border =
                x < rect.x + 2 || x >= rect.x + rect.width - 2 || y < rect.y + 2 || y >= rect.y + rect.height - 2;
            int value = 90;
            if (border) {
                value = 235;
            } else if (y < rect.y + 26) {
                value = 205;
            }
            set_px(frame, x, y, value);
        }
    }
}

// Animated preview window (the partial-anim overlay): a bright pane with a
// moving-highlight stripe pattern. Its cell means rise ABOVE the card and
// background gradient, so the coarse 9x8 dHash reorders when it appears.
void draw_anim_window(RgbaFrame& frame, const RectI& rect) {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            set_px(frame, x, y, ((x / 8 + y / 8) % 4 == 0) ? 255 : 225);
        }
    }
}

// Modal-dialog ambience: a coarse brightness checkerboard over the whole
// frame. The 48 px cells survive the change detector's coarse scales, so the
// modal event classifies kGlobal (the per-frame dHash and every block diff
// respond); while the modal is open the pattern is static, so the open period
// itself classifies kNone.
void apply_noise(RgbaFrame& frame) {
    for (int32_t y = 0; y < kHeight; ++y) {
        for (int32_t x = 0; x < kWidth; ++x) {
            const int shift = ((x / kDialogCell + y / kDialogCell) % 2 == 0) ? kDialogAmp : -kDialogAmp;
            const std::byte* row = frame.bytes.data() + static_cast<int64_t>(y) * kStride;
            const int value = std::to_integer<int>(row[static_cast<int64_t>(x) * 4]) + shift;
            set_px(frame, x, y, value);
        }
    }
}

void apply_invert(RgbaFrame& frame) {
    for (int32_t y = 0; y < kHeight; ++y) {
        for (int32_t x = 0; x < kWidth; ++x) {
            const std::byte* row = frame.bytes.data() + static_cast<int64_t>(y) * kStride;
            set_px(frame, x, y, 255 - std::to_integer<int>(row[static_cast<int64_t>(x) * 4]));
        }
    }
}

// ---- scene model -------------------------------------------------------------

enum class SceneKind : std::uint8_t { kStaticPage, kScroll, kDialog, kThemeSwitch, kSimilarIcons, kPartialAnim };

struct GtObject {
    RectF base;
    const char* label;
    uint64_t seed;
    int variant;  // 0 plain; 1/2 the twin-icon marker bars
    bool adoptable;
    bool behind_panel;    // dialog scene: occluded while the modal is open
    bool card;            // drawn on a block-aligned card that moves rigidly with it
    int32_t card_dx = 0;  // card offset frozen at scene construction (never re-snapped)
    int32_t card_dy = 0;
    int32_t card_w = 160;
    int32_t card_h = 90;
    int card_fill = 200;
};

struct FrameTruth {
    std::vector<RectF> rects;
    std::vector<bool> visible;
    bool panel = false;
    bool noise = false;
    bool invert = false;
    bool spinner = false;
};

struct SceneSpec {
    const char* name;
    SceneKind kind;
    int frames;
    std::vector<GtObject> objects;
};

RectF rect_at(const int32_t x, const int32_t y) {
    return RectF{static_cast<float>(x), static_cast<float>(y), static_cast<float>(kObjW), static_cast<float>(kObjH)};
}

// Block-aligned card behind one persistent widget: a 160x90 list-item face
// whose solid fill dominates its frame blocks, so the change detector's block
// diff sees the card move even though the 64x32 widget itself is sub-block.
// The alignment is frozen once per object so the card never re-snaps across
// block boundaries — it translates exactly with the widget.
GtObject with_card(GtObject object, const int fill = 200, const int32_t w = kCardW, const int32_t h = kCardH) {
    object.card = true;
    object.card_w = w;
    object.card_h = h;
    object.card_fill = fill;
    if (w == kCardW && h == kCardH) {
        // Block-aligned card: center the widget in one M1 block, frozen so the
        // card never re-snaps across block boundaries while it moves.
        const auto cx = static_cast<int32_t>(object.base.x) - 48;
        const auto cy = static_cast<int32_t>(object.base.y) - 29;
        object.card_dx = cx / kCardW * kCardW - static_cast<int32_t>(object.base.x);
        object.card_dy = cy / kCardH * kCardH - static_cast<int32_t>(object.base.y);
    } else {
        // Non-block-sized cards (the popup) just center rigidly on the widget.
        object.card_dx = static_cast<int32_t>(object.base.x) - w / 2 + kObjW / 2 - static_cast<int32_t>(object.base.x);
        object.card_dy = static_cast<int32_t>(object.base.y) - h / 2 + kObjH / 2 - static_cast<int32_t>(object.base.y);
    }
    return object;
}

void draw_card(RgbaFrame& frame, const GtObject& object, const RectF& widget) {
    const auto cx = static_cast<int32_t>(widget.x) + object.card_dx;
    const auto cy = static_cast<int32_t>(widget.y) + object.card_dy;
    for (int32_t y = cy; y < cy + object.card_h; ++y) {
        for (int32_t x = cx; x < cx + object.card_w; ++x) {
            set_px(frame, x, y, object.card_fill);
        }
    }
}

std::vector<GtObject> grid_six() {
    constexpr std::array<int32_t, 6> xs = {140, 400, 660, 140, 400, 920};
    constexpr std::array<int32_t, 6> ys = {120, 120, 120, 520, 520, 320};
    std::vector<GtObject> objects;
    objects.reserve(6);
    for (int i = 0; i < 6; ++i) {
        objects.push_back(with_card(
            GtObject{rect_at(xs[i], ys[i]), "button", mix64(static_cast<uint64_t>(i) + 11), 0, true, false, false}));
    }
    return objects;
}

SceneSpec make_scene(const SceneKind kind) {
    SceneSpec spec;
    spec.kind = kind;
    switch (kind) {
        case SceneKind::kStaticPage:
            spec.name = "linux-static-page";
            spec.frames = 40;
            spec.objects = grid_six();
            break;
        case SceneKind::kScroll: {
            spec.name = "linux-scroll";
            spec.frames = 30;
            constexpr std::array<int32_t, 6> xs = {140, 400, 660, 140, 400, 920};
            constexpr std::array<int32_t, 6> ys = {400, 400, 400, 560, 560, 560};
            for (int i = 0; i < 6; ++i) {
                spec.objects.push_back(with_card(GtObject{
                    rect_at(xs[i], ys[i]), "button", mix64(static_cast<uint64_t>(i) + 11), 0, true, false, false}));
            }
            break;
        }
        case SceneKind::kDialog:
            spec.name = "linux-dialog";
            spec.frames = 40;
            spec.objects = {
                with_card(GtObject{rect_at(140, 120), "button", mix64(21), 0, true, false, false}),
                with_card(GtObject{rect_at(140, 520), "button", mix64(22), 0, true, false, false}),
                with_card(GtObject{rect_at(1000, 120), "button", mix64(23), 0, true, false, false}),
                with_card(GtObject{rect_at(1000, 520), "button", mix64(24), 0, true, false, false}),
                with_card(GtObject{rect_at(560, 300), "button", mix64(25), 0, true, true, false}),  // behind the panel
                GtObject{rect_at(616, 342), "dialog-button", mix64(26), 0, true, false, false},
            };
            break;
        case SceneKind::kThemeSwitch:
            spec.name = "linux-theme-switch";
            spec.frames = 30;
            spec.objects = grid_six();
            break;
        case SceneKind::kSimilarIcons:
            spec.name = "linux-similar-icons";
            spec.frames = 30;
            spec.objects = {
                with_card(GtObject{rect_at(560, 300), "icon-alpha", mix64(31), 1, true, false, false}),
                // twin bands, own marker
                with_card(GtObject{rect_at(900, 300), "icon-beta", mix64(31), 2, true, false, false}),
                with_card(GtObject{rect_at(140, 120), "button", mix64(33), 0, true, false, false}),
                with_card(GtObject{rect_at(1000, 520), "button", mix64(34), 0, true, false, false}),
                // popup distractor: light-theme panel drawn over the alpha widget;
                // bright fill so the coarse dHash cells reorder (kPartial anchor)
                with_card(GtObject{rect_at(584, 300), "icon-beta", mix64(31), 2, false, false, false}, 235, 240, 140),
            };
            break;
        case SceneKind::kPartialAnim:
            spec.name = "linux-partial-anim";
            spec.frames = 24;
            spec.objects = {
                with_card(GtObject{rect_at(560, 300), "button", mix64(41), 0, true, false, false}),
                with_card(GtObject{rect_at(140, 120), "button", mix64(42), 0, true, false, false}),
                with_card(GtObject{rect_at(1000, 120), "button", mix64(43), 0, true, false, false}),
                with_card(GtObject{rect_at(140, 520), "button", mix64(44), 0, true, false, false}),
            };
            break;
    }
    return spec;
}

bool panel_open(const SceneKind kind, const int frame) {
    return kind == SceneKind::kDialog && frame >= kDialogOpenAt && frame < kDialogCloseAt;
}

FrameTruth truth_of(const SceneSpec& scene, const int frame) {
    FrameTruth truth;
    truth.rects.reserve(scene.objects.size());
    truth.visible.reserve(scene.objects.size());
    const bool panel = panel_open(scene.kind, frame);
    for (const GtObject& object : scene.objects) {
        float y = object.base.y;
        if (scene.kind == SceneKind::kScroll && frame >= 1) {
            y -= static_cast<float>(kScrollStep * std::min(frame, kScrollUntil));
        }
        truth.rects.push_back(RectF{object.base.x, y, object.base.width, object.base.height});
        bool visible = true;
        if (object.behind_panel) {
            visible = !panel;
        } else if (std::string(object.label) == "dialog-button") {
            visible = panel;
        } else if (!object.adoptable) {
            visible = frame >= kTooltipFrom && frame < kTooltipTo;
        }
        truth.visible.push_back(visible);
    }
    truth.panel = panel;
    truth.noise = panel;
    truth.invert = scene.kind == SceneKind::kThemeSwitch && frame >= kThemeAt;
    truth.spinner = scene.kind == SceneKind::kPartialAnim && frame >= kAnimFrom && frame < kAnimTo;
    return truth;
}

void render_scene(const SceneSpec& scene, const FrameTruth& truth, RgbaFrame& frame) {
    paint_background(frame);
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        if (truth.visible[i] && scene.objects[i].adoptable && scene.objects[i].card) {
            draw_card(frame, scene.objects[i], truth.rects[i]);
        }
    }
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        if (truth.visible[i] && scene.objects[i].adoptable) {
            draw_widget(frame, truth.rects[i], scene.objects[i].seed, scene.objects[i].variant);
        }
    }
    // Popups render on top of everything persistent (the tooltip covers the
    // widget it pops over — the swap-pressure setup of the similar-icons scene).
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        if (truth.visible[i] && !scene.objects[i].adoptable) {
            if (scene.objects[i].card) {
                draw_card(frame, scene.objects[i], truth.rects[i]);
            }
            draw_widget(frame, truth.rects[i], scene.objects[i].seed, scene.objects[i].variant);
        }
    }
    if (truth.panel) {
        draw_panel(frame, RectI{400, 210, 480, 300});
    }
    if (truth.spinner) {
        draw_anim_window(frame, RectI{488, 276, 240, 154});
    }
    if (truth.noise) {
        apply_noise(frame);
    }
    if (truth.invert) {
        apply_invert(frame);
    }
}

ChangeClassification expected_classification(const SceneSpec& scene, const int frame) {
    const bool partial = (scene.kind == SceneKind::kScroll && frame >= 1 && frame <= kScrollUntil) ||
                         (scene.kind == SceneKind::kSimilarIcons && (frame == kTooltipFrom || frame == kTooltipTo)) ||
                         (scene.kind == SceneKind::kPartialAnim && (frame == kAnimFrom || frame == kAnimTo));
    if (partial) {
        return ChangeClassification::kPartial;
    }
    const bool global = (scene.kind == SceneKind::kDialog && (frame == kDialogOpenAt || frame == kDialogCloseAt)) ||
                        (scene.kind == SceneKind::kThemeSwitch && frame == kThemeAt);
    if (global) {
        return ChangeClassification::kGlobal;
    }
    return ChangeClassification::kNone;
}

bool phase_is_static(const SceneSpec& scene, const int frame) {
    switch (scene.kind) {
        case SceneKind::kStaticPage:
            return true;
        case SceneKind::kScroll:
            return frame > kScrollUntil;
        case SceneKind::kDialog:
            return frame < kDialogOpenAt || frame > kDialogCloseAt;
        case SceneKind::kThemeSwitch:
            return frame != kThemeAt;
        case SceneKind::kSimilarIcons:
            return frame < kTooltipFrom || frame > kTooltipTo;
        case SceneKind::kPartialAnim:
            return frame < kAnimFrom || frame > kAnimTo;
    }
    return true;
}

// ---- E2 structure oracle (edge-map descriptors; theme-inversion invariant) ----

bool edge_at(const ImageView& view, const int32_t x, const int32_t y) {
    const int center = luma_at(view, x, y);
    return std::abs(luma_at(view, x + 1, y) - center) >= 16 || std::abs(luma_at(view, x, y + 1) - center) >= 16;
}

int edge_neighbour_count(const ImageView& view, const int32_t x, const int32_t y) {
    int neighbours = 0;
    for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            if ((dx != 0 || dy != 0) && edge_at(view, x + dx, y + dy)) {
                ++neighbours;
            }
        }
    }
    return neighbours;
}

void accumulate_edge_stats(const ImageView& view, const RectI& roi, int& edges, int& aligned, int& junctions) {
    const int32_t x1 = std::min(roi.x + roi.width, view.width - 1);
    const int32_t y1 = std::min(roi.y + roi.height, view.height - 1);
    for (int32_t y = std::max(roi.y, 0); y < y1; ++y) {
        for (int32_t x = std::max(roi.x, 0); x < x1; ++x) {
            if (!edge_at(view, x, y)) {
                continue;
            }
            ++edges;
            if (edge_at(view, x + 1, y) || edge_at(view, x - 1, y) || edge_at(view, x, y + 1) ||
                edge_at(view, x, y - 1)) {
                ++aligned;
            }
            junctions += edge_neighbour_count(view, x, y) >= 3 ? 1 : 0;
        }
    }
}

// Closure: the fraction of the track bounds' pixel perimeter that carries
// edges — near one for an aligned widget, near zero once it moved away.
RectI bounds_perimeter(const RectI& roi, const RectF& bounds) {
    const auto bx0 = std::max(roi.x, static_cast<int32_t>(bounds.x));
    const auto by0 = std::max(roi.y, static_cast<int32_t>(bounds.y));
    const auto bx1 = std::min(roi.x + roi.width, static_cast<int32_t>(bounds.x + bounds.width));
    const auto by1 = std::min(roi.y + roi.height, static_cast<int32_t>(bounds.y + bounds.height));
    return RectI{bx0, by0, std::max(0, bx1 - bx0), std::max(0, by1 - by0)};
}

double perimeter_closure(const ImageView& view, const RectI& ring) {
    if (ring.width <= 0 || ring.height <= 0) {
        return 0.0;
    }
    const int32_t x1 = ring.x + ring.width;
    const int32_t y1 = ring.y + ring.height;
    const int perimeter = 2 * ring.width + 2 * (ring.height - 2);
    int perimeter_edges = 0;
    for (int32_t x = ring.x; x < x1; ++x) {
        perimeter_edges += (edge_at(view, x, ring.y) || edge_at(view, x, y1 - 1)) ? 2 : 0;
    }
    for (int32_t y = ring.y + 1; y < y1 - 1; ++y) {
        perimeter_edges += (edge_at(view, ring.x, y) || edge_at(view, x1 - 1, y)) ? 2 : 0;
    }
    return perimeter == 0 ? 0.0 : static_cast<double>(perimeter_edges) / perimeter;
}

TrackStructureDescriptors measure_structure(const ImageView& view, const RectI& roi, const RectF& bounds) {
    int edges = 0;
    int aligned = 0;
    int junctions = 0;
    accumulate_edge_stats(view, roi, edges, aligned, junctions);
    TrackStructureDescriptors descriptors;
    descriptors.closure_score = static_cast<float>(perimeter_closure(view, bounds_perimeter(roi, bounds)));
    descriptors.rectangularity = edges == 0 ? 0.0F : static_cast<float>(static_cast<double>(aligned) / edges);
    descriptors.edge_support = edges == 0 ? 0.0F : static_cast<float>(static_cast<double>(junctions) / edges);
    return descriptors;
}

// ---- methods ------------------------------------------------------------------

struct MethodConfig {
    const char* name;
    bool change_gate;   // B/C/D consume evaluate_change_gate; A verifies every frame
    bool compensation;  // C/D run estimate_global_shift + compensate_global_motion on kPartial
    bool semantics;     // D supplies candidate semantics and fusion associations
};

constexpr std::array<MethodConfig, 4> kMethods = {{
    {"A-appearance", false, false, false},
    {"B-pos-gate", true, false, false},
    {"C-compensated", true, true, false},
    {"D-semantic", true, true, true},
}};

// ---- metrics ------------------------------------------------------------------

struct CellMetrics {
    int frames = 0;
    int objects = 0;
    int detector_calls = 0;
    int detector_calls_on_static = 0;
    int redetect_attempts = 0;
    int redetect_failures = 0;
    int recaptures = 0;
    int associations = 0;
    int loss_events = 0;
    int64_t recapture_latency_sum = 0;
    int recapture_latency_max = 0;
    int cont_static_num = 0;
    int cont_static_den = 0;
    int cont_dyn_num = 0;
    int cont_dyn_den = 0;
    int swaps = 0;
    int confirming_commits = 0;
    int confirming_wrong_object = 0;
    int false_loss_frames = 0;
    int missed_loss_frames = 0;
    int impostor_hits = 0;
    int semantic_vetoes = 0;
    int theme_e2_carries = 0;
    int placeholder_commits = 0;
    int64_t verify_calls = 0;
    int64_t verify_ns_sum = 0;
    int64_t pool_peak_bytes = 0;
    int64_t pool_peak_tracks = 0;
    int64_t fusion_regions = 0;
    int64_t fusion_retained = 0;
    double shift_confidence_min = 2.0;
    double shift_confidence_max = -1.0;
    double true_peak_min = 2.0;
    double psr_min = 1e18;
    double e2_dev_max = 0.0;
};

void digest_append(std::string& digest, const int64_t value) {
    digest += std::to_string(value) + "|";
}

std::string metrics_digest(const CellMetrics& m) {
    std::string digest;
    digest_append(digest, m.frames);
    digest_append(digest, m.detector_calls);
    digest_append(digest, m.detector_calls_on_static);
    digest_append(digest, m.redetect_attempts);
    digest_append(digest, m.redetect_failures);
    digest_append(digest, m.recaptures);
    digest_append(digest, m.associations);
    digest_append(digest, m.loss_events);
    digest_append(digest, m.recapture_latency_sum);
    digest_append(digest, m.recapture_latency_max);
    digest_append(digest, m.cont_static_num);
    digest_append(digest, m.cont_static_den);
    digest_append(digest, m.cont_dyn_num);
    digest_append(digest, m.cont_dyn_den);
    digest_append(digest, m.swaps);
    digest_append(digest, m.confirming_commits);
    digest_append(digest, m.confirming_wrong_object);
    digest_append(digest, m.false_loss_frames);
    digest_append(digest, m.missed_loss_frames);
    digest_append(digest, m.impostor_hits);
    digest_append(digest, m.semantic_vetoes);
    digest_append(digest, m.theme_e2_carries);
    digest_append(digest, m.placeholder_commits);
    digest_append(digest, m.verify_calls);
    // verify_ns_sum is deliberately excluded: wall-clock timing is the only
    // non-deterministic quantity here (latency metrics otherwise count frames,
    // RULE-03).
    digest_append(digest, m.pool_peak_bytes);
    digest_append(digest, m.pool_peak_tracks);
    digest_append(digest, m.fusion_regions);
    digest_append(digest, m.fusion_retained);
    return digest;
}

// ---- fail-fast helpers ----------------------------------------------------------

[[noreturn]] void die(const char* what, const char* detail) {
    std::fprintf(stderr, "harness failure: %s (%s)\n", what, detail);
    std::exit(1);
}

template <typename T>
void expect_ok(const Result<T>& result, const char* what) {
    if (!result.ok()) {
        die(what, result.status().message().c_str());
    }
}

template <typename T>
T take_ok(Result<T>&& result, const char* what) {
    if (!result.ok()) {
        die(what, result.status().message().c_str());
    }
    return result.take_value();
}

void expect_true(const char* what, const bool passed) {
    if (!passed) {
        die(what, "assertion");
    }
}

double percentile_us(const std::vector<int64_t>& samples, const double fraction) {
    if (samples.empty()) {
        return 0.0;
    }
    std::vector<int64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]) / 1000.0;
}

// ---- per-cell runner --------------------------------------------------------------

struct TrackLink {
    uint64_t track_id = 0;
    int object_index = -1;
};

struct CellRun {
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    const SceneSpec& scene;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    const MethodConfig& method;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    ObjectTracker tracker;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    StableIdTracker fusion;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    RgbaFrame prev;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    RgbaFrame curr;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    bool has_prev = false;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::vector<uint64_t> obj_track;    // object -> current track id (0 = none)
                                        // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::vector<uint64_t> obj_initial;  // object -> first track id (continuation reference)
                                        // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::vector<uint64_t> obj_fused;    // object -> fused stable id of the current region
                                        // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::vector<TrackLink> links;       // track -> object (small: linear scans)
                                        // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::vector<std::pair<uint64_t, uint64_t>> lost_seq;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::vector<int64_t> verify_ns;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    CellMetrics m;

    CellRun(const SceneSpec& spec, const MethodConfig& config) : scene(spec), method(config) {
        // Calibrated caller policy (DEC-019 section 5, measured in this
        // harness): the library default min_compensation_confidence 0.0 is
        // kept, but this pipeline gates compensation at 0.7 — true scroll
        // estimates measure confidence [0.93, 0.95] while local-change frames
        // (popups, animations) measure [0.46, 0.55] and must not translate
        // the pool (RISK-2026-17 knob).
        ObjectTrackerOptions options;
        if (config.compensation) {
            options.min_compensation_confidence = 0.7;
        }
        tracker = take_ok(ObjectTracker::create(options), "tracker create");
        const size_t count = scene.objects.size();
        obj_track.assign(count, 0);
        obj_initial.assign(count, 0);
        obj_fused.assign(count, 0);
        prev = make_frame();
        curr = make_frame();
    }
};

const TargetTrack* cell_track(const CellRun& run, const uint64_t id) {
    return run.tracker.find_track(id);
}

int cell_object_of(const CellRun& run, const uint64_t id) {
    for (const TrackLink& link : run.links) {
        if (link.track_id == id) {
            return link.object_index;
        }
    }
    return -1;
}

void cell_bind(CellRun& run, const uint64_t id, const int object_index) {
    for (TrackLink& link : run.links) {
        if (link.track_id == id) {
            link.object_index = object_index;
            return;
        }
    }
    run.links.push_back(TrackLink{id, object_index});
}

// Swap bookkeeping: the identity left `old_object` and (maybe) moved onto
// `new_object`; keep the object->track table consistent with `links`.
void cell_rebind(CellRun& run, const uint64_t id, const int old_object, const int new_object) {
    if (old_object >= 0 && run.obj_track[static_cast<size_t>(old_object)] == id) {
        run.obj_track[static_cast<size_t>(old_object)] = 0;
    }
    if (new_object >= 0) {
        run.obj_track[static_cast<size_t>(new_object)] = id;
    }
    cell_bind(run, id, new_object);
}

uint64_t cell_lost_sequence_of(const CellRun& run, const uint64_t id) {
    for (const auto& entry : run.lost_seq) {
        if (entry.first == id) {
            return entry.second;
        }
    }
    return 0;
}

void cell_set_lost_sequence(CellRun& run, const uint64_t id, const uint64_t sequence) {
    for (auto& entry : run.lost_seq) {
        if (entry.first == id) {
            entry.second = sequence;
            return;
        }
    }
    run.lost_seq.emplace_back(id, sequence);
}

void cell_clear_lost_sequence(CellRun& run, const uint64_t id) {
    run.lost_seq.erase(
        std::remove_if(run.lost_seq.begin(), run.lost_seq.end(), [id](const auto& entry) { return entry.first == id; }),
        run.lost_seq.end());
}

PointF center_of(const RectF& bounds) {
    return PointF{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
}

std::vector<VisualRegion> visible_regions(const CellRun& run, const FrameTruth& truth) {
    std::vector<VisualRegion> regions;
    for (size_t i = 0; i < run.scene.objects.size(); ++i) {
        if (!truth.visible[i] || !run.scene.objects[i].adoptable) {
            continue;
        }
        VisualRegion region;
        region.bounds = truth.rects[i];
        region.anchor = center_of(truth.rects[i]);
        region.label = run.scene.objects[i].label;
        region.confidence = 0.9F;
        regions.push_back(region);
    }
    return regions;
}

void fusion_advance(CellRun& run, const FrameTruth& truth) {
    const std::vector<VisualRegion> regions = visible_regions(run, truth);
    std::vector<mirador::ConfirmedAssociation> associations;
    if (run.method.semantics) {
        for (size_t i = 0; i < run.scene.objects.size(); ++i) {
            const uint64_t id = run.obj_track[i];
            if (id == 0 || !truth.visible[i]) {
                continue;
            }
            const TargetTrack* track = cell_track(run, id);
            if (track == nullptr || track->state != TrackState::kTracking) {
                continue;
            }
            for (size_t r = 0; r < regions.size(); ++r) {
                if (regions[r].bounds == truth.rects[i]) {
                    associations.push_back(mirador::ConfirmedAssociation{r, id});
                    break;
                }
            }
        }
    }
    const mirador::StableIdReport report =
        take_ok(run.fusion.advance(std::span<const VisualRegion>(regions), {}, {}, associations), "fusion advance");
    size_t region_index = 0;
    for (size_t i = 0; i < run.scene.objects.size(); ++i) {
        if (!truth.visible[i] || !run.scene.objects[i].adoptable) {
            continue;
        }
        run.obj_fused[i] = report.assignments[region_index].stable_id;
        ++region_index;
    }
    run.m.fusion_regions += static_cast<int64_t>(regions.size());
    run.m.fusion_retained += static_cast<int64_t>(report.retained_count);
}

void adopt_object(CellRun& run, const size_t index, const FrameTruth& truth, const uint64_t sequence) {
    const std::string adopt_what = std::string("adopt_track [") + run.scene.name + " " + run.method.name + "]";
    VisualRegion region;
    region.bounds = truth.rects[index];
    region.anchor = center_of(truth.rects[index]);
    region.label = run.scene.objects[index].label;
    region.confidence = 0.9F;
    region.stable_id = run.obj_fused[index];
    const TrackAdoption adoption =
        take_ok(run.tracker.adopt_track(region, run.curr.view, sequence), adopt_what.c_str());
    run.obj_track[index] = adoption.track_id;
    cell_bind(run, adoption.track_id, static_cast<int>(index));
    if (run.obj_initial[index] == 0) {
        run.obj_initial[index] = adoption.track_id;
    }
    const RectI roi = take_ok(run.tracker.verification_roi(adoption.track_id, run.curr.view), "verification_roi");
    const TrackStructureDescriptors baseline = measure_structure(run.curr.view, roi, truth.rects[index]);
    expect_ok(run.tracker.record_structure_baseline(adoption.track_id, baseline, sequence),
              "record_structure_baseline");
}

void adopt_visible_objects(CellRun& run, const FrameTruth& truth, const uint64_t sequence) {
    for (size_t i = 0; i < run.scene.objects.size(); ++i) {
        if (!truth.visible[i] || !run.scene.objects[i].adoptable || run.obj_track[i] != 0) {
            continue;
        }
        // The fused id may still be claimed by a pool track whose identity
        // swapped onto another element — that identity is alive, adoption of
        // the same id is impossible and pointless (the pool owns the id).
        bool id_claimed = false;
        for (const uint64_t id : run.tracker.track_ids()) {
            if (id == run.obj_fused[i]) {
                id_claimed = true;
            }
        }
        if (id_claimed) {
            continue;
        }
        adopt_object(run, i, truth, sequence);
    }
}

// GT object the bounds sit on, by maximum rect overlap (the twin-icon popup
// deliberately overlaps the widget it covers, so first-containment would be
// ambiguous); -1 when nothing overlaps.
int object_at_bounds(const CellRun& run, const FrameTruth& truth, const RectF& bounds) {
    int best = -1;
    double best_overlap = 0.0;
    for (size_t i = 0; i < run.scene.objects.size(); ++i) {
        if (!truth.visible[i]) {
            continue;
        }
        const RectF& rect = truth.rects[i];
        const double overlap_x = std::min(bounds.x + bounds.width, rect.x + rect.width) - std::max(bounds.x, rect.x);
        const double overlap_y = std::min(bounds.y + bounds.height, rect.y + rect.height) - std::max(bounds.y, rect.y);
        if (overlap_x <= 0.0 || overlap_y <= 0.0) {
            continue;
        }
        const double overlap = overlap_x * overlap_y;
        if (overlap > best_overlap) {
            best_overlap = overlap;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::optional<TrackSemantics> candidate_semantics(const CellRun& run, const FrameTruth& truth,
                                                  const TrackVerification& verification, const RectF& bounds) {
    if (!run.method.semantics) {
        return std::nullopt;
    }
    RectF window = bounds;
    if (verification.appearance.outcome != mirador::AppearanceChannelOutcome::kNone) {
        window.x += static_cast<float>(verification.appearance.best_offset_dx);
        window.y += static_cast<float>(verification.appearance.best_offset_dy);
    }
    const int index = object_at_bounds(run, truth, window);
    if (index < 0) {
        return std::nullopt;
    }
    return TrackSemantics{run.scene.objects[static_cast<size_t>(index)].label, ""};
}

PositionScenario frame_scenario(const ChangeClassification classification, const bool comp_applied) {
    if (classification == ChangeClassification::kGlobal) {
        return PositionScenario::kGenerationSwitch;
    }
    return comp_applied ? PositionScenario::kCompensatedScroll : PositionScenario::kStationary;
}

void reconcile_commit(CellRun& run, const FrameTruth& truth, const uint64_t track_id,
                      const mirador::TrackEvidenceCommit& commit, const uint64_t sequence) {
    if (commit.state == TrackState::kLost && commit.previous_state != TrackState::kLost) {
        cell_set_lost_sequence(run, track_id, sequence);
        ++run.m.loss_events;
    }
    if (commit.impostor_hit) {
        ++run.m.impostor_hits;
    }
    if (commit.semantics_conflict) {
        ++run.m.semantic_vetoes;
    }
    const bool confirming = commit.grade == EvidenceGrade::kConfirmed || commit.grade == EvidenceGrade::kTentative;
    if (!confirming) {
        if (commit.grade == EvidenceGrade::kPlaceholder) {
            ++run.m.placeholder_commits;
        }
        return;
    }
    ++run.m.confirming_commits;
    const TargetTrack* track = cell_track(run, track_id);
    if (track == nullptr) {
        return;
    }
    const int index = object_at_bounds(run, truth, track->last_bounds);
    const int assigned = cell_object_of(run, track_id);
    if (index != assigned) {
        ++run.m.confirming_wrong_object;
        ++run.m.swaps;
        cell_rebind(run, track_id, assigned, index);
        return;
    }
    const auto peak = static_cast<double>(track->confidence);
    if (peak > 0.0 && peak < run.m.true_peak_min) {
        run.m.true_peak_min = peak;
    }
}

bool compensate_step(CellRun& run, const ChangeClassification classification) {
    if (!run.method.compensation || classification != ChangeClassification::kPartial) {
        return false;
    }
    const ShiftEstimate estimate = take_ok(
        estimate_global_shift(run.prev.view, run.curr.view, mirador::ShiftEstimationParams{}), "estimate_global_shift");
    run.m.shift_confidence_min = std::min(run.m.shift_confidence_min, static_cast<double>(estimate.confidence));
    run.m.shift_confidence_max = std::max(run.m.shift_confidence_max, static_cast<double>(estimate.confidence));
    const mirador::MotionCompensationResult compensation =
        take_ok(run.tracker.compensate_global_motion(estimate), "compensate_global_motion");
    return compensation.applied;
}

TrackVerification timed_review(CellRun& run, const uint64_t id, const TrackStructureDescriptors& descriptors) {
    const auto begin = std::chrono::steady_clock::now();
    const TrackVerification verification =
        take_ok(run.tracker.verify_track(id, run.curr.view, descriptors), "verify_track");
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin);
    run.verify_ns.push_back(elapsed.count());
    ++run.m.verify_calls;
    run.m.verify_ns_sum += elapsed.count();
    return verification;
}

void record_channel_evidence(CellRun& run, const ChangeClassification classification,
                             const TrackVerification& verification) {
    if (verification.appearance.outcome != mirador::AppearanceChannelOutcome::kNone) {
        run.m.psr_min = std::min(run.m.psr_min, verification.appearance.peak_sidelobe_ratio);
    }
    run.m.e2_dev_max = std::max(run.m.e2_dev_max, verification.structure.max_deviation);
    if (classification == ChangeClassification::kGlobal &&
        verification.appearance.outcome == mirador::AppearanceChannelOutcome::kNone &&
        verification.structure.outcome == mirador::StructureChannelOutcome::kConsistent) {
        ++run.m.theme_e2_carries;
    }
}

void redetect_new_id_branch(CellRun& run, const FrameTruth& truth, const uint64_t sequence, const uint64_t id,
                            const int index) {
    ++run.m.redetect_failures;
    const mirador::RedetectionFailureRecord failure =
        take_ok(run.tracker.record_redetection_failure(id, sequence), "failure record");
    const bool present = truth.visible[static_cast<size_t>(index)];
    if (failure.state == TrackState::kTerminated || !present) {
        return;
    }
    // Coarse recall found the object but the review did not confirm it: the
    // new-id branch adopts through the normal fusion path (DEC-010).
    VisualRegion region;
    region.bounds = truth.rects[static_cast<size_t>(index)];
    region.anchor = center_of(region.bounds);
    region.label = run.scene.objects[static_cast<size_t>(index)].label;
    region.confidence = 0.9F;
    region.stable_id = run.obj_fused[static_cast<size_t>(index)];
    const std::string adopt_what = std::string("new-id adopt [") + run.scene.name + " " + run.method.name + "]";
    const TrackAdoption adoption =
        take_ok(run.tracker.adopt_track(region, run.curr.view, sequence), adopt_what.c_str());
    run.obj_track[static_cast<size_t>(index)] = adoption.track_id;
    cell_bind(run, adoption.track_id, index);
    expect_ok(run.tracker.record_redetection_association(id, adoption.track_id, sequence), "association record");
    ++run.m.associations;
}

void run_redetect_attempts(CellRun& run, const ChangeClassification classification, const FrameTruth& truth,
                           const uint64_t sequence, const std::vector<uint64_t>& triggered) {
    for (const uint64_t id : triggered) {
        ++run.m.redetect_attempts;
        const TargetTrack* track = cell_track(run, id);
        const int index = cell_object_of(run, id);
        if (track == nullptr || index < 0) {
            continue;
        }
        const RectI roi = take_ok(run.tracker.verification_roi(id, run.curr.view), "verification_roi");
        const TrackStructureDescriptors descriptors = measure_structure(run.curr.view, roi, track->last_bounds);
        const TrackVerification review = timed_review(run, id, descriptors);
        record_channel_evidence(run, classification, review);
        const mirador::TrackEvidenceCommit commit =
            take_ok(run.tracker.commit_track_evidence(
                        id, review, mirador::TrackPositionEvidence{frame_scenario(classification, false), true},
                        candidate_semantics(run, truth, review, track->last_bounds), run.curr.view, sequence),
                    "redetection commit");
        reconcile_commit(run, truth, id, commit, sequence);
        if (commit.grade == EvidenceGrade::kConfirmed || commit.grade == EvidenceGrade::kTentative) {
            const uint64_t lost_sequence = cell_lost_sequence_of(run, id);
            expect_ok(run.tracker.record_redetection_recapture(id, sequence, lost_sequence), "recapture record");
            const auto latency = static_cast<int64_t>(sequence) - static_cast<int64_t>(lost_sequence);
            run.m.recapture_latency_sum += latency;
            run.m.recapture_latency_max =
                static_cast<int>(std::max<int64_t>(static_cast<int64_t>(run.m.recapture_latency_max), latency));
            ++run.m.recaptures;
            cell_clear_lost_sequence(run, id);
            continue;
        }
        redetect_new_id_branch(run, truth, sequence, id, index);
    }
}

void redetection_step(CellRun& run, const ChangeClassification classification, const FrameTruth& truth,
                      const uint64_t sequence) {
    const std::vector<uint64_t> ids = run.tracker.track_ids();
    std::vector<uint64_t> triggered;
    for (const uint64_t id : ids) {
        const TargetTrack* track = cell_track(run, id);
        if (track == nullptr || track->state != TrackState::kLost) {
            continue;
        }
        const mirador::RedetectionGateDecision gate =
            take_ok(run.tracker.evaluate_redetection_gate(id, classification, sequence), "redetection gate");
        if (gate.verdict == mirador::RedetectionGateVerdict::kTrigger) {
            triggered.push_back(id);
        }
    }
    if (triggered.empty()) {
        return;
    }
    // One batched Detector call serves every triggered track of this frame
    // (the coarse-recall cost model of design section 7).
    ++run.m.detector_calls;
    if (classification == ChangeClassification::kNone) {
        ++run.m.detector_calls_on_static;
    }
    run_redetect_attempts(run, classification, truth, sequence, triggered);
}

void verify_and_commit_one(CellRun& run, const ChangeReport& report, const PositionScenario scenario,
                           const FrameTruth& truth, const uint64_t sequence, const uint64_t id) {
    const TargetTrack* track = cell_track(run, id);
    const RectI roi = take_ok(run.tracker.verification_roi(id, run.curr.view), "verification_roi");
    const TrackStructureDescriptors descriptors = measure_structure(run.curr.view, roi, track->last_bounds);
    const TrackVerification verification = timed_review(run, id, descriptors);
    record_channel_evidence(run, report.classification, verification);
    const std::optional<TrackSemantics> semantics = candidate_semantics(run, truth, verification, track->last_bounds);
    const mirador::TrackEvidenceCommit commit =
        take_ok(run.tracker.commit_track_evidence(id, verification, mirador::TrackPositionEvidence{scenario, true},
                                                  semantics, run.curr.view, sequence),
                "commit_track_evidence");
    reconcile_commit(run, truth, id, commit, sequence);
    if (commit.grade == EvidenceGrade::kConfirmed || commit.grade == EvidenceGrade::kTentative) {
        const TargetTrack* committed = cell_track(run, id);
        if (committed != nullptr) {
            expect_ok(run.tracker.record_observation(id, committed->last_bounds, committed->confidence, sequence),
                      "record_observation");
        }
    }
}

void verification_step(CellRun& run, const ChangeReport& report, const std::vector<mirador::TrackGateDecision>& gates,
                       const bool comp_applied, const FrameTruth& truth, const uint64_t sequence) {
    const PositionScenario scenario = frame_scenario(report.classification, comp_applied);
    const std::vector<uint64_t> ids = run.tracker.track_ids();
    for (const uint64_t id : ids) {
        const TargetTrack* track = cell_track(run, id);
        if (track == nullptr || track->state == TrackState::kLost || track->state == TrackState::kTerminated) {
            continue;
        }
        bool should_verify = false;
        if (!run.method.change_gate || track->state == TrackState::kUncertain) {
            should_verify = true;
        } else {
            for (const mirador::TrackGateDecision& decision : gates) {
                if (decision.track_id == id && decision.decision == mirador::ChangeGateDecision::kVerify) {
                    should_verify = true;
                }
            }
        }
        if (!should_verify) {
            continue;
        }
        verify_and_commit_one(run, report, scenario, truth, sequence, id);
    }
}

bool identity_alive(const CellRun& run, const uint64_t id) {
    const TargetTrack* track = id != 0 ? cell_track(run, id) : nullptr;
    return track != nullptr && (track->state == TrackState::kTracking || track->state == TrackState::kUncertain);
}

void scan_continuation(CellRun& run, const FrameTruth& truth, const bool phase_static) {
    for (size_t i = 0; i < run.scene.objects.size(); ++i) {
        if (!run.scene.objects[i].adoptable || !truth.visible[i] || run.obj_initial[i] == 0) {
            continue;
        }
        const uint64_t id = run.obj_track[i];
        const bool correct = id == run.obj_initial[i] && identity_alive(run, id);
        if (phase_static) {
            ++run.m.cont_static_den;
            run.m.cont_static_num += correct ? 1 : 0;
        } else {
            ++run.m.cont_dyn_den;
            run.m.cont_dyn_num += correct ? 1 : 0;
        }
        const TargetTrack* track = id != 0 ? cell_track(run, id) : nullptr;
        const bool judged_lost = !correct && track != nullptr &&
                                 (track->state == TrackState::kLost || track->state == TrackState::kTerminated);
        if (judged_lost) {
            ++run.m.false_loss_frames;
        }
    }
}

void scan_missed_losses(CellRun& run, const FrameTruth& truth) {
    for (size_t i = 0; i < run.scene.objects.size(); ++i) {
        if (!run.scene.objects[i].adoptable || truth.visible[i] || run.obj_track[i] == 0) {
            continue;
        }
        if (identity_alive(run, run.obj_track[i])) {
            ++run.m.missed_loss_frames;
        }
    }
}

void continuation_scan(CellRun& run, const FrameTruth& truth, const bool phase_static) {
    scan_continuation(run, truth, phase_static);
    scan_missed_losses(run, truth);
}

void frame_step(CellRun& run, const int frame) {
    const FrameTruth truth = truth_of(run.scene, frame);
    if (!run.has_prev) {
        render_scene(run.scene, truth, run.curr);
        run.has_prev = true;
        fusion_advance(run, truth);
        adopt_visible_objects(run, truth, 1);
        run.m.frames = 1;
        run.m.objects = static_cast<int>(run.scene.objects.size());
        run.m.pool_peak_bytes = std::max(run.m.pool_peak_bytes, run.tracker.byte_size());
        run.m.pool_peak_tracks = std::max(run.m.pool_peak_tracks, static_cast<int64_t>(run.tracker.track_count()));
        return;
    }
    std::swap(run.prev, run.curr);
    render_scene(run.scene, truth, run.curr);
    const auto sequence = static_cast<uint64_t>(frame) + 1;
    const ChangeReport report =
        take_ok(detect_change(run.prev.view, run.curr.view, ChangeDetectionParams{}), "detect_change");
    if (report.classification != expected_classification(run.scene, frame)) {
        std::fprintf(stderr, "classification drift: %s frame %d got %d reason %d sim %.4f ratio %.4f\n", run.scene.name,
                     frame, static_cast<int>(report.classification), static_cast<int>(report.reason),
                     report.frame_similarity, report.changed_area_ratio);
        std::exit(1);
    }
    fusion_advance(run, truth);
    expect_ok(run.tracker.advance_generation_for_classification(report.classification), "generation trigger");
    const bool comp_applied = compensate_step(run, report.classification);
    std::vector<mirador::TrackGateDecision> gates;
    if (run.method.change_gate) {
        gates = take_ok(run.tracker.evaluate_change_gate(report), "evaluate_change_gate").tracks;
    }
    redetection_step(run, report.classification, truth, sequence);
    verification_step(run, report, gates, comp_applied, truth, sequence);
    adopt_visible_objects(run, truth, sequence);
    continuation_scan(run, truth, phase_is_static(run.scene, frame));
    expect_ok(run.tracker.sweep_generation_lag(sequence), "sweep_generation_lag");
    expect_true("pool budget exceeded", run.tracker.byte_size() <= run.tracker.options().pool_budget_bytes);
    run.m.frames = frame + 1;
    run.m.pool_peak_bytes = std::max(run.m.pool_peak_bytes, run.tracker.byte_size());
    run.m.pool_peak_tracks = std::max(run.m.pool_peak_tracks, static_cast<int64_t>(run.tracker.track_count()));
}

struct CellOutcome {
    CellMetrics metrics;
    std::vector<int64_t> verify_ns;
};

CellOutcome run_cell(const SceneSpec& scene, const MethodConfig& method) {
    CellRun run(scene, method);
    for (int frame = 0; frame < scene.frames; ++frame) {
        frame_step(run, frame);
    }
    expect_true("static-frame Detector trigger", run.m.detector_calls_on_static == 0);
    return CellOutcome{run.m, run.verify_ns};
}

// ---- self-checks ----------------------------------------------------------------

void self_check_background_y_invariance() {
    RgbaFrame frame = make_frame();
    paint_background(frame);
    for (int32_t x = 0; x < kWidth; x += 97) {
        expect_true("background depends on y", luma_at(frame.view, x, 100) == luma_at(frame.view, x, 600));
    }
}

void self_check_theme_edges_invariant() {
    const SceneSpec scene = make_scene(SceneKind::kThemeSwitch);
    const FrameTruth truth = truth_of(scene, 0);
    RgbaFrame light = make_frame();
    render_scene(scene, truth, light);
    FrameTruth dark_truth = truth;
    dark_truth.invert = true;
    RgbaFrame dark = make_frame();
    render_scene(scene, dark_truth, dark);
    const RectI roi{100, 80, 140, 110};
    expect_true("theme switches the E2 baseline", measure_structure(light.view, roi, scene.objects[0].base) ==
                                                      measure_structure(dark.view, roi, scene.objects[0].base));
}

void self_check_scroll_shift_recovery() {
    const SceneSpec scene = make_scene(SceneKind::kScroll);
    RgbaFrame prev = make_frame();
    RgbaFrame curr = make_frame();
    render_scene(scene, truth_of(scene, 0), prev);
    render_scene(scene, truth_of(scene, 1), curr);
    const ShiftEstimate estimate =
        take_ok(estimate_global_shift(prev.view, curr.view, mirador::ShiftEstimationParams{}), "shift self-check");
    expect_true("scroll shift not recovered", std::abs(estimate.dy + static_cast<float>(kScrollStep)) <= 12.0F);
}

// ---- static-frame short-circuit regression check (DEC-019 section 5) --------

// Per-call samples with the M1 bench protocol (20 warmups, 300 iterations).
template <typename Fn>
std::vector<int64_t> timed_samples(Fn&& callable) {
    for (int i = 0; i < 20; ++i) {
        callable();
    }
    std::vector<int64_t> samples;
    samples.reserve(300);
    for (int i = 0; i < 300; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        callable();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
    }
    return samples;
}

// Acceptance anchor: the B pipeline's static-frame prefix (detect_change ->
// gate -> generation trigger -> sweep) must not regress the M1 detect_change
// baseline. Times detect_change alone and the whole prefix over the same
// adopted pool on an unchanged frame pair.
void static_short_circuit_timing() {
    const SceneSpec scene = make_scene(SceneKind::kStaticPage);
    const FrameTruth truth = truth_of(scene, 0);
    RgbaFrame frame = make_frame();
    render_scene(scene, truth, frame);
    CellRun run(scene, kMethods[1]);  // B: change gate + position, no compensation
    fusion_advance(run, truth);
    adopt_visible_objects(run, truth, 1);
    const auto prefix = [&](const ChangeReport& report) {
        expect_ok(run.tracker.evaluate_change_gate(report), "timing gate");
        expect_ok(run.tracker.advance_generation_for_classification(report.classification), "timing trigger");
        expect_ok(run.tracker.sweep_generation_lag(1), "timing sweep");
    };
    const auto detect_samples = timed_samples(
        [&] { take_ok(detect_change(frame.view, frame.view, ChangeDetectionParams{}), "timing detect"); });
    const auto both_samples = timed_samples([&] {
        const ChangeReport report =
            take_ok(detect_change(frame.view, frame.view, ChangeDetectionParams{}), "timing detect");
        prefix(report);
    });
    std::printf(
        "static short-circuit (kNone, %zu tracks): detect p50=%.2fus p95=%.2fus | "
        "detect+gate+trigger+sweep p50=%.2fus p95=%.2fus\n",
        run.tracker.track_count(), percentile_us(detect_samples, 0.50), percentile_us(detect_samples, 0.95),
        percentile_us(both_samples, 0.50), percentile_us(both_samples, 0.95));
}

// ---- printing ---------------------------------------------------------------------

void print_cell(const SceneSpec& scene, const MethodConfig& method, const CellMetrics& m,
                const std::vector<int64_t>& samples) {
    const double static_rate =
        m.cont_static_den == 0 ? 1.0 : static_cast<double>(m.cont_static_num) / static_cast<double>(m.cont_static_den);
    const double dyn_rate =
        m.cont_dyn_den == 0 ? 1.0 : static_cast<double>(m.cont_dyn_num) / static_cast<double>(m.cont_dyn_den);
    const double fusion_rate =
        m.fusion_regions == 0 ? 1.0 : static_cast<double>(m.fusion_retained) / static_cast<double>(m.fusion_regions);
    const double minutes = static_cast<double>(m.frames) / static_cast<double>(kFpsHint) / 60.0;
    std::printf(
        "%-20s %-14s det=%d att=%d fail=%d recapt=%d lat_mean=%.1f lat_max=%d assoc=%d | cont_stat=%.3f (%d/%d) "
        "cont_dyn=%.3f (%d/%d) | swap=%d fp=%d/%d false_loss=%d missed_loss=%d\n",
        scene.name, method.name, m.detector_calls, m.redetect_attempts, m.redetect_failures, m.recaptures,
        m.recaptures == 0 ? 0.0 : static_cast<double>(m.recapture_latency_sum) / static_cast<double>(m.recaptures),
        m.recapture_latency_max, m.associations, static_rate, m.cont_static_num, m.cont_static_den, dyn_rate,
        m.cont_dyn_num, m.cont_dyn_den, m.swaps, m.confirming_wrong_object, m.confirming_commits, m.false_loss_frames,
        m.missed_loss_frames);
    std::printf(
        "%-20s %-14s verify=%lld p50=%.1fus p95=%.1fus | pool_peak=%lldB tracks_peak=%lld | fusion_ret=%.3f "
        "det_rate=%.2f/min | imp=%d sem=%d ph=%d e2c=%d | conf=[%.2f,%.2f] true_peak>=%.3f psr>=%.2f e2dev<=%.3f\n",
        scene.name, method.name, static_cast<long long>(m.verify_calls), percentile_us(samples, 0.50),
        percentile_us(samples, 0.95), static_cast<long long>(m.pool_peak_bytes),
        static_cast<long long>(m.pool_peak_tracks), fusion_rate,
        minutes <= 0.0 ? 0.0 : static_cast<double>(m.detector_calls) / minutes, m.impostor_hits, m.semantic_vetoes,
        m.placeholder_commits, m.theme_e2_carries, m.shift_confidence_min, m.shift_confidence_max,
        m.true_peak_min > 1.5 ? 0.0 : m.true_peak_min, m.psr_min > 1e17 ? 0.0 : m.psr_min, m.e2_dev_max);
}

}  // namespace

int main(int argc, char** argv) {
    int reps = kRepsDefault;
    if (argc > 1) {
        reps = std::atoi(argv[1]);
    }
    reps = std::max(reps, 2);  // the second repetition doubles as the determinism check
    std::printf("mirador_bench_object_tracking %dx%d RGBA, A/B/C/D x 6 scenes, %d reps (determinism checked)\n", kWidth,
                kHeight, reps);
    std::printf("tracker options: frozen M7 defaults (calibration surface untouched)\n\n");
    self_check_background_y_invariance();
    self_check_theme_edges_invariant();
    self_check_scroll_shift_recovery();
    std::printf("self-checks ok\n\n");
    static_short_circuit_timing();

    const std::vector<SceneSpec> scenes = {make_scene(SceneKind::kStaticPage),   make_scene(SceneKind::kScroll),
                                           make_scene(SceneKind::kDialog),       make_scene(SceneKind::kThemeSwitch),
                                           make_scene(SceneKind::kSimilarIcons), make_scene(SceneKind::kPartialAnim)};

    for (const MethodConfig& method : kMethods) {
        for (const SceneSpec& scene : scenes) {
            CellMetrics base;
            std::vector<int64_t> samples;
            for (int rep = 0; rep < reps; ++rep) {
                const CellOutcome outcome = run_cell(scene, method);
                if (rep == 0) {
                    base = outcome.metrics;
                } else if (metrics_digest(outcome.metrics) != metrics_digest(base)) {
                    die("determinism violation", scene.name);
                }
                samples = outcome.verify_ns;
            }
            print_cell(scene, method, base, samples);
        }
    }
    std::printf(
        "\nDEC-011 qualification: synthetic in-memory scenes, seeded and bit-deterministic;\n"
        "timing valid only on this machine; no real-screenshot claims (DOD-05).\n");
    return 0;
}
