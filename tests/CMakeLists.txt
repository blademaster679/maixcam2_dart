cmake_minimum_required(VERSION 3.13)
project(dart_green_detector_host_tests LANGUAGES CXX)

enable_testing()

set(HOST_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/host_sources")
file(MAKE_DIRECTORY "${HOST_SOURCE_DIR}")
configure_file(../main/src/config.cpp "${HOST_SOURCE_DIR}/config.cpp" COPYONLY)
configure_file(../main/src/green_detector.cpp "${HOST_SOURCE_DIR}/green_detector.cpp" COPYONLY)
configure_file(../main/src/green_detector_core.cpp "${HOST_SOURCE_DIR}/green_detector_core.cpp" COPYONLY)
configure_file(../main/src/target_fusion.cpp "${HOST_SOURCE_DIR}/target_fusion.cpp" COPYONLY)
configure_file(../main/src/target_json.cpp "${HOST_SOURCE_DIR}/target_json.cpp" COPYONLY)
configure_file(../main/src/visual_motion.cpp "${HOST_SOURCE_DIR}/visual_motion.cpp" COPYONLY)

add_executable(green_detector_tests
    test_green_detector.cpp
    ../main/src/nv21_pipeline.cpp
    "${HOST_SOURCE_DIR}/config.cpp"
    "${HOST_SOURCE_DIR}/green_detector.cpp"
    "${HOST_SOURCE_DIR}/green_detector_core.cpp"
    "${HOST_SOURCE_DIR}/target_fusion.cpp"
    "${HOST_SOURCE_DIR}/target_json.cpp"
    "${HOST_SOURCE_DIR}/visual_motion.cpp"
)
target_compile_features(green_detector_tests PRIVATE cxx_std_17)
target_include_directories(green_detector_tests PRIVATE
    stubs
    ../main/include
)
target_compile_definitions(green_detector_tests PRIVATE
    TEST_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

if(MSVC)
    target_compile_options(green_detector_tests PRIVATE /W4)
else()
    target_compile_options(green_detector_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME green_detector_tests COMMAND green_detector_tests)

find_package(Threads REQUIRED)
add_executable(async_frame_tests test_async_frame.cpp)
target_compile_features(async_frame_tests PRIVATE cxx_std_17)
target_include_directories(async_frame_tests PRIVATE ../main/include)
target_link_libraries(async_frame_tests PRIVATE Threads::Threads)
add_test(NAME async_frame_tests COMMAND async_frame_tests)

target_link_libraries(green_detector_tests PRIVATE Threads::Threads)

# Optional vendor ABI mocks; no board libraries are linked or executed.
set(AX_SDK_INCLUDE "" CACHE PATH "Device-matched MSP public include directory")
if(AX_SDK_INCLUDE)
    add_executable(vin_nv21_frame_tests test_vin_nv21_frame.cpp)
    target_compile_features(vin_nv21_frame_tests PRIVATE cxx_std_17)
    target_include_directories(vin_nv21_frame_tests PRIVATE stubs ../main/include "${AX_SDK_INCLUDE}")
    target_link_libraries(vin_nv21_frame_tests PRIVATE Threads::Threads)
    add_test(NAME vin_nv21_frame_tests COMMAND vin_nv21_frame_tests)
endif()
