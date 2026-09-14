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

# M3-11: the two design-section-24 demos proving the library is not bound to
# one platform story: road-boundary line detection (robotics/desktop) and
# icon-state reuse through the visual index (desktop/system UI).
if(TARGET mirador::geometry)
    add_executable(mirador_example_road_segments road_segments_tour.cpp)
    target_link_libraries(mirador_example_road_segments PRIVATE mirador::geometry)
    mirador_apply_warnings(mirador_example_road_segments)
endif()

if(TARGET mirador::cache AND TARGET mirador::image)
    add_executable(mirador_example_icon_state_index icon_state_index_tour.cpp)
    target_link_libraries(mirador_example_icon_state_index PRIVATE mirador::cache mirador::image)
    mirador_apply_warnings(mirador_example_icon_state_index)
endif()
