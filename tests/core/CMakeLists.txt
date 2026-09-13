# Unit tests for mirador-core public contracts. Architecture boundary tests live in
# tests/architecture/; additional contract suites are appended by M0-02..M0-04.

mirador_add_test(
    NAME mirador.core.version
    SOURCES version_test.cpp
    LABELS unit)
