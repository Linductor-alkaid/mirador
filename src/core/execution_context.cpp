#include <mirador/execution_context.hpp>

#include <chrono>

namespace mirador {

bool is_cancelled(const ExecutionContext& context) noexcept {
    if (!context.is_cancelled) {
        return false;
    }
    try {
        return context.is_cancelled();
    } catch (...) {
        // A throwing callback is indistinguishable from a hostile environment;
        // treating it as cancellation keeps the public API no-throw (DEC-004).
        return true;
    }
}

bool deadline_reached(const ExecutionContext& context) noexcept {
    return context.deadline.has_value() && std::chrono::steady_clock::now() > *context.deadline;
}

}  // namespace mirador
