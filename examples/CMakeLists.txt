# Examples are runtime-free demos of the public API; they build by default so
# CI keeps them compiling, but they are not ctest entries. Each example is
# guarded by its module target so module-off configurations stay buildable.

add_executable(mirador_example_change_detection change_detection_tour.cpp)
target_link_libraries(mirador_example_change_detection PRIVATE mirador::image mirador::cache)
mirador_apply_warnings(mirador_example_change_detection)

if(TARGET mirador::fusion)
    add_executable(mirador_example_perception_session perception_session_tour.cpp)
    target_link_libraries(mirador_example_perception_session PRIVATE mirador::fusion)
    mirador_apply_warnings(mirador_example_perception_session)
endif()
