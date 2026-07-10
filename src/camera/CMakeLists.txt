cmake_minimum_required(VERSION 3.20)
project(insta360_camera_ai_stitch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(insta360_ai_stitch_preview
    insta360_ai_stitch_preview.cpp
)

target_include_directories(insta360_ai_stitch_preview PRIVATE
    ../../libs/CameraSDK/include
    ../../libs/MediaSDK/include/stitcher
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(insta360_ai_stitch_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/CameraSDK/lib/CameraSDK.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/MediaSDK/lib/MediaSDK.lib
    ${OpenCV_LIBS}
)

add_custom_command(TARGET insta360_ai_stitch_preview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/CameraSDK/bin
        $<TARGET_FILE_DIR:insta360_ai_stitch_preview>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/MediaSDK/bin
        $<TARGET_FILE_DIR:insta360_ai_stitch_preview>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/MediaSDK/models
        $<TARGET_FILE_DIR:insta360_ai_stitch_preview>/models
)
