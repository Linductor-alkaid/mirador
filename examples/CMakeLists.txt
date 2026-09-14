# Examples are runtime-free demos of the public API; they build by default so
# CI keeps them compiling, but they are not ctest entries.

add_executable(mirador_example_change_detection change_detection_tour.cpp)
target_link_libraries(mirador_example_change_detection PRIVATE mirador::image mirador::cache)
mirador_apply_warnings(mirador_example_change_detection)
