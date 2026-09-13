#ifndef MIRADOR_IMAGE_VIEW_HPP
#define MIRADOR_IMAGE_VIEW_HPP

#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <cstdint>

namespace mirador {

/// Upper bound for any image dimension; protects against overflow and runaway
/// allocations (design section 20). 65535 covers every terminal capture scenario.
inline constexpr int32_t kMaxImageDimension = 65535;

/// One image plane: non-owning pointer plus row stride in bytes. For single-plane
/// formats the primary plane fields live directly on ImageView (design section 6);
/// for multi-plane formats the chroma plane is carried in ImageView::secondary_plane
/// (DEC-007 draft).
struct ImagePlane {
    const std::byte* data = nullptr;
    int64_t row_stride_bytes = 0;
};

/// Non-owning, format-aware view of CPU image memory. The view never modifies the
/// underlying pixels and does not keep them alive; use Frame::owner for lifetime
/// management. `width`/`height` describe the view as presented (after `rotation`),
/// not the raw capture dimensions.
struct ImageView {
    const std::byte* data = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int64_t row_stride_bytes = 0;
    PixelFormat format = PixelFormat::kRgba8;
    Rotation rotation = Rotation::k0;
    /// Secondary plane for multi-plane formats (NV12 chroma); must stay default for
    /// single-plane formats (DEC-007 draft).
    ImagePlane secondary_plane;
};

/// Structural validation of a view: defined format and rotation, dimensions within
/// [1, kMaxImageDimension], non-null plane data, strides at least
/// min_row_stride_bytes, and DEC-007 plane rules (NV12 requires a populated
/// secondary plane; single-plane formats must not carry one). Returns kInvalidArgument
/// with a reason message on the first violated rule.
[[nodiscard]] Result<void> validate(const ImageView& image);

}  // namespace mirador

#endif  // MIRADOR_IMAGE_VIEW_HPP
