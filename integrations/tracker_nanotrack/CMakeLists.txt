# M7-12 (DEC-020/DEC-015): NanoTrack ncnn reference tracking backend behind
# the mirador::TrackerBackend SPI (M7-11), reusing the M5-02 NcnnRuntime
# wrapper. Builds only under MIRADOR_BUILD_INTEGRATIONS=ON. The reviewed
# candidate port (docs/supply-chain/nanotrack.md) is a contract-alignment
# reference only: nothing is fetched from it, compiled from it or downloaded
# from it — models come from explicit caller paths at runtime, and the smoke
# layer generates synthetic models in the build/test tree (no weights ever in
# the source tree, project standards section 9.2.4).
add_library(mirador_tracker_nanotrack
    nanotrack_backend.cpp
)
target_link_libraries(mirador_tracker_nanotrack
    PUBLIC mirador::core mirador::image mirador_ncnn_runtime
)
target_include_directories(mirador_tracker_nanotrack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mirador_apply_warnings(mirador_tracker_nanotrack)

# Synthetic-model smoke test (model generated at runtime, no weights in the
# source tree), delivered by the independent verification suite and registered
# here like the detector_yolo/ocr_ppocr smoke targets: frozen decode, prior
# ownership, validation/ordering matrix, budget, failure visibility, per-frame
# determinism, coordinate/stride/rotation matrix, immutability and privacy.
if(MIRADOR_BUILD_TESTS)
    add_executable(mirador_integrations_tracker_nanotrack_smoke test/nanotrack_smoke.cpp)
    target_link_libraries(mirador_integrations_tracker_nanotrack_smoke PRIVATE mirador_tracker_nanotrack)
    mirador_apply_warnings(mirador_integrations_tracker_nanotrack_smoke)
    add_test(NAME mirador.integrations.tracker_nanotrack_smoke COMMAND mirador_integrations_tracker_nanotrack_smoke)
endif()
