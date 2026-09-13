# M0-05: architecture boundary tests. These run in every CI job (token scan is
# portable; the NEEDED check is Linux-only because readelf is a binutils tool).

# The core target must not link anything beyond its own sources (RULE-01): its
# INTERFACE link interface has to stay empty.
get_target_property(mirador_core_link_interface mirador::core INTERFACE_LINK_LIBRARIES)
if(mirador_core_link_interface)
    message(FATAL_ERROR
        "mirador::core must not link any dependency, found: ${mirador_core_link_interface}")
endif()

# Every first-party module may depend on exactly mirador::core (RULE-01, M1-01):
# no sibling module dependencies and no third-party libraries in its interface.
foreach(mirador_module IN ITEMS image cache geometry fusion render)
    if(TARGET mirador::${mirador_module})
        get_target_property(mirador_module_links mirador::${mirador_module} INTERFACE_LINK_LIBRARIES)
        if(NOT mirador_module_links STREQUAL "mirador::core")
            message(FATAL_ERROR
                "mirador::${mirador_module} must link exactly mirador::core, found: "
                "${mirador_module_links}")
        endif()
    endif()
endforeach()

add_executable(mirador_architecture_link_probe link_closure_probe.cpp)
target_link_libraries(mirador_architecture_link_probe PRIVATE mirador::core)

if(TARGET mirador::image)
    add_executable(mirador_architecture_image_link_probe image_link_closure_probe.cpp)
    target_link_libraries(mirador_architecture_image_link_probe PRIVATE mirador::image)
endif()

add_test(
    NAME mirador.architecture.source_scan
    COMMAND ${CMAKE_COMMAND} -DMIRADOR_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../..
            -P ${CMAKE_CURRENT_SOURCE_DIR}/public_api_scan.cmake)
set_tests_properties(mirador.architecture.source_scan PROPERTIES LABELS architecture)

find_program(MIRADOR_READELF_EXECUTABLE readelf)
if(MIRADOR_READELF_EXECUTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(
        NAME mirador.architecture.link_closure
        COMMAND ${CMAKE_COMMAND}
                -DMIRADOR_PROBE=$<TARGET_FILE:mirador_architecture_link_probe>
                -DMIRADOR_READELF=${MIRADOR_READELF_EXECUTABLE}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/link_closure_test.cmake)
    set_tests_properties(mirador.architecture.link_closure PROPERTIES LABELS architecture)

    if(TARGET mirador_architecture_image_link_probe)
        add_test(
            NAME mirador.architecture.link_closure_image
            COMMAND ${CMAKE_COMMAND}
                    -DMIRADOR_PROBE=$<TARGET_FILE:mirador_architecture_image_link_probe>
                    -DMIRADOR_READELF=${MIRADOR_READELF_EXECUTABLE}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/link_closure_test.cmake)
        set_tests_properties(mirador.architecture.link_closure_image PROPERTIES LABELS architecture)
    endif()
else()
    message(STATUS "readelf unavailable or not Linux: NEEDED closure check skipped here")
endif()
