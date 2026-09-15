# M5-02 (DEC-015): reference runtime integrations. Strictly opt-in: the root
# option MIRADOR_BUILD_INTEGRATIONS (default OFF) is the only entry point, so
# the default build never fetches, compiles or links a model runtime
# (AGENTS.md runtime boundary, RULE-01, project standards section 9.2.6).
#
# Enabling this option requires network access at configure time: the pinned
# runtime is pulled via FetchContent into the build tree and is never
# vendored into the source tree, never installed with the core and never
# released under a Mirador version (implementation plan POST-05).

# Single source of truth for the pin is deps.lock.json; the CMake-side value
# below must match it or configure fails (project standards section 9.1).
set(MIRADOR_NCNN_PINNED_COMMIT "e54f7b1f88434e1d844ea0551b880a1cfb079ce1") # 20260526

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/deps.lock.json" mirador_integrations_lock)
string(REGEX MATCH "\"pinned_commit\"[ \\t\\r\\n]*:[ \\t\\r\\n]*\"([0-9a-f]+)\""
    mirador_integrations_lock_pin "${mirador_integrations_lock}")
if(NOT mirador_integrations_lock_pin)
    message(FATAL_ERROR "integrations/deps.lock.json has no pinned_commit entry")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL MIRADOR_NCNN_PINNED_COMMIT)
    message(FATAL_ERROR
        "integrations/deps.lock.json pins ${CMAKE_MATCH_1} but the integrations "
        "build pins ${MIRADOR_NCNN_PINNED_COMMIT}; keep the two in sync (upgrade "
        "goes through its own MR and a supply-chain audit record update)")
endif()

include(FetchContent)

# Trim ncnn's own build to the runtime surface the reference backends need;
# tools, examples, benchmarks, tests and the Vulkan GPU path stay out.
set(NCNN_BUILD_TOOLS OFF CACHE INTERNAL "" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE INTERNAL "" FORCE)
set(NCNN_BUILD_BENCHMARK OFF CACHE INTERNAL "" FORCE)
set(NCNN_BUILD_TESTS OFF CACHE INTERNAL "" FORCE)
set(NCNN_VULKAN OFF CACHE INTERNAL "" FORCE)
set(NCNN_PYTHON OFF CACHE INTERNAL "" FORCE)
set(NCNN_PLATFORM_ANDROID OFF CACHE INTERNAL "" FORCE)

set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(mirador_ncnn
    GIT_REPOSITORY "https://github.com/Tencent/ncnn.git"
    GIT_TAG "${MIRADOR_NCNN_PINNED_COMMIT}"
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(mirador_ncnn)

add_subdirectory(common)
add_subdirectory(ocr_ppocr)
