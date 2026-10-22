# Unit/integration tests for mirador::fusion (M2 perception session, M4 fusion
# engine / evidence / stable ids / session fuse). Fake backends keep the whole
# suite runtime-free (design section 23).

mirador_add_test(
    NAME mirador.fusion.perception_session
    SOURCES perception_session_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M8-04 (DEC-025): run_ocr_budgeted wiring — coordinate recovery, source
# binding, cache isolation, report enforcement and the error contract.
mirador_add_test(
    NAME mirador.fusion.session_budgeted_ocr
    SOURCES perception_session_budgeted_test.cpp
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

# M7-09 calibration freeze (DEC-019 section 5): the ObjectTrackerOptions and
# ShiftEstimationParams defaults pinned to the values the M7-09 harness
# published in docs/benchmarks/linux-x64-object-tracking-2026-09.md, the
# frozen verification-ROI reach that separates the 48 px scroll step from the
# +24 px popup displacement, and the harness caller's 0.7 compensation-gate
# boundary behavior.
mirador_add_test(
    NAME mirador.fusion.object_tracker_calibration
    SOURCES object_tracker_calibration_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M7-10 go/no-go verdict pin: the compensation-confidence bands the verdict
# record ruled on — the library default 0.0 applies the measured local-change
# pseudo-shift band (the recorded exposure) while the 0.7 caller reference
# policy refuses it, the true-scroll band applies at both, and an explicit 0.0
# configuration is behaviorally identical to the library default.
mirador_add_test(
    NAME mirador.fusion.object_tracker_verdict_ruling
    SOURCES object_tracker_verdict_ruling_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M7-11 TrackerBackend SPI contract freeze (DEC-020): the fake fixed-trajectory
# tracker backend makes the frozen header contract executable and rehearses the
# fusion-side orchestration with the frozen ObjectTracker primitives only —
# initialize/update lifecycle, the validation matrix with the frozen
# validation-before-cancellation ordering, cancel/deadline conversion with
# all-or-nothing session state, kBackendFailure degradation to kUncertain
# through the existing commit_track_evidence grade and session rebuild, handle
# destruction on terminate/pool eviction/reset, the capability-result cache
# exemption negative, per-sequence determinism, the DOD-03 rotation/odd-size/
# stride/flush matrix and the RULE-10 privacy negatives.
mirador_add_test(
    NAME mirador.fusion.tracker_backend_orchestration
    SOURCES tracker_backend_orchestration_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M7-13 deep-enhanced channel conditional fusion (design section 6.2, RISK-2026-18):
# the injection-point slot primitives (attach/detach/tracker_session) with their
# byte accounting, explicit budget rejection and the terminate/eviction/reset
# destruction triangle, the RISK-2026-18 combination matrix (co-located
# corroboration, conservative conflict demotion, never-confirms-by-itself), the
# dual-channel-consistent template protection incl. the redetection-review scope
# note, the kBackendFailure disposal -> degradation -> rebuild loop, DEC-021
# adoption-point validation with the frozen validation-before-cancellation
# ordering, the RULE-12 switch zero-change contract, DOD-04 non-relaxation,
# twin determinism, the DOD-03 rotation/odd-size/stride/flush matrix through a
# pixel-echo fake backend and the RULE-10 privacy negatives.
mirador_add_test(
    NAME mirador.fusion.object_tracker_deep_channel
    SOURCES object_tracker_deep_channel_test.cpp
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

# M9-01 continuous-perception contract (DEC-026, Experimental): the frozen
# reference limits and byte-sized enums, the per-pair relation decision table
# with inclusive threshold boundaries, decision traces, symmetry and invalid-
# geometry rejection, the semantics-compatibility predicate, relation-view
# invariants and byte accounting, attention-policy validation, the field-
# sensitive policy digest and focus resolution with the frozen criterion
# priority, work-token validation and the frozen staleness check order,
# semantic-event invariants and byte accounting, and the coordinator identity
# core (create validation, source-epoch lifecycle, zero coordination bytes,
# move-only semantics, twin determinism).
mirador_add_test(
    NAME mirador.fusion.continuous_perception
    SOURCES continuous_perception_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M9-02 relation-view builder (DEC-026 section 4, Experimental): the fixed
# fusion fixtures (duplicate / part-of / plain-overlap / conflict / disjoint),
# star-shaped duplicate grouping without transitive chains, evidence and merge
# trace preservation, the whole-batch rejection matrix (illegal geometry,
# stale generation, tighten-only caps, byte budget), anchor-field determinism,
# cancellation conversion and the old-fusion-default zero-change negative.
mirador_add_test(
    NAME mirador.fusion.relation_view
    SOURCES relation_view_test.cpp
    LIBS mirador::fusion
    LABELS unit)

# M9-03 attention planning (DEC-026 section 2, Experimental): the engaged
# policy with its digest-driven monotonic revision and byte accounting, the
# tracker-backed focus resolution matrix (snapshot win, stale-generation
# short-circuit, tracking/uncertain resolve, lost -> missing-localization,
# terminated/absent -> unknown), the synchronous plan (focus/bootstrap work
# ordering, importance priority echo, per-condition resolution table) with
# the kFrame/kOriented conversion matrix over all rotations, peripheral
# reconsideration records (partial ROI intersection, global collapse),
# whole-batch validation and cancellation conversion.
mirador_add_test(
    NAME mirador.fusion.attention_plan
    SOURCES attention_plan_test.cpp
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
