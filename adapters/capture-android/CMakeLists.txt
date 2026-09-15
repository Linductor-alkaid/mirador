# M5-05 (design sections 21, 24 M5): optional Android capture adapter example
# (MediaProjection + Accessibility). Independent adaptation layer only:
# Mirador core never references it, and it never enters the default build
# (MIRADOR_BUILD_ADAPTERS_CAPTURE_ANDROID, OFF).
#
# The pure conversion utilities (accessibility node -> ExternalRegion,
# DEC-016 projection display transform) build on any host so the host test
# suite can exercise them; the NDK sources (AImageReader projection capture,
# JNI bridge) build only under the Android toolchain.

add_library(mirador_capture_android_core
    accessibility_regions.cpp
    display_transform.cpp
)
target_link_libraries(mirador_capture_android_core
    PUBLIC mirador::core
)
target_include_directories(mirador_capture_android_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mirador_apply_warnings(mirador_capture_android_core)

if(ANDROID)
    add_library(mirador_capture_android
        projection_capture.cpp
        accessibility_jni.cpp
    )
    target_link_libraries(mirador_capture_android
        PUBLIC mirador_capture_android_core mediandk
    )
    mirador_apply_warnings(mirador_capture_android)
endif()

if(MIRADOR_BUILD_TESTS)
    add_executable(mirador_adapters_capture_android_test test/accessibility_regions_test.cpp)
    target_link_libraries(mirador_adapters_capture_android_test
        PRIVATE mirador_capture_android_core mirador::core
    )
    mirador_apply_warnings(mirador_adapters_capture_android_test)
    add_test(NAME mirador.adapters.capture_android_accessibility COMMAND mirador_adapters_capture_android_test)
endif()
