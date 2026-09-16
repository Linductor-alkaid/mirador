# Benchmarks produce wall-clock evidence on the machine that runs them; they
# are executables, not ctest entries, so CI only verifies that they build.

add_executable(mirador_bench_change_detection change_detection_bench.cpp)
target_link_libraries(mirador_bench_change_detection PRIVATE mirador::image)
mirador_apply_warnings(mirador_bench_change_detection)

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
