# M5-07 fuzz entry points (design sections 19/23; SCOPE-08/M5-07): adversarial
# inputs into the three highest-risk pure surfaces — DB probability-map
# post-processing, capability cache key serialization and coordinate
# transforms. Harnesses are executables (not ctest entries); the CI fuzz job
# runs each for bounded time with address+undefined sanitizers, so every
# crash, sanitizer report or invariant break is a finding.
#
# Sanitizer flags live here (not in the CI job) so local runs get the same
# semantics: -fno-sanitize-recover makes UB abort instead of printing.

set(MIRADOR_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)

if(NOT TARGET mirador::image OR NOT TARGET mirador::cache)
    message(FATAL_ERROR "MIRADOR_BUILD_FUZZ requires mirador::image and mirador::cache (default-on modules)")
endif()

function(mirador_add_fuzz)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;LIBS" ${ARGN})
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_compile_options(${ARG_NAME} PRIVATE ${MIRADOR_FUZZ_FLAGS})
    target_link_options(${ARG_NAME} PRIVATE ${MIRADOR_FUZZ_FLAGS})
    target_link_libraries(${ARG_NAME} PRIVATE mirador::core ${ARG_LIBS})
endfunction()

mirador_add_fuzz(NAME mirador_fuzz_db_postprocess
    SOURCES db_postprocess_fuzz.cpp
    LIBS mirador::image)
mirador_add_fuzz(NAME mirador_fuzz_cache_key
    SOURCES cache_key_fuzz.cpp
    LIBS mirador::cache)
mirador_add_fuzz(NAME mirador_fuzz_transform
    SOURCES transform_fuzz.cpp)
