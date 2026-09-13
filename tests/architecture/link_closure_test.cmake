# M0-05 link-closure check: the probe executable linking mirador::core may depend,
# at load time, only on the C++ standard library and toolchain runtime (RULE-01,
# DEC-002). Any NEEDED entry outside the allowlist means a third-party dependency
# leaked into the core closure.
#
# Usage (Linux only):
#   cmake -DMIRADOR_PROBE=<elf> -DMIRADOR_READELF=<readelf> -P link_closure_test.cmake

if(NOT EXISTS "${MIRADOR_PROBE}")
    message(FATAL_ERROR "probe executable not found: ${MIRADOR_PROBE}")
endif()

execute_process(
    # readelf output is localized; force the C locale so the "Shared library" match
    # below is deterministic on every machine.
    COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C "${MIRADOR_READELF}" -d "${MIRADOR_PROBE}"
    OUTPUT_VARIABLE mirador_readelf_output
    RESULT_VARIABLE mirador_readelf_result
    ERROR_VARIABLE mirador_readelf_error)
if(NOT mirador_readelf_result EQUAL 0)
    message(FATAL_ERROR "readelf failed: ${mirador_readelf_error}")
endif()

# (NEEDED) [Shared library: libfoo.so.1]
string(REGEX MATCHALL "Shared library: \\[[^]]*\\]" mirador_needed_lines "${mirador_readelf_output}")
if(NOT mirador_needed_lines)
    message(FATAL_ERROR "no NEEDED entries found in ${MIRADOR_PROBE}; readelf output:\n${mirador_readelf_output}")
endif()

# The C++20 standard library and its toolchain runtime; glibc subsumes rt/pthread/dl
# since 2.34 but older toolchains still list them separately.
set(mirador_allowed "^(lib(stdc\\+\\+|c|m|gcc_s|rt|pthread|dl|atomic|uc)\\.so|ld-linux)")

set(mirador_violations "")
foreach(mirador_line IN LISTS mirador_needed_lines)
    string(REGEX REPLACE "Shared library: \\[([^]]*)\\]" "\\1" mirador_lib "${mirador_line}")
    if(NOT mirador_lib MATCHES "${mirador_allowed}")
        string(APPEND mirador_violations "  ${mirador_lib}\n")
    else()
        message(STATUS "allowed NEEDED: ${mirador_lib}")
    endif()
endforeach()

if(mirador_violations)
    message(FATAL_ERROR
        "mirador::core link closure contains non-standard-library dependencies:\n"
        "${mirador_violations}")
endif()

message(STATUS "link closure of ${MIRADOR_PROBE} contains only standard-library entries")
