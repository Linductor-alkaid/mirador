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
    NAME mirador.fusion.display_space
    SOURCES display_space_test.cpp
    LIBS mirador::fusion
    LABELS unit)

mirador_add_test(
    NAME mirador.fusion.stable_id_tracker
    SOURCES stable_id_tracker_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M7-01 bounded target pool contract (DEC-019): options, adoption atomicity,
# byte accounting, eviction order, terminate lifecycle, coordinate matrix.
mirador_add_test(
    NAME mirador.fusion.object_tracker
    SOURCES object_tracker_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M7-05 neighborhood verifier: verification ROI, template NCC with the
# peak-sidelobe gate, structure-baseline bookkeeping, budget/cancel paths,
# coordinate matrix and determinism.
mirador_add_test(
    NAME mirador.fusion.object_tracker_verification
    SOURCES object_tracker_verification_test.cpp
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

# M5-07 concurrency matrix (TSAN preset): published-snapshot readers and
# parallel sessions. Threads need the system threads library on platforms
# where std::thread does not link transitively.
find_package(Threads REQUIRED)
mirador_add_test(
    NAME mirador.fusion.concurrency
    SOURCES concurrency_test.cpp
    LIBS mirador::fusion
    LABELS "unit;concurrency")
target_link_libraries(mirador.fusion.concurrency PRIVATE Threads::Threads)
