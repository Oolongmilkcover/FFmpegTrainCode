# =============================================================================
#  08-04-mp4muxer —— CMake 构建脚本（Qt 6.11 + FFmpeg 8.1）
#
#  教学工程：YUV + PCM → H.264 + AAC → MP4，重点是**音视频同步封装**。
#  学习重点在各文件的注释（按「【知识点 N】」编号），配套讲解见同目录
#  《音视频同步封装知识点详解.md》。
#
#  和 08-01/02/03 一样：
#    - 纯 C++17，不链接任何 Qt 库（选 Qt 的 kit 只为拿 MinGW 工具链）
#    - FFmpeg 路径沿用播放器工程的 FFMPEG_PATH 缓存变量 + ffmpeg_deps 目标
#    - POST_BUILD 自动把 FFmpeg DLL 拷到 exe 同目录，运行不用配 PATH
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(mp4muxer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ========================== 源文件 ==========================
# 4 个类各管一件事，职责边界见各 .h 顶部注释
set(PROJECT_SOURCES
    main.cpp
    videoencoder.cpp
    audioencoder.cpp
    audioresampler.cpp
    muxer.cpp
)

set(PROJECT_HEADERS
    videoencoder.h
    audioencoder.h
    audioresampler.h
    muxer.h
)

add_executable(mp4muxer ${PROJECT_SOURCES} ${PROJECT_HEADERS})

# ========================== FFmpeg 依赖 ==========================
add_library(ffmpeg_deps INTERFACE)

if(WIN32)
    set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1" CACHE PATH
        "FFmpeg 安装目录（需含 include/、lib/、bin/），可用 -DFFMPEG_PATH=... 覆盖")

    target_include_directories(ffmpeg_deps INTERFACE ${FFMPEG_PATH}/include)

    # 本工程实际用到：avformat（封装）、avcodec（编码）、avutil、
    #                  swresample（s16→fltp）。其余几个保持和别的工程一致。
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

target_link_libraries(mp4muxer PRIVATE ffmpeg_deps)

# -----------------------------------------------------------------------------
# 静态链接 MinGW 运行时（libstdc++ / libgcc）
#
# 为什么必须做：
#   exe 默认动态依赖 libstdc++-6.dll / libgcc_s_seh-1.dll，运行时由系统按 PATH
#   顺序去找。如果机器上还装着别的 MinGW（比如 C:\mingw64，GCC 15、win32 线程
#   模型），而它排在 PATH 前面，就会加载到**版本不匹配**的 libstdc++，
#   表现为程序一行代码都没执行就退出，退出码 0xC0000139
#   （STATUS_ENTRYPOINT_NOT_FOUND）。
#
#   本项目就是踩了这个坑：用 Qt 的 MinGW 13.1（posix 线程）编译，但 PATH 首位
#   是 C:\mingw64（GCC 15、win32 线程），一运行就 0xC0000139。
#
# 静态链接之后 exe 不依赖任何 MinGW 运行时 DLL，只依赖 FFmpeg 的 DLL
# （那些由下面的 POST_BUILD 拷到 exe 同目录），彻底摆脱 PATH 顺序影响。
# 代价是 exe 大一点（约 2MB），完全可以接受。
# -----------------------------------------------------------------------------
if(MINGW)
    target_link_options(mp4muxer PRIVATE -static-libgcc -static-libstdc++)

    # ---------------------------------------------------------------------
    # 剩一个 libwinpthread-1.dll（posix 线程模型的 libstdc++ 会用到它的符号）。
    # 想用 -Wl,-Bstatic -lwinpthread 静态掉它，但 GNU ld 是「从左到右」解析的，
    # 那句出现在 libstdc++ 之前时引用还没出现，等于没生效（实测仍会动态依赖）。
    #
    # 所以改成：构建后从**编译器自带的 bin 目录**把同版本的 DLL 拷到 exe 旁边。
    # 这样无论 PATH 上有什么别的 MinGW，加载的都是配套版本，
    # 不会出现 0xC0000139（ENTRYPOINT_NOT_FOUND）。
    # ---------------------------------------------------------------------
    get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${_mingw_bin_dir}/libwinpthread-1.dll")
        add_custom_command(TARGET mp4muxer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin_dir}/libwinpthread-1.dll"
                    "$<TARGET_FILE_DIR:mp4muxer>"
            COMMENT "拷贝 libwinpthread-1.dll（MinGW 运行时，避免版本冲突）")
    endif()
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(mp4muxer PRIVATE Threads::Threads)
endif()

# macOS / MinGW 上 <filesystem> 可能需要额外链接（GCC 9 以前需要 stdc++fs）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(mp4muxer PRIVATE stdc++fs)
endif()

# =============================================================================
#  运行期 DLL 拷贝（Windows）
# =============================================================================
if(WIN32)
    set(FFMPEG_DLLS
        avcodec-62.dll avutil-60.dll swresample-6.dll
        avformat-62.dll avfilter-11.dll swscale-9.dll avdevice-62.dll)

    foreach(_dll ${FFMPEG_DLLS})
        if(EXISTS "${FFMPEG_PATH}/bin/${_dll}")
            add_custom_command(TARGET mp4muxer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FFMPEG_PATH}/bin/${_dll}"
                        "$<TARGET_FILE_DIR:mp4muxer>"
                COMMENT "拷贝 ${_dll}")
        endif()
    endforeach()
endif()

# =============================================================================
#  用法
# =============================================================================
# 配置 + 构建：
#   cmake -S . -B build -G "MinGW Makefiles" ^
#         -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
#         -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe ^
#         -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
#   cmake --build build
#
# 生成测试素材（首次）：
#   fetch_test_media.bat
#
# 运行：
#   build\mp4muxer.exe 720x576_yuv420p.yuv 44100_2_s16le.pcm out.mp4
#
# 验证：
#   ffprobe -show_format -show_streams out.mp4
#   ffplay out.mp4
