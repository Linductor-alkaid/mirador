#ifndef MIRADOR_STATUS_HPP
#define MIRADOR_STATUS_HPP

#include <cstdint>
#include <string>
#include <utility>

namespace mirador {

/// Stable error categories for every public failure path (design section 19, DEC-004).
/// The enumeration never leaks runtime- or platform-specific codes; backends may attach
/// implementation-private detail only through Status::message().
enum class ErrorCode : uint8_t {
    kOk = 0,
    kInvalidArgument,      ///< malformed input: null data, non-positive size, invalid ROI or enum value
    kUnsupportedFormat,    ///< pixel format not supported by this code path or backend
    kCoordinateTransform,  ///< degenerate or incomposable transform, or coordinate space mismatch
    kBackendUnavailable,   ///< no backend provided, or its capability/identity query failed
    kBackendFailure,       ///< backend executed and reported failure
    kTimeout,              ///< deadline exceeded (never reported as kCancelled)
    kCancelled,            ///< cancellation observed via ExecutionContext (never a generic failure)
    kCacheCorrupt,         ///< cache entry failed integrity validation
    kBudgetExceeded,       ///< byte, element or iteration budget exhausted
};

/// Short, stable identifier for diagnostics and logs ("Ok", "InvalidArgument", ...).
/// Never returns nullptr; unknown values yield "Unknown".
const char* error_code_name(ErrorCode code) noexcept;

/// Error code plus a short human-readable message. Default construction is success.
/// Success statuses carry no message; error statuses should carry one short line.
class [[nodiscard]] Status {
public:
    Status() noexcept = default;
    Status(ErrorCode code, std::string message) noexcept : code_(code), message_(std::move(message)) {}

    /// Success with no payload. For error paths prefer the (code, message) constructor.
    static Status success() noexcept { return {}; }

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kOk; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    /// Precondition: !ok(). Success statuses return the empty string.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    ErrorCode code_ = ErrorCode::kOk;
    std::string message_;
};

}  // namespace mirador

#endif  // MIRADOR_STATUS_HPP
