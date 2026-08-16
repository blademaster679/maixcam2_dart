cmake_minimum_required(VERSION 3.13)
project(dart_green_detector_host_tests LANGUAGES CXX)

enable_testing()

set(HOST_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/host_sources")
file(MAKE_DIRECTORY "${HOST_SOURCE_DIR}")
configure_file(../main/src/config.cpp "${HOST_SOURCE_DIR}/config.cpp" COPYONLY)
configure_file(../main/src/green_detector.cpp "${HOST_SOURCE_DIR}/green_detector.cpp" COPYONLY)
configure_file(../main/src/green_detector_core.cpp "${HOST_SOURCE_DIR}/green_detector_core.cpp" COPYONLY)

add_executable(green_detector_tests
    test_green_detector.cpp
    "${HOST_SOURCE_DIR}/config.cpp"
    "${HOST_SOURCE_DIR}/green_detector.cpp"
    "${HOST_SOURCE_DIR}/green_detector_core.cpp"
)
target_compile_features(green_detector_tests PRIVATE cxx_std_17)
target_include_directories(green_detector_tests PRIVATE
    stubs
    ../main/include
)
target_compile_definitions(green_detector_tests PRIVATE
    TEST_PROJECT_ROOT=".."
)

if(MSVC)
    target_compile_options(green_detector_tests PRIVATE /W4)
else()
    target_compile_options(green_detector_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME green_detector_tests COMMAND green_detector_tests)
