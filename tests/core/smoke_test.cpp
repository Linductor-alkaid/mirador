#include <mirador/version.hpp>

#include <cstdio>
#include <string>

namespace {

// Smoke test placeholder until the test framework decision (DEC-006) lands in M0-08.
int fail(const char* what) {
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
    return 1;
}

}  // namespace

int main() {
    const std::string expected = std::to_string(mirador::kVersionMajor) + "." + std::to_string(mirador::kVersionMinor) +
                                 "." + std::to_string(mirador::kVersionPatch);
    if (std::string(mirador::version()) != expected) {
        return fail("version() does not match kVersionMajor/Minor/Patch");
    }
    return 0;
}
