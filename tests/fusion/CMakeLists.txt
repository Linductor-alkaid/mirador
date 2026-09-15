# Unit/integration tests for mirador::fusion (M2 perception session, M4 fusion
# engine / evidence / stable ids / session fuse). Fake backends keep the whole
# suite runtime-free (design section 23).

mirador_add_test(
    NAME mirador.fusion.perception_session
    SOURCES perception_session_test.cpp
    LIBS mirador::fusion
    LABELS unit)

mirador_add_test(
    NAME mirador.fusion.evidence_set
    SOURCES evidence_set_test.cpp
    LIBS mirador::fusion
    LABELS unit)

mirador_add_test(
    NAME mirador.fusion.fusion_engine
    SOURCES fusion_engine_test.cpp
    LIBS mirador::fusion
    LABELS unit)

mirador_add_test(
    NAME mirador.fusion.stable_id_tracker
    SOURCES stable_id_tracker_test.cpp
    LIBS mirador::fusion
    LABELS unit)

mirador_add_test(
    NAME mirador.fusion.snapshot
    SOURCES semantic_snapshot_test.cpp
    LIBS mirador::fusion
    LABELS unit)

mirador_add_test(
    NAME mirador.fusion.session_fuse
    SOURCES session_fuse_test.cpp
    LIBS mirador::fusion
    LABELS unit)
