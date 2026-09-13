cmake_minimum_required(VERSION 3.16)

project(mirador
    VERSION 0.1.0
    DESCRIPTION "Lightweight terminal vision infrastructure library"
    LANGUAGES CXX)

option(MIRADOR_BUILD_TESTS "Build mirador tests" ON)
option(MIRADOR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optional module targets (mirador::image/cache/geometry/fusion/render) are added
# by M0-01 as they gain content; optional dependencies stay off by default (DEC-002).
add_library(mirador_core
    src/core/version.cpp
)
add_library(mirador::core ALIAS mirador_core)

target_include_directories(mirador_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(mirador_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(mirador_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(mirador_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(MIRADOR_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(mirador_core PRIVATE /WX)
    else()
        target_compile_options(mirador_core PRIVATE -Werror)
    endif()
endif()

if(MIRADOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
