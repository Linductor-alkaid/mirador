// M0-05: probe executable that forces every mirador-core translation unit into the
// link closure so the NEEDED-entry check covers the whole core.
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/transform.hpp>

#include <array>
#include <cstddef>

int main() {
    static std::array<std::byte, 64> storage{};
    mirador::ImageView view;
    view.data = storage.data();
    view.width = 4;
    view.height = 4;
    view.row_stride_bytes = 16;
    view.format = mirador::PixelFormat::kRgb8;
    if (!mirador::validate(view).ok()) {
        return 1;
    }
    const auto rotation = mirador::make_rotation(mirador::Rotation::k90, 4, 4, mirador::CoordinateSpaceId::kFrame,
                                                 mirador::CoordinateSpaceId::kOriented);
    const auto recovered = mirador::inverse(rotation);
    return recovered.ok() ? 0 : 2;
}
