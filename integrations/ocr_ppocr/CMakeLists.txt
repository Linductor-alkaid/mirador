# M5-03 (DEC-015): PP-OCR mobile reference OCR backends — detection (DBNet)
# and recognition (CRNN-style CTC) behind the mirador::OcrBackend SPI, plus
# the combined pipeline. Builds only under MIRADOR_BUILD_INTEGRATIONS=ON.
add_library(mirador_ocr_ppocr
    ctc_decode.cpp
    ppocr_backend.cpp
)
target_link_libraries(mirador_ocr_ppocr
    PUBLIC mirador::core mirador::image mirador_ncnn_runtime
)
target_include_directories(mirador_ocr_ppocr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mirador_apply_warnings(mirador_ocr_ppocr)

# Synthetic-model smoke tests (models generated at runtime, no weights in the
# source tree): det path probability-map contract and rec path CTC contract.
if(MIRADOR_BUILD_TESTS)
    add_executable(mirador_integrations_ppocr_smoke test/ppocr_smoke.cpp)
    target_link_libraries(mirador_integrations_ppocr_smoke PRIVATE mirador_ocr_ppocr)
    mirador_apply_warnings(mirador_integrations_ppocr_smoke)
    add_test(NAME mirador.integrations.ppocr_smoke COMMAND mirador_integrations_ppocr_smoke)
endif()
