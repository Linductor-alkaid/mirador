# Test suite. GoogleTest (DEC-006) is a submodule pinned via dependencies.lock.json;
# it is only linked into test targets and never enters the mirador-core link closure.

set(MIRADOR_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(MIRADOR_GTEST_DIR "${MIRADOR_THIRD_PARTY_DIR}/googletest")

if(NOT EXISTS "${MIRADOR_GTEST_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "GoogleTest submodule is missing (required by MIRADOR_BUILD_TESTS=ON).\n"
        "Run: git submodule update --init third_party/googletest\n"
        "Or configure with -DMIRADOR_BUILD_TESTS=OFF for a minimal core build.")
endif()

# Configure-time lock check (project standards section 9): the submodule HEAD must
# match dependencies.lock.json. Plain regex parsing keeps the 3.16 minimum viable.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../dependencies.lock.json" mirador_lock_contents)
string(REGEX MATCH
    "\"name\"[\t ]*:[\t ]*\"googletest\"[^]]*\"pinned_commit\"[\t ]*:[\t ]*\"([0-9a-f][0-9a-f]*)\""
    mirador_gtest_lock_match "${mirador_lock_contents}")
if(NOT mirador_gtest_lock_match)
    message(FATAL_ERROR "dependencies.lock.json has no pinned_commit entry for googletest.")
endif()
set(mirador_gtest_pinned "${CMAKE_MATCH_1}")
execute_process(
    COMMAND git -C "${MIRADOR_GTEST_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE mirador_gtest_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE mirador_gtest_git_result
    ERROR_QUIET)
if(NOT mirador_gtest_git_result EQUAL 0)
    message(FATAL_ERROR "Cannot determine googletest submodule HEAD (is it a git checkout?).")
endif()
if(NOT mirador_gtest_head STREQUAL mirador_gtest_pinned)
    message(FATAL_ERROR
        "googletest submodule HEAD ${mirador_gtest_head} does not match pinned_commit "
        "${mirador_gtest_pinned} in dependencies.lock.json.\n"
        "Run: git submodule update --init --recursive third_party/googletest")
endif()

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${MIRADOR_GTEST_DIR}" "${CMAKE_BINARY_DIR}/third_party/googletest" EXCLUDE_FROM_ALL)

# Adds a labeled GoogleTest executable and registers it with CTest. LIBS carries
# extra first-party module targets (e.g. mirador::image) beyond mirador::core.
function(mirador_add_test)
    cmake_parse_arguments(ARG "" "NAME;LABELS" "SOURCES;LIBS" ${ARGN})
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE mirador::core ${ARG_LIBS} gtest gtest_main)
    # TSAN on high-entropy-ASLR kernels (vm.mmap_rnd_bits = 32, Ubuntu 24.04
    # defaults) aborts probabilistically at startup with "unexpected memory
    # mapping" when randomized library mappings land in the shadow gap. The
    # mitigation is a no-ASLR personality (setarch -R, util-linux): CI wraps
    # the whole ctest invocation with it (ci.yml), and it is applied per test
    # here as well so a plain `ctest --preset tsan` is safe on any Linux box.
    # Nested setarch is harmless (the personality is per process).
    set(mirador_test_command $<TARGET_FILE:${ARG_NAME}>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_FLAGS MATCHES "fsanitize=thread")
        find_program(MIRADOR_SETARCH_EXECUTABLE setarch)
        if(MIRADOR_SETARCH_EXECUTABLE)
            set(mirador_test_command ${MIRADOR_SETARCH_EXECUTABLE} "${CMAKE_SYSTEM_PROCESSOR}" -R
                                     $<TARGET_FILE:${ARG_NAME}>)
        else()
            message(WARNING
                "TSAN tests without setarch: on high-entropy-ASLR kernels tests abort with "
                "'unexpected memory mapping' (see docs/compatibility/compatibility.md).")
        endif()
    endif()
    add_test(NAME ${ARG_NAME} COMMAND ${mirador_test_command})
    set_tests_properties(${ARG_NAME} PROPERTIES LABELS "${ARG_LABELS}")
endfunction()

add_subdirectory(core)
add_subdirectory(architecture)
if(TARGET mirador::adapters::opencv)
    add_subdirectory(adapters)
endif()
if(TARGET mirador::image)
    add_subdirectory(image)
endif()
if(TARGET mirador::cache)
    add_subdirectory(cache)
endif()
if(TARGET mirador::fusion)
    add_subdirectory(fusion)
endif()
# M5-07 privacy negative tests (DOD-06): exercise the full fusion pipeline.
if(TARGET mirador::fusion)
    add_subdirectory(privacy)
endif()
if(TARGET mirador::geometry)
    add_subdirectory(geometry)
endif()
if(TARGET mirador::render)
    add_subdirectory(render)
endif()

# M5-07: libFuzzer entry points (optional, clang-only; AGENTS.md discipline:
# the fuzzer runtime never enters mirador-core or any shipped target). The
# harnesses are executables, not ctest entries — the CI fuzz job runs them
# for bounded time; everything they link is first-party code plus gtest-free.
option(MIRADOR_BUILD_FUZZ "Build libFuzzer harnesses (requires clang with -fsanitize=fuzzer)" OFF)
if(MIRADOR_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MIRADOR_BUILD_FUZZ requires clang (libFuzzer runtime)")
    endif()
    add_subdirectory(fuzz)
endif()
