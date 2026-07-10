cmake_minimum_required(VERSION 3.20)

project(insta360_stitch_preview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(
    CAMERA_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/CameraSDK"
    ABSOLUTE
)
get_filename_component(
    MEDIA_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/MediaSDK"
    ABSOLUTE
)
get_filename_component(
    LOCAL_OPENCV_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/opencv/build"
    ABSOLUTE
)

find_package(
    OpenCV REQUIRED CONFIG
    PATHS "${LOCAL_OPENCV_DIR}"
    NO_DEFAULT_PATH
)

add_executable(
    insta360_ai_stitch_preview
    insta360_ai_stitch_preview.cpp
)

target_include_directories(
    insta360_ai_stitch_preview
    PRIVATE
    "${CAMERA_SDK_ROOT}/include"
    "${MEDIA_SDK_ROOT}/include/stitcher"
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(
    insta360_ai_stitch_preview
    PRIVATE
    "${CAMERA_SDK_ROOT}/lib/CameraSDK.lib"
    "${MEDIA_SDK_ROOT}/lib/MediaSDK.lib"
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(
        insta360_ai_stitch_preview
        PRIVATE
        /utf-8
        /EHsc
        /wd4819
        /wd4828
    )
endif()

# 复制相机、拼接和 OpenCV 运行库，使程序可直接从输出目录启动。
add_custom_command(
    TARGET insta360_ai_stitch_preview
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CAMERA_SDK_ROOT}/bin"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${MEDIA_SDK_ROOT}/bin"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>"
    # MediaSDK 附带的是 VS 2015 初版 CRT，会覆盖系统新版运行库并导致访问冲突。
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/msvcp140.dll"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/msvcp140_1.dll"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/msvcp140_2.dll"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/vcruntime140.dll"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/vcruntime140_1.dll"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/vccorlib140.dll"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/ucrtbase.dll"
        "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>/ucrtbased.dll"
)

file(
    GLOB OPENCV_RUNTIME_DLLS
    "${LOCAL_OPENCV_DIR}/x64/vc16/bin/opencv_world*.dll"
)

if(OPENCV_RUNTIME_DLLS)
    add_custom_command(
        TARGET insta360_ai_stitch_preview
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OPENCV_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:insta360_ai_stitch_preview>"
    )
endif()
