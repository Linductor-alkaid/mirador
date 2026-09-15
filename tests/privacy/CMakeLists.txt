# M5-07 privacy negative tests (DOD-06, RULE-10): no file writes from the
# pipeline, no evidence text on Status/trace side channels.

mirador_add_test(
    NAME mirador.privacy.privacy
    SOURCES privacy_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# The Backend test doubles live in tests/fusion; share them by include path
# instead of duplicating the header.
target_include_directories(mirador.privacy.privacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fusion)
