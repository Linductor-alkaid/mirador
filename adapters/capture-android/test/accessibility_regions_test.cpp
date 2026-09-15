// M5-05 host test (runs on any platform; no NDK needed): the pure Android
// adapter conversion utilities — AndroidNodeRegion -> ExternalRegion bounds /
// flag mapping and the DEC-016 projection display transform — including their
// documented failure paths.

#include "accessibility_regions.hpp"
#include "display_transform.hpp"

#include <mirador/geometry.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failed_checks = 0;

void expect_true(bool condition, const std::string& what) {
    if (condition) {
        std::printf("ok: %s\n", what.c_str());
    } else {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failed_checks;
    }
}

void expect_code(const mirador::Status& status, mirador::ErrorCode expected, const std::string& what) {
    const bool matched = !status.ok() && status.code() == expected;
    if (matched) {
        std::printf("ok: %s\n", what.c_str());
    } else {
        std::printf("FAIL: %s (expected %s, got %s)\n", what.c_str(), mirador::error_code_name(expected),
                    status.ok() ? "Ok" : mirador::error_code_name(status.code()));
        ++g_failed_checks;
    }
}

}  // namespace

int main() {
    using mirador::adapters::AndroidNodeRegion;
    using mirador::adapters::projection_display_transform;
    using mirador::adapters::to_external_region;

    // Node -> ExternalRegion: rect convention and flag passthrough.
    AndroidNodeRegion node;
    node.left = 12;
    node.top = 34;
    node.right = 112;
    node.bottom = 84;
    node.text = "Settings";
    node.role = "button";
    node.description = "main entry";
    node.interactive = true;
    node.enabled = false;
    const mirador::Result<mirador::ExternalRegion> converted = to_external_region(node);
    expect_true(converted.ok(), "valid node converts");
    if (converted.ok()) {
        const mirador::ExternalRegion& region = converted.value();
        expect_true(region.bounds == mirador::RectF{12.0F, 34.0F, 100.0F, 50.0F}, "bounds map l/t/w/h");
        expect_true(region.text == "Settings" && region.role == "button" && region.description == "main entry",
                    "strings pass through");
        expect_true(region.interactive && !region.enabled, "flags pass through");
        expect_true(region.confidence == 1.0F, "platform facts default to confidence 1");
    }

    // Degenerate zero-size nodes are kept; inverted ones are rejected.
    AndroidNodeRegion zero = node;
    zero.right = zero.left;
    zero.bottom = zero.top;
    expect_true(to_external_region(zero).ok(), "zero-size node is kept");
    AndroidNodeRegion inverted = node;
    inverted.right = inverted.left - 1;
    expect_code(to_external_region(inverted).status(), mirador::ErrorCode::kInvalidArgument,
                "inverted node bounds rejected");
    AndroidNodeRegion nan_confidence = node;
    nan_confidence.confidence = std::nanf("");
    expect_code(to_external_region(nan_confidence).status(), mirador::ErrorCode::kInvalidArgument,
                "NaN confidence rejected");

    // Projection display transform: buffer 1080x2400 rendered from a
    // 2160x4800 display is a pure 2x scale, kOriented -> kDisplay.
    const mirador::Result<mirador::Transform2D> transform = projection_display_transform(1080, 2400, 2160, 4800);
    expect_true(transform.ok(), "valid projection transform created");
    if (transform.ok()) {
        const mirador::Transform2D& value = transform.value();
        expect_true(value.from == mirador::CoordinateSpaceId::kOriented &&
                        value.to == mirador::CoordinateSpaceId::kDisplay,
                    "transform maps kOriented to kDisplay");
        const mirador::PointF mapped = mirador::transform_point(value, mirador::PointF{540.0F, 1200.0F});
        expect_true(std::abs(mapped.x - 1080.0F) < 1e-6F && std::abs(mapped.y - 2400.0F) < 1e-6F,
                    "buffer center maps to display center");
        // Roundtrip (DEC-016, DOD-03): inverse restores buffer coordinates.
        const mirador::Result<mirador::Transform2D> back = mirador::inverse(value);
        expect_true(back.ok(), "transform invertible");
        if (back.ok()) {
            const mirador::PointF restored = mirador::transform_point(back.value(), mapped);
            expect_true(std::abs(restored.x - 540.0F) < 1e-6F && std::abs(restored.y - 1200.0F) < 1e-6F,
                        "roundtrip restores buffer coordinates");
        }
    }

    // Negative paths: non-positive sizes.
    expect_code(projection_display_transform(0, 100, 100, 100).status(), mirador::ErrorCode::kInvalidArgument,
                "zero buffer width rejected");
    expect_code(projection_display_transform(100, 100, -1, 100).status(), mirador::ErrorCode::kInvalidArgument,
                "negative display width rejected");

    if (g_failed_checks != 0) {
        std::printf("%d check(s) FAILED\n", g_failed_checks);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
