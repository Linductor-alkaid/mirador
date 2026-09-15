#ifndef MIRADOR_SET_OF_MARK_HPP
#define MIRADOR_SET_OF_MARK_HPP

#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirador {

/// Owning packed RGB8 image the renderer produces (design section 17). Render
/// keeps its own pixel representation — by DEC-013 it does not consume
/// `mirador::image`. Pure aggregate; the projection is the free function
/// below.
struct MarkedImage {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<std::byte> pixels;  ///< packed kRgb8 rows, tight stride
};

/// Projects the marked image as a non-owning core ImageView; an empty buffer
/// yields an invalid view.
[[nodiscard]] ImageView marked_image_view(const MarkedImage& image) noexcept;

/// One drawn mark: the display numbering plus the identity it refers to.
/// `mark_id` is the 1-based snapshot region order (deterministic); the VLM
/// returns it and the upper layer maps it back through `stable_id`.
struct SoMMark {
    uint32_t mark_id = 0;
    uint64_t stable_id = 0;
    /// Clamped pixel box actually drawn on the background; empty when the
    /// region fell outside the image (mapping is still emitted).
    RectI drawn_bounds;
    /// Region anchor in background pixel coordinates (design section 17).
    PointF anchor;
};

/// Set-of-Mark output (design section 17): marked image, mark mapping and the
/// snapshot identity it was rendered from.
struct SetOfMarkResult {
    MarkedImage image;
    std::vector<SoMMark> marks;
    uint64_t generation = 0;
    uint64_t frame_sequence = 0;
};

/// Tunables of SoM rendering. Defaults target terminal screenshots; every
/// dimension is deterministic and budget-guarded (RULE-06).
struct SoMRenderOptions {
    /// Output allocation budget in bytes (width * height * 3).
    int64_t max_bytes = int64_t{64} * 1024 * 1024;
    /// Box outline thickness in pixels, in [1, 16].
    int32_t box_thickness = 2;
    /// Label chip height in pixels, in [0, 64]; 0 disables labels.
    int32_t label_height = 14;
    /// Region budget: more regions fail with kBudgetExceeded.
    int32_t max_marks = 256;
};

/// Renders the Set-of-Mark view of `snapshot` over `background` (design
/// section 17): copies the background into an RGB8 buffer, outlines every
/// region with its palette color (indexed by stable_id), places the numeric
/// label chip at the first non-occluded deterministic position (above, below,
/// then inside the box) and returns the `mark_id -> stable_id` mapping. Marks
/// never mutate the snapshot or background (RULE-04); the renderer never
/// calls a VLM and never executes actions.
///
/// `snapshot` regions are interpreted in background pixel coordinates (the
/// caller fuses in the space of the view it renders). `background` must be a
/// valid single-plane 8-bit view in rotation k0 (kNv12 and rotated buffers
/// are rejected; M4 contract).
///
/// Errors: kInvalidArgument (invalid background view, rotated background,
/// invalid options), kUnsupportedFormat (NV12 background), kCancelled/
/// kTimeout from `context`, kBudgetExceeded (output over `max_bytes`, more
/// regions than `max_marks`, allocation failure). Never throws.
[[nodiscard]] Result<SetOfMarkResult> render_set_of_mark(const ImageView& background, const SemanticSnapshot& snapshot,
                                                         const SoMRenderOptions& options = {},
                                                         const ExecutionContext& context = {}) noexcept;

}  // namespace mirador

#endif  // MIRADOR_SET_OF_MARK_HPP
