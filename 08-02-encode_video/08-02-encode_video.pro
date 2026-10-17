# =============================================================================
#  08-02-encode_video.pro
#  视频编码示例：读本地 YUV 裸数据 → libx264 编成 H.264
#
#  环境：Qt 6.11.0 + FFmpeg 8.1（libavcodec 62 / libavutil 60）
#
#  本工程是纯 C++17，CONFIG -= qt，不链接任何 Qt 库。
#  选 Qt 6.11 的 kit 只是借用它的 qmake + MinGW 13.1 工具链，
#  保证和 pusher 工程（QtFFmpegPlayer）同一套 ABI。
#
#  推荐用 CMake（见同目录 CMakeLists.txt），本文件作为 qmake 备选。
#       cmake -S . -B build -G "MinGW Makefiles" ^
#             -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
#             -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe ^
#             -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
#       cmake --build build
#
#  qmake 方式：
#       "C:\Qt\6.11.0\mingw_64\bin\qmake.exe" 08-02-encode_video.pro
#       "C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe"
#       # release\ 下没有 DLL，运行前需要把 ffmpeg8.1\bin 加进 PATH
# =============================================================================

TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt
CONFIG += c++17

SOURCES += main.cpp

win32 {
    # FFMPEG_DIR 优先取环境变量，便于换机器/换版本
    FFMPEG_DIR = $$(FFMPEG_DIR)
    isEmpty(FFMPEG_DIR) {
        FFMPEG_DIR = $$quote(C:/Program Files/ffmpeg/ffmpeg8.1)
    }

    INCLUDEPATH += $$FFMPEG_DIR/include

    # 注意：不要加 -Wl,-rpath,<含空格的路径>，ld 会从空格处切断参数
    LIBS += -L$$quote($$FFMPEG_DIR/lib) \
            -lavformat   \
            -lavcodec    \
            -lavdevice   \
            -lavfilter   \
            -lavutil     \
            -lswresample \
            -lswscale
}
