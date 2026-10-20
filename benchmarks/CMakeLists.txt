# Benchmarks produce wall-clock evidence on the machine that runs them; they
# are executables, not ctest entries, so CI only verifies that they build.

if(TARGET mirador::fusion AND TARGET mirador::geometry)
    add_executable(mirador_bench_desktop_perception desktop_perception_bench.cpp)
    target_link_libraries(mirador_bench_desktop_perception PRIVATE mirador::fusion mirador::geometry)
    mirador_apply_warnings(mirador_bench_desktop_perception)
endif()

add_executable(mirador_bench_change_detection change_detection_bench.cpp)
target_link_libraries(mirador_bench_change_detection PRIVATE mirador::image)
mirador_apply_warnings(mirador_bench_change_detection)

# M7-03 (DEC-019): change-gated short-circuit overhead against the M1
# detect_change baseline. Links fusion: the gate lives on ObjectTracker and
# detect_change reaches the executable through fusion's public link interface.
add_executable(mirador_bench_change_gate change_gate_bench.cpp)
target_link_libraries(mirador_bench_change_gate PRIVATE mirador::fusion)
mirador_apply_warnings(mirador_bench_change_gate)

# M7-09 (DEC-019 section 4): synthetic verification harness and A/B/C/D method
# matrix for the cross-frame object tracker. Links fusion: the harness is the
# caller-composed frame pipeline over the frozen M7-03..08 pool primitives
# (detect_change and estimate_global_shift reach it through fusion's public
# link interface).
add_executable(mirador_bench_object_tracking object_tracking_bench.cpp)
target_link_libraries(mirador_bench_object_tracking PRIVATE mirador::fusion)
mirador_apply_warnings(mirador_bench_object_tracking)

# M5-06 (SCOPE-08): cache hit path and backend outer cost. Links the fusion
# module because PerceptionSession composes image and cache (DEC-013).
add_executable(mirador_bench_cache_backend cache_backend_bench.cpp)
target_link_libraries(mirador_bench_cache_backend PRIVATE mirador::fusion)
mirador_apply_warnings(mirador_bench_cache_backend)

# M6-04 (DEC-017): synthetic verification harness for the experimental
# geometric region proposals. Links geometry only: the harness rasterizes with
# its own deterministic code, so no image-module pipeline is involved.
if(TARGET mirador::geometry)
    add_executable(mirador_bench_geometric_proposal geometric_proposal_bench.cpp)
    target_link_libraries(mirador_bench_geometric_proposal PRIVATE mirador::geometry)
    mirador_apply_warnings(mirador_bench_geometric_proposal)
endif()

# M6-05 (DEC-017): cross-frame association stability probe and the
# geometry-gated VisualIndex hit comparison. Adds the image module (patch
# fingerprints) and the cache module (VisualIndex) on top of geometry.
if(TARGET mirador::geometry AND TARGET mirador::image AND TARGET mirador::cache)
    add_executable(mirador_bench_proposal_reuse geometric_proposal_reuse_bench.cpp)
    target_link_libraries(mirador_bench_proposal_reuse PRIVATE mirador::geometry mirador::image
                                                               mirador::cache)
    mirador_apply_warnings(mirador_bench_proposal_reuse)
endif()

# M8-01 (DEC-025): interactive OCR budget harness — deterministic synthetic
# scenes plus the baseline/budgeted measurement paths. Needs the PP-OCR
# integration target and the fusion session, so it only appears when
# MIRADOR_BUILD_INTEGRATIONS is on; weights stay caller-provided paths.
if(TARGET mirador_ocr_ppocr)
    add_executable(mirador_bench_ocr_budget ocr_budget_bench.cpp)
    target_link_libraries(mirador_bench_ocr_budget PRIVATE mirador_ocr_ppocr mirador::fusion)
    mirador_apply_warnings(mirador_bench_ocr_budget)

    # M8-05 (DEC-025 AC8-09): rec optimization experiment on real caller-provided
    # weights — single-line cost decomposition, width policy, packing and thread
    # scaling arms plus the optional dense end-to-end sanity arm. Measurement
    # evidence only; it changes no library code and freezes no default.
    add_executable(mirador_bench_ocr_rec_experiment ocr_rec_experiment.cpp)
    target_link_libraries(mirador_bench_ocr_rec_experiment PRIVATE mirador_ocr_ppocr)
    mirador_apply_warnings(mirador_bench_ocr_rec_experiment)
endif()
