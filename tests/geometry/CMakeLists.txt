# Unit tests for mirador::geometry public contracts (M3-02). Architecture
# boundary tests live in tests/architecture/; core contracts live in tests/core/.

mirador_add_test(
    NAME mirador.geometry.line_detector
    SOURCES line_detector_test.cpp
    LIBS mirador::geometry
    LABELS unit)
