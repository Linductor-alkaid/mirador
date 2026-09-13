#include <mirador/status.hpp>

namespace mirador {

const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return "Ok";
        case ErrorCode::kInvalidArgument:
            return "InvalidArgument";
        case ErrorCode::kUnsupportedFormat:
            return "UnsupportedFormat";
        case ErrorCode::kCoordinateTransform:
            return "CoordinateTransform";
        case ErrorCode::kBackendUnavailable:
            return "BackendUnavailable";
        case ErrorCode::kBackendFailure:
            return "BackendFailure";
        case ErrorCode::kTimeout:
            return "Timeout";
        case ErrorCode::kCancelled:
            return "Cancelled";
        case ErrorCode::kCacheCorrupt:
            return "CacheCorrupt";
        case ErrorCode::kBudgetExceeded:
            return "BudgetExceeded";
    }
    return "Unknown";
}

}  // namespace mirador
