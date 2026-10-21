# =============================================================================
#  08-05-capture_push —— CMake 构建脚本（Qt 6.11 + FFmpeg 8.1）
#
#  教学工程：采集 → 编码(H.264+AAC) → 封装 → 落盘 mp4/flv 或 RTMP 推流
#  参考实现：QtFFmpegPlayer/Player/QtPlayer/pusher/（mini-OBS 风格的分层）
#  学习重点在各文件的注释（按「【知识点 N】」编号），
#  配套讲解见同目录《采集推流知识点详解.md》。
#
#  和 08-01~08-04 保持一致：
#    - 纯 C++17，不链接任何 Qt 库（选 Qt 的 kit 只为拿 MinGW 工具链）
#    - FFmpeg 路径用 FFMPEG_PATH 缓存变量 + ffmpeg_deps 目标
#    - POST_BUILD 自动把 FFmpeg DLL 拷到 exe 同目录，运行不用配 PATH
#    - 静态链接 MinGW 运行时，避免 PATH 上别的 MinGW 撞版本（0xC0000139）
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(capture_push VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ========================== 源文件 ==========================
# 分层：main（配置）→ pusher（编排/线程）→ capture/encoder/muxer（各干一件事）
set(PROJECT_SOURCES
    main.cpp
    pusher.cpp
    capture.cpp
    encoder.cpp
    muxer.cpp
)

set(PROJECT_HEADERS
    pusher.h
    dropqueue.h
    capture.h
    encoder.h
    muxer.h
)

add_executable(capture_push ${PROJECT_SOURCES} ${PROJECT_HEADERS})

# 多线程：本工程用了 std::thread
find_package(Threads REQUIRED)

# ========================== FFmpeg 依赖 ==========================
add_library(ffmpeg_deps INTERFACE)

if(WIN32)
    set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1" CACHE PATH
        "FFmpeg 安装目录（需含 include/、lib/、bin/），可用 -DFFMPEG_PATH=... 覆盖")

    target_include_directories(ffmpeg_deps INTERFACE ${FFMPEG_PATH}/include)

    # 本工程用到全套：avdevice（dshow 采集）、avformat（设备/封装）、
    # avcodec（编码/解码）、avutil、swresample（s16→fltp）、swscale（→yuv420p）
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

target_link_libraries(capture_push PRIVATE ffmpeg_deps Threads::Threads)

# -----------------------------------------------------------------------------
# 静态链接 MinGW 运行时（libstdc++ / libgcc）+ 拷贝 libwinpthread
#
# 为什么要做（08-04 实测踩到的坑）：
#   exe 默认动态依赖 libstdc++-6.dll / libgcc_s_seh-1.dll，运行时按 PATH 顺序查找。
#   如果机器上还装着别的 MinGW（例如 C:\mingw64，GCC 15、win32 线程模型），
#   而它排在 PATH 前面，就会加载到版本不匹配的运行时 ——
#   程序一行代码都没执行就退出，退出码 0xC0000139（STATUS_ENTRYPOINT_NOT_FOUND）。
# -----------------------------------------------------------------------------
if(MINGW)
    target_link_options(capture_push PRIVATE -static-libgcc -static-libstdc++)

    get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${_mingw_bin_dir}/libwinpthread-1.dll")
        add_custom_command(TARGET capture_push POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin_dir}/libwinpthread-1.dll"
                    "$<TARGET_FILE_DIR:capture_push>"
            COMMENT "拷贝 libwinpthread-1.dll（MinGW 运行时，避免版本冲突）")
    endif()
endif()

# =============================================================================
#  运行期 DLL 拷贝（Windows）
# =============================================================================
# 本工程比前几个多了 avdevice-62.dll（采集设备），它必须有。
if(WIN32)
    set(FFMPEG_DLLS
        avcodec-62.dll avutil-60.dll swresample-6.dll
        avformat-62.dll avfilter-11.dll swscale-9.dll avdevice-62.dll)

    foreach(_dll ${FFMPEG_DLLS})
        if(EXISTS "${FFMPEG_PATH}/bin/${_dll}")
            add_custom_command(TARGET capture_push POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FFMPEG_PATH}/bin/${_dll}"
                        "$<TARGET_FILE_DIR:capture_push>"
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
# 运行（不需要任何硬件的全链路自检）：
#   build\capture_push.exe --synthetic-video --synthetic-audio --output out.mp4 --duration 5
#
# 真实摄像头（先用 ffmpeg 查设备名）：
#   ffmpeg -list_devices true -f dshow -i dummy
#   build\capture_push.exe --video "video=AMD Privacy View camera" ^
#                          --synthetic-audio --output out.mp4 --duration 5
#
# 推到本地 RTMP（先起 SRS / nginx-rtmp）：
#   build\capture_push.exe --video "video=..." --audio "audio=..." ^
#                          --output rtmp://127.0.0.1/live/demo --duration 30
#
# 验证：
#   ffprobe -show_format -show_streams out.mp4
#   ffplay out.mp4
