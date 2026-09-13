#ifndef MIRADOR_RESULT_HPP
#define MIRADOR_RESULT_HPP

#include <cassert>
#include <optional>
#include <utility>

#include <mirador/status.hpp>

namespace mirador {

/// Value-or-Status return type for public API boundaries (DEC-004). Exceptions never
/// cross the public API; errors travel as Status with a stable ErrorCode.
///
/// - Construction from `T` is the success path; construction from `Status` propagates
///   an error (precondition: the status is not ok, asserted in debug builds).
/// - `value()` and `status()` are precondition-checked accessors, never throwing.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value)  // NOLINT(google-explicit-constructor): ergonomic success propagation
        : value_(std::move(value)) {}
    Result(Status status)  // NOLINT(google-explicit-constructor): ergonomic error propagation
        : status_(std::move(status)) {
        assert(!status_.ok() && "Result error construction requires a non-ok status");
    }

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    /// Precondition: ok().
    [[nodiscard]] const T& value() const noexcept {
        assert(ok() && "value() requires an ok Result");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): precondition asserted above
        return *value_;
    }
    /// Moves the value out; the Result becomes not-ok with an unspecified status.
    /// Precondition: ok().
    T&& take_value() noexcept {
        assert(ok() && "take_value() requires an ok Result");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): precondition asserted above
        return std::move(*value_);
    }
    /// Precondition: !ok(). Calling status() on a value-carrying Result returns a
    /// default success status.
    [[nodiscard]] const Status& status() const noexcept {
        assert(!ok() && "status() requires an error Result");
        return status_;
    }
    /// Returns the carried value or `fallback` when not ok.
    [[nodiscard]] T value_or(T fallback) const { return value_ ? *value_ : std::move(fallback); }

private:
    std::optional<T> value_;
    Status status_;
};

/// Void specialization for operations that either succeed or produce a Status. Here
/// the Status doubles as the full state: a default or ok Status is success.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() noexcept = default;  // NOLINT(google-explicit-constructor)
    Result(Status status)         // NOLINT(google-explicit-constructor)
        : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    Status status_;
};

}  // namespace mirador

#endif  // MIRADOR_RESULT_HPP
