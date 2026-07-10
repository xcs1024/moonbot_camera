cmake_minimum_required(VERSION 3.20)
project(insta360_camera_ai_stitch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(LOCAL_OPENCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/opencv/build" ABSOLUTE)
if(EXISTS "${LOCAL_OPENCV_DIR}/OpenCVConfig.cmake")
    if(NOT OpenCV_DIR OR NOT EXISTS "${OpenCV_DIR}/OpenCVConfig.cmake")
        set(OpenCV_DIR "${LOCAL_OPENCV_DIR}" CACHE PATH "Local OpenCV package path" FORCE)
    endif()
endif()

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

if(MSVC)
    target_compile_options(insta360_ai_stitch_preview PRIVATE /utf-8 /EHsc /wd4819 /wd4828)
endif()

set(OPENCV_RUNTIME_DIR "")
if(EXISTS "${OpenCV_DIR}/x64/vc16/bin")
    set(OPENCV_RUNTIME_DIR "${OpenCV_DIR}/x64/vc16/bin")
elseif(EXISTS "${OpenCV_DIR}/../bin")
    get_filename_component(OPENCV_RUNTIME_DIR "${OpenCV_DIR}/../bin" ABSOLUTE)
endif()

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

if(OPENCV_RUNTIME_DIR)
    file(GLOB OPENCV_RUNTIME_DLLS "${OPENCV_RUNTIME_DIR}/opencv_world*.dll")
    add_custom_command(TARGET insta360_ai_stitch_preview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OPENCV_RUNTIME_DLLS}
            $<TARGET_FILE_DIR:insta360_ai_stitch_preview>
    )
endif()
