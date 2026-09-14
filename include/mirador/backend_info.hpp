#ifndef MIRADOR_BACKEND_INFO_HPP
#define MIRADOR_BACKEND_INFO_HPP

#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <string>
#include <vector>

namespace mirador {

/// Identity and capability of one Backend implementation (design section 9).
/// Returned by value on every `info()` call so implementations can report
/// loaded-model state. All strings participate in capability-result cache keys
/// (RULE-07): a model revision bump therefore invalidates cached results
/// without any explicit flush.
///
/// `thread_safe` is the mandatory concurrency declaration (AGENTS.md sync API
/// boundary, DEC-012): Mirador never locks around Backend calls, so callers
/// may share one Backend across threads only when the implementation sets it.
struct BackendInfo {
    std::string name;                           ///< implementation name, e.g. "ppocr-mobile-fake"
    std::string implementation_version;         ///< version of the adapter/backend code
    std::string model_id;                       ///< logical model identifier (may be empty for model-free backends)
    std::string model_revision;                 ///< revision/hash of the loaded weights
    std::vector<PixelFormat> accepted_formats;  ///< formats `recognize`/`detect` accept, in preference order
    bool thread_safe = false;                   ///< declared by the implementation; default conservative
};

/// Structural validation of BackendInfo (DEC-012 capability gating): non-empty
/// name and implementation version, at least one accepted format, and every
/// format a defined PixelFormat value. Failures report kBackendUnavailable —
/// a malformed capability query means the Backend cannot be used — mirroring
/// the Status enumeration's "capability/identity query failed" semantics.
[[nodiscard]] Result<void> validate(const BackendInfo& info);

}  // namespace mirador

#endif  // MIRADOR_BACKEND_INFO_HPP
