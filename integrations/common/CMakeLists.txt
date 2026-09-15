# M5-02 (DEC-015): shared ncnn runtime wrapper for the reference backends.
# Lives entirely under integrations/; no core module ever links it. The ncnn
# target comes from FetchContent in the parent integrations/CMakeLists.txt.
add_library(mirador_ncnn_runtime
    ncnn_runtime.cpp
)
target_link_libraries(mirador_ncnn_runtime PUBLIC mirador::core ncnn)
target_include_directories(mirador_ncnn_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mirador_apply_warnings(mirador_ncnn_runtime)

# Synthetic-model smoke test: generates a tiny deterministic param/bin pair at
# runtime (no weights in the source tree, project standards section 9.2.4),
# loads it through the wrapper and checks the forward result numerically.
if(MIRADOR_BUILD_TESTS)
    add_executable(mirador_integrations_ncnn_smoke test/ncnn_smoke.cpp)
    target_link_libraries(mirador_integrations_ncnn_smoke PRIVATE mirador_ncnn_runtime)
    mirador_apply_warnings(mirador_integrations_ncnn_smoke)
    add_test(NAME mirador.integrations.ncnn_smoke COMMAND mirador_integrations_ncnn_smoke)
endif()
