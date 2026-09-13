# Unit tests for mirador-core public contracts. Architecture boundary tests live in
# tests/architecture/; additional contract suites are appended by M0-04.

mirador_add_test(
    NAME mirador.core.version
    SOURCES version_test.cpp
    LABELS unit)

mirador_add_test(
    NAME mirador.core.status_result
    SOURCES status_result_test.cpp
    LABELS unit)

mirador_add_test(
    NAME mirador.core.image_view
    SOURCES image_view_test.cpp
    LABELS unit)

mirador_add_test(
    NAME mirador.core.transform
    SOURCES transform_test.cpp
    LABELS unit)

mirador_add_test(
    NAME mirador.core.transform_property
    SOURCES transform_property_test.cpp
    LABELS "property;unit")
