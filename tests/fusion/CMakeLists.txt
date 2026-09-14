# Unit/integration tests for mirador::fusion (M2-04/M2-05). Fake backends keep
# the whole suite runtime-free (design section 23).

mirador_add_test(
    NAME mirador.fusion.perception_session
    SOURCES perception_session_test.cpp
    LIBS mirador::fusion
    LABELS unit)
