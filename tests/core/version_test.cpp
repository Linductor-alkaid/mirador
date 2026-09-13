#include <mirador/version.hpp>

#include <gtest/gtest.h>
#include <string>

namespace {

TEST(Version, ReportsConfiguredSemver) {
    const std::string expected = std::to_string(mirador::kVersionMajor) + "." +
                                 std::to_string(mirador::kVersionMinor) + "." +
                                 std::to_string(mirador::kVersionPatch);
    EXPECT_EQ(mirador::version(), expected);
}

}  // namespace
