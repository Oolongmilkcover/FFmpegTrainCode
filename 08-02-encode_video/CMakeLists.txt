# =============================================================================
#  08-02-encode_video —— CMake 构建脚本（Qt 6.11 + FFmpeg 8.1）
#
#  教学工程：读本地 YUV → libx264 编码成 H.264
#  学习重点在 main.cpp 的注释（按「【知识点 N】」编号）。
#
#  FFmpeg 路径与依赖组织方式沿用同一套 Qt6 播放器工程
#  （QtFFmpegPlayer/Player/QtPlayer/CMakeLists.txt）：
#    FFMPEG_PATH 缓存变量 + ffmpeg_deps INTERFACE 目标。
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# 纯 C++17，不链接 Qt 库 —— 选 Qt 6.11 的 kit 只是借用它的 MinGW 13.1 工具链
project(encode_video VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(encode_video main.cpp)

# ========================== FFmpeg 依赖 ==========================
add_library(ffmpeg_deps INTERFACE)

if(WIN32)
    set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1" CACHE PATH
        "FFmpeg 安装目录（需含 include/、lib/、bin/），可用 -DFFMPEG_PATH=... 覆盖")

    target_include_directories(ffmpeg_deps INTERFACE ${FFMPEG_PATH}/include)

    # 本工程用到：avcodec（编码）、avutil（imgutils/opt/time/error）
    # 另外几个是为了和播放器工程共用同一份 ffmpeg_deps 定义而保留
    find_library(AVCODEC_LIB    avcodec    PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVFORMAT_LIB   avformat   PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVDEVICE_LIB   avdevice   PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVUTIL_LIB     avutil     PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIB swresample PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWSCALE_LIB    swscale    PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVFILTER_LIB   avfilter   PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)

    foreach(_lib AVCODEC_LIB AVFORMAT_LIB AVDEVICE_LIB AVUTIL_LIB
                 SWRESAMPLE_LIB SWSCALE_LIB AVFILTER_LIB)
        if(NOT ${_lib})
            message(FATAL_ERROR
                "找不到 FFmpeg 库 ${_lib}（FFMPEG_PATH=${FFMPEG_PATH}）。"
                " 请确认该目录下有 include/ 与 lib/，或用 -DFFMPEG_PATH=<你的路径> 指定。")
        endif()
    endforeach()

    target_link_libraries(ffmpeg_deps INTERFACE
        ${AVCODEC_LIB}
        ${AVFORMAT_LIB}
        ${AVDEVICE_LIB}
        ${AVUTIL_LIB}
        ${SWRESAMPLE_LIB}
        ${SWSCALE_LIB}
        ${AVFILTER_LIB}
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavdevice libavutil libswresample libswscale libavfilter)
    target_link_libraries(ffmpeg_deps INTERFACE PkgConfig::FFMPEG)
endif()

target_link_libraries(encode_video PRIVATE ffmpeg_deps)

# -----------------------------------------------------------------------------
# 静态链接 MinGW 运行时 + 拷贝 libwinpthread（避免 PATH 上别的 MinGW 撞版本，
# 表现为启动即退出 0xC0000139）。说明详见 08-01 的同名段落。
# -----------------------------------------------------------------------------
if(MINGW)
    target_link_options(encode_video PRIVATE -static-libgcc -static-libstdc++)

    get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${_mingw_bin_dir}/libwinpthread-1.dll")
        add_custom_command(TARGET encode_video POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin_dir}/libwinpthread-1.dll"
                    "$<TARGET_FILE_DIR:encode_video>"
            COMMENT "拷贝 libwinpthread-1.dll（MinGW 运行时，避免版本冲突）")
    endif()
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(encode_video PRIVATE Threads::Threads)
endif()

# =============================================================================
#  运行期 DLL 拷贝（Windows）
# =============================================================================
# MinGW/MSVC 对 DLL 都没有 rpath 语义，exe 找不到 avcodec-62.dll 会直接启动失败。
# 构建后自动拷到 exe 同目录 → 不需要改 PATH 就能运行。
# 路径含空格时参数必须整体加引号（qmake 的 -Wl,-rpath 就栽在这上面）。
if(WIN32)
    set(FFMPEG_DLLS
        avcodec-62.dll avutil-60.dll swresample-6.dll
        avformat-62.dll avfilter-11.dll swscale-9.dll avdevice-62.dll)

    foreach(_dll ${FFMPEG_DLLS})
        if(EXISTS "${FFMPEG_PATH}/bin/${_dll}")
            add_custom_command(TARGET encode_video POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FFMPEG_PATH}/bin/${_dll}"
                        "$<TARGET_FILE_DIR:encode_video>"
                COMMENT "拷贝 ${_dll}")
        endif()
    endforeach()
endif()

# =============================================================================
#  用法
# =============================================================================
# 配置 + 构建：
#   cmake -S . -B build -G "MinGW Makefiles" ^
#         -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe ^
#         -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
#   cmake --build build
#
# 运行（DLL 已就位，无需改 PATH）：
#   build\encode_video.exe yuv420p_1280x720.yuv out.h264
#   build\encode_video.exe yuv420p_1280x720.yuv out.h264 libx264 1280 720 25 25
#
# 验证：
#   ffprobe -show_streams out.h264
#   ffplay out.h264
