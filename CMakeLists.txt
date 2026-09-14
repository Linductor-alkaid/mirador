cmake_minimum_required(VERSION 3.16)

project(mirador
    VERSION 0.1.0
    DESCRIPTION "Lightweight terminal vision infrastructure library"
    LANGUAGES CXX)

option(MIRADOR_BUILD_TESTS "Build mirador tests" ON)
option(MIRADOR_BUILD_BENCHMARKS "Build mirador benchmarks" ON)
option(MIRADOR_BUILD_EXAMPLES "Build mirador examples" ON)
# Optional adapter for integrator-provided OpenCV (design section 24 M1, M1-09):
# never vendored and never part of the default build; enabling it requires an
# OpenCV development installation found through find_package.
option(MIRADOR_BUILD_ADAPTERS_OPENCV "Build the optional OpenCV adapter (requires OpenCV core)" OFF)
option(MIRADOR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mirador_core
    src/core/version.cpp
    src/core/status.cpp
    src/core/pixel_format.cpp
    src/core/image_view.cpp
    src/core/geometry.cpp
    src/core/transform.cpp
    src/core/execution_context.cpp
    src/core/backend_info.cpp
)
add_library(mirador::core ALIAS mirador_core)

target_include_directories(mirador_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(mirador_core PUBLIC cxx_std_20)

# Warning policy shared by every compiled first-party target.
function(mirador_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
    if(MIRADOR_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

mirador_apply_warnings(mirador_core)

# First-party module targets (design section 21, M0-01; M1-01 promotes
# mirador::image to a compiled library). Modules without compiled content stay
# INTERFACE until they gain sources (M2+); the dependency direction module -> core
# is expressed here and locked by the architecture tests (M0-05). Optional
# third-party dependencies stay off by default (DEC-002) and are only ever pulled
# in by the module that implements them.
set(MIRADOR_MODULES image cache geometry fusion render)
foreach(mirador_module IN LISTS MIRADOR_MODULES)
    string(TOUPPER "${mirador_module}" mirador_module_upper)
    option(MIRADOR_BUILD_${mirador_module_upper}
        "Build mirador::${mirador_module} module" ON)
    if(MIRADOR_BUILD_${mirador_module_upper})
        if(mirador_module STREQUAL "image")
            add_library(mirador_${mirador_module}
                src/image/image_buffer.cpp
                src/image/color_convert.cpp
                src/image/crop.cpp
                src/image/resize.cpp
                src/image/fingerprint.cpp
                src/image/change_detection.cpp
            )
            mirador_apply_warnings(mirador_${mirador_module})
            target_link_libraries(mirador_${mirador_module} PUBLIC mirador::core)
        elseif(mirador_module STREQUAL "cache")
            add_library(mirador_${mirador_module}
                src/cache/frame_cache.cpp
                src/cache/capability_cache.cpp
            )
            mirador_apply_warnings(mirador_${mirador_module})
            target_link_libraries(mirador_${mirador_module} PUBLIC mirador::core)
        elseif(mirador_module STREQUAL "fusion")
            # The perception session composes image pipelines with the caches
            # (design section 5 DAG: image -> fusion <- cache, DEC-013); the
            # dependency direction towards core abstractions is unchanged.
            if(NOT MIRADOR_BUILD_IMAGE OR NOT MIRADOR_BUILD_CACHE)
                message(FATAL_ERROR "MIRADOR_BUILD_FUSION=ON requires MIRADOR_BUILD_IMAGE and MIRADOR_BUILD_CACHE")
            endif()
            add_library(mirador_${mirador_module} src/fusion/perception_session.cpp)
            mirador_apply_warnings(mirador_${mirador_module})
            target_link_libraries(mirador_${mirador_module} PUBLIC mirador::core mirador::image mirador::cache)
        else()
            add_library(mirador_${mirador_module} INTERFACE)
            target_link_libraries(mirador_${mirador_module} INTERFACE mirador::core)
        endif()
        add_library(mirador::${mirador_module} ALIAS mirador_${mirador_module})
    endif()
endforeach()

if(MIRADOR_BUILD_BENCHMARKS AND TARGET mirador::image)
    add_subdirectory(benchmarks)
endif()

if(MIRADOR_BUILD_EXAMPLES AND TARGET mirador::image AND TARGET mirador::cache)
    add_subdirectory(examples)
endif()

if(MIRADOR_BUILD_ADAPTERS_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core)
    add_subdirectory(adapters/opencv)
endif()

if(MIRADOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
