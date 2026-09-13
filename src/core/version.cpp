#include <mirador/version.hpp>

#include <string>

namespace mirador {

const char* version() noexcept {
    static const std::string kVersion =
        std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch);
    return kVersion.c_str();
}

}  // namespace mirador
