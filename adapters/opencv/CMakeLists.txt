# Optional OpenCV adapter (M1-09). Configured only when
# MIRADOR_BUILD_ADAPTERS_OPENCV=ON and OpenCV is found by the root list file;
# OpenCV stays an integrator-provided dependency (see THIRD_PARTY_NOTICES).

add_library(mirador_adapters_opencv src/opencv_adapter.cpp)
add_library(mirador::adapters::opencv ALIAS mirador_adapters_opencv)

target_include_directories(mirador_adapters_opencv
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# SYSTEM keeps OpenCV's own headers out of our warning gate. Variable form is
# used because OpenCVConfig exports opencv_* targets without the OpenCV::
# namespace on some distributions (e.g. Ubuntu 24.04).
target_include_directories(mirador_adapters_opencv SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(mirador_adapters_opencv PUBLIC mirador::core mirador::image ${OpenCV_LIBS})

target_compile_features(mirador_adapters_opencv PUBLIC cxx_std_20)
mirador_apply_warnings(mirador_adapters_opencv)
