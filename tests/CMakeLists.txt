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
