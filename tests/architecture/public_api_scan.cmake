# M0-05 architecture scan: public headers and core sources must stay free of
# thread/scheduling facilities and third-party vision/runtime dependencies
# (AGENTS.md concurrency and runtime boundaries, RULE-01..RULE-03).
#
# Usage: cmake -DMIRADOR_SOURCE_DIR=<repo root> -P public_api_scan.cmake
# Portable (CMake script mode); runs on every platform in CI.

set(MIRADOR_BANNED_TOKENS
    # Thread / scheduling facilities (core never creates threads or timers).
    "#include <thread>"
    "#include <mutex>"
    "#include <future>"
    "#include <condition_variable>"
    "#include <semaphore>"
    "#include <latch>"
    "#include <barrier>"
    "#include <stop_token>"
    "std::thread"
    "std::jthread"
    "std::async"
    "std::future"
    "std::promise"
    "std::mutex"
    "std::condition_variable"
    "std::counting_semaphore"
    "std::latch"
    "std::barrier"
    "std::stop_token"
    "pthread_create"
    "CreateThread"
    "_beginthread"
    # Third-party vision / runtime types and includes (design sections 3, 21).
    "opencv2"
    "cv::"
    "ncnn"
    "onnxruntime"
    "Ort::"
    "tensorrt"
    "NvInfer"
    "MNN"
)

file(GLOB_RECURSE mirador_scan_files
    RELATIVE "${MIRADOR_SOURCE_DIR}"
    "${MIRADOR_SOURCE_DIR}/include/mirador/*.hpp"
    "${MIRADOR_SOURCE_DIR}/include/mirador/*.h"
    "${MIRADOR_SOURCE_DIR}/src/*.cpp"
    "${MIRADOR_SOURCE_DIR}/src/*.hpp"
)
if(NOT mirador_scan_files)
    message(FATAL_ERROR "architecture scan found no sources under include/ or src/")
endif()

set(mirador_violations "")
foreach(mirador_file IN LISTS mirador_scan_files)
    file(READ "${MIRADOR_SOURCE_DIR}/${mirador_file}" mirador_contents)
    foreach(mirador_token IN LISTS MIRADOR_BANNED_TOKENS)
        string(FIND "${mirador_contents}" "${mirador_token}" mirador_hit)
        if(NOT mirador_hit EQUAL -1)
            string(APPEND mirador_violations
                "  ${mirador_file}: banned token '${mirador_token}'\n")
        endif()
    endforeach()
endforeach()

if(mirador_violations)
    message(FATAL_ERROR
        "Architecture boundary violations (thread/scheduling facilities or third-party "
        "dependencies are forbidden in include/ and src/):\n${mirador_violations}")
endif()

# M1-09: OpenCV tokens may appear only inside adapters/ (and their own tests);
# examples, benchmarks and the rest of the test suite stay adapter-free so the
# optional dependency cannot leak past the adapter boundary.
set(mirador_opencv_tokens "opencv2" "cv::")
set(mirador_adapter_files "")
file(GLOB_RECURSE mirador_adapter_files
    RELATIVE "${MIRADOR_SOURCE_DIR}"
    "${MIRADOR_SOURCE_DIR}/examples/*.cpp"
    "${MIRADOR_SOURCE_DIR}/examples/*.hpp"
    "${MIRADOR_SOURCE_DIR}/benchmarks/*.cpp"
    "${MIRADOR_SOURCE_DIR}/benchmarks/*.hpp"
    "${MIRADOR_SOURCE_DIR}/tests/*.cpp"
    "${MIRADOR_SOURCE_DIR}/tests/*.hpp"
)
list(FILTER mirador_adapter_files EXCLUDE REGEX "^tests/adapters/")
set(mirador_adapter_violations "")
foreach(mirador_file IN LISTS mirador_adapter_files)
    file(READ "${MIRADOR_SOURCE_DIR}/${mirador_file}" mirador_contents)
    foreach(mirador_token IN LISTS mirador_opencv_tokens)
        string(FIND "${mirador_contents}" "${mirador_token}" mirador_hit)
        if(NOT mirador_hit EQUAL -1)
            string(APPEND mirador_adapter_violations
                "  ${mirador_file}: adapter token '${mirador_token}' outside adapters/\n")
        endif()
    endforeach()
endforeach()

if(mirador_adapter_violations)
    message(FATAL_ERROR
        "Architecture boundary violations (OpenCV tokens are allowed only under "
        "adapters/):\n${mirador_adapter_violations}")
endif()

# M5-02 (DEC-015): model-runtime tokens are allowed only under integrations/
# (the default-build opt-in reference backends). Examples, benchmarks and the
# whole core test suite stay runtime-free; include/ and src/ are already
# covered by the banned-token scan above.
set(mirador_runtime_tokens "ncnn" "onnxruntime" "Ort::" "tensorrt" "NvInfer" "MNN")
set(mirador_runtime_violations "")
foreach(mirador_file IN LISTS mirador_adapter_files)
    file(READ "${MIRADOR_SOURCE_DIR}/${mirador_file}" mirador_contents)
    foreach(mirador_token IN LISTS mirador_runtime_tokens)
        string(FIND "${mirador_contents}" "${mirador_token}" mirador_hit)
        if(NOT mirador_hit EQUAL -1)
            string(APPEND mirador_runtime_violations
                "  ${mirador_file}: runtime token '${mirador_token}' outside integrations/\n")
        endif()
    endforeach()
endforeach()

if(mirador_runtime_violations)
    message(FATAL_ERROR
        "Architecture boundary violations (model runtime tokens are allowed only "
        "under integrations/):\n${mirador_runtime_violations}")
endif()

message(STATUS "architecture scan: ${mirador_scan_files} checked, no banned tokens")
