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
