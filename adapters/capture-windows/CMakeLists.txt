# M5-05 (design sections 21, 24 M5): optional Windows GDI capture adapter.
# Independent adaptation layer only: Mirador core never references it, and it
# never enters the default build (MIRADOR_BUILD_ADAPTERS_CAPTURE_WINDOWS, OFF).
if(NOT WIN32)
    message(FATAL_ERROR "MIRADOR_BUILD_ADAPTERS_CAPTURE_WINDOWS is only available on Windows hosts")
endif()

add_library(mirador_capture_windows
    gdi_capture.cpp
)
target_link_libraries(mirador_capture_windows
    PUBLIC mirador::core user32 gdi32
)
target_include_directories(mirador_capture_windows PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mirador_apply_warnings(mirador_capture_windows)
