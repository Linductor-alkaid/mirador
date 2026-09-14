#ifndef MIRADOR_EXECUTION_CONTEXT_HPP
#define MIRADOR_EXECUTION_CONTEXT_HPP

#include <chrono>
#include <functional>
#include <optional>

namespace mirador {

/// Lightweight cancellation and deadline channel for long-running operations
/// (design section 18, DEC-001/DEC-012). The core never creates threads or
/// timers; the caller owns whatever drives the callback (an atomic flag set
/// from another thread, for example) and passes a fresh context on every call.
/// Contexts are cheap value types: the SPI takes them by const reference and
/// pipeline stage boundaries poll them through the helpers below.
struct ExecutionContext {
    /// Returns true when the caller wants the operation abandoned. May be null
    /// (never cancelled). Must be safe to call from the thread executing the
    /// operation and must not block; implementations should flip an atomic.
    std::function<bool()> is_cancelled;
    /// Point in time after which the operation should give up with kTimeout.
    /// Null means no deadline. Uses the steady clock, so it is immune to wall
    /// clock adjustments.
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

/// True when the context carries a cancellation callback and that callback
/// reports cancellation. Never throws: a throwing callback is treated as
/// "cancelled" (the safest interpretation) to keep the no-throw public API.
[[nodiscard]] bool is_cancelled(const ExecutionContext& context) noexcept;

/// True when the context carries a deadline that has already passed at the
/// time of the call.
[[nodiscard]] bool deadline_reached(const ExecutionContext& context) noexcept;

}  // namespace mirador

#endif  // MIRADOR_EXECUTION_CONTEXT_HPP
