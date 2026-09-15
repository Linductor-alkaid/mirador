# M5-04 (DEC-015): YOLO-family reference detection backend behind the
# mirador::DetectorBackend SPI, reusing the M3 letterbox and NMS reference
# components. Builds only under MIRADOR_BUILD_INTEGRATIONS=ON.
add_library(mirador_detector_yolo
    yolo_backend.cpp
)
target_link_libraries(mirador_detector_yolo
    PUBLIC mirador::core mirador::image mirador_ncnn_runtime
)
target_include_directories(mirador_detector_yolo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mirador_apply_warnings(mirador_detector_yolo)

# Synthetic-model smoke test (model generated at runtime, no weights in the
# source tree): YOLOv5-layout decode, unletterbox mapping, NMS duplicate
# suppression, budget and factory error paths.
if(MIRADOR_BUILD_TESTS)
    add_executable(mirador_integrations_detector_yolo_smoke test/yolo_smoke.cpp)
    target_link_libraries(mirador_integrations_detector_yolo_smoke PRIVATE mirador_detector_yolo)
    mirador_apply_warnings(mirador_integrations_detector_yolo_smoke)
    add_test(NAME mirador.integrations.detector_yolo_smoke COMMAND mirador_integrations_detector_yolo_smoke)
endif()
