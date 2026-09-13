#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <gtest/gtest.h>
#include <string>
#include <utility>

namespace {

using mirador::ErrorCode;
using mirador::Result;
using mirador::Status;

TEST(Status, DefaultIsSuccess) {
    const Status status;
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(status);
    EXPECT_EQ(status.code(), ErrorCode::kOk);
    EXPECT_TRUE(status.message().empty());
}

TEST(Status, ErrorCarriesCodeAndMessage) {
    const Status status(ErrorCode::kInvalidArgument, "width must be positive");
    EXPECT_FALSE(status.ok());
    EXPECT_FALSE(status);
    EXPECT_EQ(status.code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(status.message(), "width must be positive");
}

// Design section 19 / DEC-004: every category must be representable and diagnosable.
TEST(Status, CoversAllDesignErrorCategories) {
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kOk), "Ok");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kInvalidArgument), "InvalidArgument");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kUnsupportedFormat), "UnsupportedFormat");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kCoordinateTransform), "CoordinateTransform");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kBackendUnavailable), "BackendUnavailable");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kBackendFailure), "BackendFailure");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kTimeout), "Timeout");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kCancelled), "Cancelled");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kCacheCorrupt), "CacheCorrupt");
    EXPECT_STREQ(mirador::error_code_name(ErrorCode::kBudgetExceeded), "BudgetExceeded");
}

// DEC-004: timeout and cancellation are distinct outcomes, never conflated.
TEST(Status, TimeoutAndCancellationRemainDistinct) {
    const Status timeout(ErrorCode::kTimeout, "deadline exceeded");
    const Status cancelled(ErrorCode::kCancelled, "context cancelled");
    EXPECT_NE(timeout.code(), cancelled.code());
}

TEST(Result, HoldsValueOnSuccess) {
    const Result<int> result(42);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 42);
}

TEST(Result, PropagatesErrorStatus) {
    const Result<int> result(Status(ErrorCode::kBackendFailure, "inference failed"));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kBackendFailure);
    EXPECT_EQ(result.status().message(), "inference failed");
}

TEST(Result, ValueOrFallsBackOnError) {
    const Result<int> failure(Status(ErrorCode::kBudgetExceeded, "budget"));
    const Result<int> success(7);
    EXPECT_EQ(failure.value_or(-1), -1);
    EXPECT_EQ(success.value_or(-1), 7);
}

TEST(Result, TakeValueMovesPayloadOut) {
    Result<std::string> result(std::string("payload"));
    const std::string moved = std::move(result).take_value();
    EXPECT_EQ(moved, "payload");
}

TEST(ResultVoid, DefaultsToSuccess) {
    const Result<void> result;
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.status().ok());
}

TEST(ResultVoid, CarriesErrorStatus) {
    const Result<void> result(Status(ErrorCode::kUnsupportedFormat, "nv12 not supported here"));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), ErrorCode::kUnsupportedFormat);
}

TEST(Result, PropagatesThroughErrorChaining) {
    auto parse = [](int input) -> Result<int> {
        if (input < 0) {
            return Status(ErrorCode::kInvalidArgument, "negative input");
        }
        return input * 2;
    };
    const Result<int> ok_result = parse(21);
    const Result<int> error_result = parse(-1);
    EXPECT_EQ(ok_result.value(), 42);
    EXPECT_EQ(error_result.status().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
