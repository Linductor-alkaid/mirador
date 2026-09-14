# Adapter tests run only when the optional OpenCV adapter is enabled; they use
# the shared GoogleTest plumbing from tests/CMakeLists.txt.

if(TARGET mirador::adapters::opencv)
    mirador_add_test(
        NAME mirador.adapters.opencv
        SOURCES opencv_adapter_test.cpp
        LIBS mirador::adapters::opencv
        LABELS unit)
endif()
