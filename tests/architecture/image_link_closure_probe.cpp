// M1-01: probe executable that forces the mirador-image translation units into
// the link closure so the NEEDED-entry check covers the whole image module.
#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>

int main() {
    auto buffer = mirador::ImageBuffer::create(mirador::PixelFormat::kNv12, 8, 8, 4096);
    if (!buffer.ok()) {
        return 1;
    }
    return mirador::validate(buffer.value().view()).ok() ? 0 : 2;
}
