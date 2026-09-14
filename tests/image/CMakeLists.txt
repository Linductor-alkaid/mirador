# Unit tests for mirador::image public contracts (M1+). Architecture boundary
# tests live in tests/architecture/; core contracts live in tests/core/.

mirador_add_test(
    NAME mirador.image.image_buffer
    SOURCES image_buffer_test.cpp
    LIBS mirador::image
    LABELS unit)

mirador_add_test(
    NAME mirador.image.color_convert
    SOURCES color_convert_test.cpp
    LIBS mirador::image
    LABELS unit)

mirador_add_test(
    NAME mirador.image.crop
    SOURCES crop_test.cpp
    LIBS mirador::image
    LABELS unit)

mirador_add_test(
    NAME mirador.image.resize
    SOURCES resize_test.cpp
    LIBS mirador::image
    LABELS unit)

mirador_add_test(
    NAME mirador.image.fingerprint
    SOURCES fingerprint_test.cpp
    LIBS mirador::image
    LABELS unit)

mirador_add_test(
    NAME mirador.image.change_detection
    SOURCES change_detection_test.cpp
    LIBS mirador::image
    LABELS unit)

mirador_add_test(
    NAME mirador.image.letterbox
    SOURCES letterbox_test.cpp
    LIBS mirador::image
    LABELS unit)
