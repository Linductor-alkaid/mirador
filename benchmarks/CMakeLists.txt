# Benchmarks produce wall-clock evidence on the machine that runs them; they
# are executables, not ctest entries, so CI only verifies that they build.

add_executable(mirador_bench_change_detection change_detection_bench.cpp)
target_link_libraries(mirador_bench_change_detection PRIVATE mirador::image)
mirador_apply_warnings(mirador_bench_change_detection)
