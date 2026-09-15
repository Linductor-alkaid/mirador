# M5-05 (design sections 21, 24 M5): optional Linux X11 capture adapter.
# Independent adaptation layer only: Mirador core never references it, and it
# never enters the default build (MIRADOR_BUILD_ADAPTERS_CAPTURE_LINUX, OFF).
find_package(X11 REQUIRED)

add_library(mirador_capture_linux
    x11_capture.cpp
)
target_link_libraries(mirador_capture_linux
    PUBLIC mirador::core ${X11_LIBRARIES}
)
target_include_directories(mirador_capture_linux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mirador_apply_warnings(mirador_capture_linux)

# Smoke test against a live (or Xvfb) display: creates its own X window with
# a deterministic pattern, captures it through the adapter and checks the
# recovered RGB pixels. Skips nothing: a missing X server fails loudly (CI
# runs it under xvfb-run).
if(MIRADOR_BUILD_TESTS)
    add_executable(mirador_adapters_capture_linux_smoke test/x11_capture_smoke.cpp)
    target_link_libraries(mirador_adapters_capture_linux_smoke PRIVATE mirador_capture_linux X11)
    mirador_apply_warnings(mirador_adapters_capture_linux_smoke)
    add_test(NAME mirador.adapters.capture_linux_smoke COMMAND mirador_adapters_capture_linux_smoke)
endif()
