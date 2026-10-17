# =============================================================================
#  08-01-encode_audio —— CMake 构建脚本（Qt 6.11 + FFmpeg 8.1）
#
#  教学工程：读本地裸 PCM → AAC 编码（含 ADTS 头）
#  学习重点在 main.cpp 的注释（按「【知识点 N】」编号）。
#  配套系统讲解：同目录《音频编码知识点详解.md》。
#
#  FFmpeg 路径与依赖收集方式直接沿用了同一套 Qt6 播放器工程
#  （QtFFmpegPlayer/Player/QtPlayer/CMakeLists.txt）的做法：
#     - FFMPEG_PATH 作为 CACHE PATH，可在命令行 -DFFMPEG_PATH=... 覆盖
#     - 用一个 INTERFACE 目标 ffmpeg_deps 统一封装 "头文件路径 + 库"
#     - WIN32 走 find_library，其它平台走 pkg-config
#  这样后续新增 target 时只要 target_link_libraries(xxx PRIVATE ffmpeg_deps)，
#  不会出现 "忘了加 include 目录 → 找不到 libavcodec/avcodec.h" 的问题。
#
#  源码语言：C++17（main.cpp）。
#  08-01（音频编码）/ 08-02（视频编码）/ 08-03（封装）三个工程
#  统一用同一套 C++ 风格 + 同一套 CMake 配置，便于直接迁进 pusher。
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# 纯 C++17，不链接 Qt 库 —— 选 Qt 6.11 的 kit 只是借用它的 MinGW 13.1 工具链
project(encode_audio VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 默认用 Release：编码这类计算密集代码在 Debug 下慢 5~10 倍
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ========================== 源文件 ==========================
set(PROJECT_SOURCES
    main.cpp
)

add_executable(encode_audio ${PROJECT_SOURCES})

# =============================================================================
#  FFmpeg 依赖
# =============================================================================
add_library(ffmpeg_deps INTERFACE)

if(WIN32)
    # 和播放器工程保持完全一致的默认路径与变量名，方便两个工程互相拷贝配置。
    set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1" CACHE PATH
        "FFmpeg 安装目录（需含 include/、lib/、bin/），可用 -DFFMPEG_PATH=... 覆盖")

    target_include_directories(ffmpeg_deps INTERFACE ${FFMPEG_PATH}/include)

    # find_library 会在 ${FFMPEG_PATH}/lib 下找 avcodec.lib（MSVC 风格）
    # 或 libavcodec.dll.a（MinGW 风格）。
    # FFmpeg 8.1 的 Windows dev 包两种导入库都提供，所以 MSVC / MinGW 都能用。
    find_library(AVCODEC_LIB    avcodec    PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVFORMAT_LIB   avformat   PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVDEVICE_LIB   avdevice   PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVUTIL_LIB     avutil     PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIB swresample PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWSCALE_LIB    swscale    PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVFILTER_LIB   avfilter   PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)

    # 缺库时给出可读的报错，而不是让链接器吐一堆 undefined reference
    foreach(_lib AVCODEC_LIB AVFORMAT_LIB AVDEVICE_LIB AVUTIL_LIB
                 SWRESAMPLE_LIB SWSCALE_LIB AVFILTER_LIB)
        if(NOT ${_lib})
            message(FATAL_ERROR
                "找不到 FFmpeg 库 ${_lib}（FFMPEG_PATH=${FFMPEG_PATH}）。请确认该目录下"
                " 有 include/ 与 lib/，或用 -DFFMPEG_PATH=<你的路径> 指定。")
        endif()
    endforeach()

    # 本工程实际只用到 avcodec / avutil / swresample，其余是为了与播放器工程
    # 共用同一个 ffmpeg_deps 定义而保留的，删掉也能编过。
    target_link_libraries(ffmpeg_deps INTERFACE
        ${AVCODEC_LIB}
        ${AVFORMAT_LIB}
        ${AVDEVICE_LIB}      # 采集设备（dshow）用；本示例不用
        ${AVUTIL_LIB}
        ${SWRESAMPLE_LIB}    # ★ 本工程用到了：s16le -> fltp 重采样
        ${SWSCALE_LIB}
        ${AVFILTER_LIB}
    )
else()
    # Linux/macOS：走 pkg-config，PkgConfig::FFMPEG 自带 include 与链接参数
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavdevice libavutil libswresample libswscale libavfilter)
    target_link_libraries(ffmpeg_deps INTERFACE PkgConfig::FFMPEG)
endif()

target_link_libraries(encode_audio PRIVATE ffmpeg_deps)

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(encode_audio PRIVATE Threads::Threads)
endif()

# =============================================================================
#  运行期 DLL 拷贝（Windows）
# =============================================================================
# MinGW/MSVC 对 DLL 都没有 rpath 语义，exe 找不到 avcodec-62.dll 时会直接启动失败。
# 这里在构建后自动把 FFmpeg 的 DLL 拷到 exe 同目录，做到「构建完就能直接运行」。
#
# 注意：路径含空格时参数必须整体用引号包住，否则 copy 会把
# "C:/Program Files/..." 从空格处切断（qmake 的 -Wl,-rpath 就栽在这上面）。
if(WIN32)
    set(FFMPEG_DLLS
        avcodec-62.dll avutil-60.dll swresample-6.dll
        avformat-62.dll avfilter-11.dll swscale-9.dll avdevice-62.dll)

    foreach(_dll ${FFMPEG_DLLS})
        if(EXISTS "${FFMPEG_PATH}/bin/${_dll}")
            add_custom_command(TARGET encode_audio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FFMPEG_PATH}/bin/${_dll}"
                        "$<TARGET_FILE_DIR:encode_audio>"
                COMMENT "拷贝 ${_dll}")
        endif()
    endforeach()
endif()

# =============================================================================
#  用法
# =============================================================================
# 配置 + 构建（MinGW 13.1，Qt 6.11 自带的那套）：
#   cmake -S . -B build -G "MinGW Makefiles" ^
#         -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
#         -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
#   cmake --build build
#
# 运行（DLL 已自动拷到 build 目录，无需改 PATH）：
#   build\encode_audio.exe 48000_2_f32le.pcm out.aac
#   build\encode_audio.exe 48000_2_s16le.pcm out.aac aac 48000 2 s16le
#
# 只用到 avcodec/avutil/swresample，未链接 Qt，因此不需要 CMAKE_PREFIX_PATH。
