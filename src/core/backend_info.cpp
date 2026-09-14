#include <mirador/backend_info.hpp>

#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

namespace mirador {

Result<void> validate(const BackendInfo& info) {
    if (info.name.empty()) {
        return Status(ErrorCode::kBackendUnavailable, "backend info has empty name");
    }
    if (info.implementation_version.empty()) {
        return Status(ErrorCode::kBackendUnavailable, "backend info has empty implementation_version");
    }
    if (info.accepted_formats.empty()) {
        return Status(ErrorCode::kBackendUnavailable, "backend info accepts no pixel formats");
    }
    for (const PixelFormat format : info.accepted_formats) {
        if (!is_valid(format)) {
            return Status(ErrorCode::kBackendUnavailable, "backend info carries undefined pixel format");
        }
    }
    return Status::success();
}

}  // namespace mirador
