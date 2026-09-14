#ifndef MIRADOR_IMAGE_BUFFER_HPP
#define MIRADOR_IMAGE_BUFFER_HPP

#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirador {

/// Owning, packed CPU image buffer for pipeline intermediates (design section 12
/// frame-cache layer). All planes live in one zero-initialized allocation laid out
/// contiguously per DEC-007 (frozen): the primary plane rows (tight stride
/// `width * bytes_per_pixel`), followed by the NV12 chroma rows (`width +
/// width % 2` bytes per row, `ceil(height / 2)` rows). Move-only; `view()`
/// projects the memory as a valid non-owning ImageView in rotation k0.
class ImageBuffer {
public:
    /// Empty buffer; `view()` of an empty buffer is an invalid view.
    ImageBuffer() noexcept = default;
    ImageBuffer(const ImageBuffer&) = delete;
    ImageBuffer& operator=(const ImageBuffer&) = delete;
    ImageBuffer(ImageBuffer&&) noexcept = default;
    ImageBuffer& operator=(ImageBuffer&&) noexcept = default;
    ~ImageBuffer() noexcept = default;

    /// Allocates a zero-initialized buffer of `format`/`width`/`height`. The request
    /// must fit `max_bytes` (RULE-06: explicit budgets, no unbounded growth); the
    /// size is validated and the budget checked before any allocation. Returns
    /// kInvalidArgument for undefined formats or dimensions outside
    /// [1, kMaxImageDimension], kBudgetExceeded when the request exceeds
    /// `max_bytes` or the allocation itself fails. Never throws.
    [[nodiscard]] static Result<ImageBuffer> create(PixelFormat format, int32_t width, int32_t height,
                                                    int64_t max_bytes) noexcept;

    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    /// Meaningful only when non-empty.
    [[nodiscard]] int32_t width() const noexcept { return width_; }
    [[nodiscard]] int32_t height() const noexcept { return height_; }
    [[nodiscard]] PixelFormat format() const noexcept { return format_; }
    /// Tight stride of the primary plane: `width * bytes_per_pixel(format)`.
    [[nodiscard]] int64_t row_stride_bytes() const noexcept {
        return static_cast<int64_t>(width_) * bytes_per_pixel(format_);
    }
    /// Total allocation size across all planes.
    [[nodiscard]] int64_t byte_size() const noexcept { return byte_size_; }
    /// Writable pointer to the first primary-plane byte, for in-place writers such
    /// as adapters and tests; the const projection is `view()`.
    [[nodiscard]] std::byte* data() noexcept { return data_.data(); }
    /// Projection of the buffer memory; valid per `validate()` when non-empty.
    [[nodiscard]] ImageView view() const noexcept;

private:
    std::vector<std::byte> data_;
    int32_t width_ = 0;
    int32_t height_ = 0;
    PixelFormat format_ = PixelFormat::kGray8;
    int64_t byte_size_ = 0;
};

}  // namespace mirador

#endif  // MIRADOR_IMAGE_BUFFER_HPP
