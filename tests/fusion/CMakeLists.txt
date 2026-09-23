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

# M7-06 evidence fusion state machine: the frozen grade table (impostor/
# semantic vetoes, strong/weak appearance with the position gate,
# placeholder), the four-state transitions with the commit-counted
# uncertain_frame_limit boundary, the negative-template collection policies,
# state-slot byte accounting, atomic commits with validation-before-
# cancellation, the DEC-010 confirmed-association gate passthrough, the
# coordinate matrix and determinism.
mirador_add_test(
    NAME mirador.fusion.object_tracker_evidence_fusion
    SOURCES object_tracker_evidence_fusion_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M7-07 global motion compensation and layout-generation integration: the
# frozen kGlobal trigger, whole-pool compensation with the confidence gate and
# atomic invalid/overflow paths, the strictly-greater generation-lag sweep with
# its kUncertain-only exhaustion rule, the trigger -> kGenerationSwitch state
# chain, the estimator-in-the-loop scroll round trip and the DOD-03 rotation/
# odd-size/stride/flush matrix with bitwise determinism.
mirador_add_test(
    NAME mirador.fusion.object_tracker_motion_generation
    SOURCES object_tracker_motion_generation_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M7-08 cascade redetection primitives and identity review: the pure const
# gate with the static-frame zero-trigger anchor, the frozen doubling backoff
# in frame sequences, the explicit kTerminated budget exhaustion, both
# identity-review ID branches over the real verify_track/commit_track_evidence
# reuse, the bounded redetection log with its eviction counter, episode slot
# lifecycle/byte accounting, cancellation conversion, twin determinism and the
# DOD-03 rotation/odd-size/stride matrix over the reused review path.
mirador_add_test(
    NAME mirador.fusion.object_tracker_redetection
    SOURCES object_tracker_redetection_test.cpp
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
