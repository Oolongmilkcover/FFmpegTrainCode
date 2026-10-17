# =============================================================================
#  08-01-encode_audio.pro
#  音频编码示例（裸 PCM → AAC）
#
#  环境：Qt 6.11.0 + FFmpeg 8.1（libavcodec 62 / libavutil 60）
#
#  ---------------------------------------------------------------------------
#  本工程的性质
#  ---------------------------------------------------------------------------
#  CONFIG -= qt，代码只用 <cstdio>/<cstdlib> 等标准库，**一行 Qt API 都没用**。
#  所以：
#    * 不依赖任何 Qt 库，用哪个 Qt 版本的 kit 编译结果都一样；
#    * 选 Qt 6.11.0 的 kit，只是借用它的 qmake + MinGW 13.1 工具链
#      （C:\Qt\Tools\mingw1310_64），保证和后续 Qt 音视频项目同一套 ABI；
#    * 换 MSVC kit 也能编（FFmpeg 8.1 的 Windows dev 包同时提供 .lib 与
#      .dll.a 两种导入库），只是要在 "x64 Native Tools Command Prompt" 里构建。
#
#  源码语言：C++17（main.cpp）。原始版本是 C（main.c），
#  已备份为 main.c.legacy-c-version.bak 供对照。
#  08-01 / 08-02 / 08-03 三个工程现在统一用同一套 C++ 风格，便于迁进 pusher。
#
#  学习重点在 main.cpp 的注释（按「【知识点 N】」编号组织，讲音频编码流程与
#  API 细节），系统的文字讲解见「音频编码知识点详解.md」。
#  ---------------------------------------------------------------------------
#
#  构建方法（推荐用 CMake，见同目录 CMakeLists.txt）
#      cmake -S . -B build -G "MinGW Makefiles" ^
#            -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
#            -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe ^
#            -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
#      cmake --build build
#
#  构建方法（qmake，本文件）
#      "C:\Qt\6.11.0\mingw_64\bin\qmake.exe" 08-01-encode_audio.pro
#      "C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe"
#
#  两种方式产出的 .aac 文件经 MD5 比对完全一致。
# =============================================================================

TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt
CONFIG += c++17

SOURCES += main.cpp

# -----------------------------------------------------------------------------
# FFmpeg 8.1 依赖库路径
#
# 优先取环境变量 FFMPEG_DIR（便于在别的机器/别的版本上复用本 .pro），
# 没有设置时用本机的默认安装路径（带空格的路径要用 $$quote() 包住）。
#
# 原 .pro 的写法是 $$PWD/ffmpeg-4.2.1-win32-dev，即把 SDK 复制到工程目录里。
# 新版 FFmpeg 8.1 装在 Program Files 下，且 lib/*.lib 同时提供了 MinGW 的
# .dll.a 导入库（libavcodec.dll.a / libavutil.dll.a ...），
# 因此 -lavcodec 这种 GNU 风格写法也能直接用。
# -----------------------------------------------------------------------------
win32 {
    FFMPEG_DIR = $$(FFMPEG_DIR)
    isEmpty(FFMPEG_DIR) {
        FFMPEG_DIR = $$quote(C:/Program Files/ffmpeg/ffmpeg8.1)
    }

    INCLUDEPATH += $$FFMPEG_DIR/include

    # 链接顺序：被依赖的库放后面（libavcodec 依赖 libavutil / libswresample）
    #
    # ⚠ 踩坑记录：不要在路径里用 $$quote() 之外的写法，也**不要**加
    #      QMAKE_LFLAGS += -Wl,-rpath,$$FFMPEG_BIN
    #   因为 "C:/Program Files/..." 中间有空格，MinGW 的 ld 会把 -rpath 的参数
    #   从空格处切断，报错：
    #      ld.exe: cannot find Files/ffmpeg/ffmpeg8.1/bin: No such file or directory
    #   MinGW 对 DLL 也不支持 rpath 语义，运行期还是靠 PATH。正确做法见文件末尾。
    LIBS += -L$$quote($$FFMPEG_DIR/lib) \
            -lavformat   \
            -lavcodec    \
            -lavdevice   \
            -lavfilter   \
            -lavutil     \
            -lswresample \
            -lswscale
}

# -----------------------------------------------------------------------------
# 运行期：让 exe 找到 avcodec-62.dll / avutil-60.dll 等
#
# DLL 不在 exe 同目录时，Windows 会报「找不到 avcodec-62.dll」。三种做法：
#   (1) 把 FFmpeg 的 bin 目录加入系统 PATH（最简单）
#   (2) 在 Qt Creator 里：Projects -> Run -> Environment -> 追加
#         PATH=C:\Program Files\ffmpeg\ffmpeg8.1\bin;%PATH%
#   (3) 构建后自动把 DLL 拷到目标目录（下面这段，默认注释掉，按需启用）
#
# 启用方法：取消注释后重新 qmake + make。
# -----------------------------------------------------------------------------
# win32 {
#     FFMPEG_BIN_DIR = $$FFMPEG_DIR/bin
#     DEST_DIR       = $$OUT_PWD/release
#     FFMPEG_DLLS    = avcodec-62.dll avutil-60.dll swresample-6.dll \
#                      avformat-62.dll avfilter-11.dll swscale-9.dll \
#                      avdevice-62.dll
#     for(dll, FFMPEG_DLLS) {
#         QMAKE_POST_LINK += $$QMAKE_COPY /Y \
#             $$shell_path($$FFMPEG_BIN_DIR/$$dll) \
#             $$shell_path($$DEST_DIR) $$escape_expand(\\n\\t)
#     }
# }

# 显式关掉 deprecated 告警会让"我们用了旧的 AVCodec.sample_fmts"这类提示消失，
# 学习阶段建议保持打开，正好可以看到 FFmpeg 8.1 提示你该换哪个新 API。
# QMAKE_CFLAGS += -Wno-deprecated-declarations
