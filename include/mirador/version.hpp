#ifndef MIRADOR_VERSION_HPP
#define MIRADOR_VERSION_HPP

namespace mirador {

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

/// Returns the mirador core version string, e.g. "0.1.0".
const char* version() noexcept;

}  // namespace mirador

#endif  // MIRADOR_VERSION_HPP
