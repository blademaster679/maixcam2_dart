cmake_minimum_required(VERSION 3.13)
project(dart_green_video_replay LANGUAGES CXX)

find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc videoio)

add_executable(dart_video_replay
    video_replay.cpp
    maix_image_opencv.cpp
    ../../main/src/config.cpp
    ../../main/src/green_detector.cpp
    ../../main/src/green_detector_core.cpp
)

target_compile_features(dart_video_replay PRIVATE cxx_std_17)
target_include_directories(dart_video_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../main/include
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(dart_video_replay PRIVATE ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(dart_video_replay PRIVATE /W4)
else()
    target_compile_options(dart_video_replay PRIVATE -Wall -Wextra -Wpedantic)
endif()
