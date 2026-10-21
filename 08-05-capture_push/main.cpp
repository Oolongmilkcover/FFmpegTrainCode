// =============================================================================
//  08-05-capture_push / main.cpp
//  采集 → 编码(H.264 + AAC) → 封装 → 落盘 mp4/flv 或 RTMP 推流
//
// ===========================================================================
//  这份代码要学什么
// ===========================================================================
//  08-01 ~ 08-04 都是「从文件读 → 编码/封装 → 写文件」，数据源是死的、速度由自己控制。
//  本工程第一次接上**实时源**，于是多出来一整类问题：
//
//    * 采集是"设备到点就出帧"，你处理不过来它不会等你 → 必须有队列 + 丢弃策略
//    * 设备参数是协商出来的（提议 1280x720，可能给 640x480）→ 必须回填给编码器
//    * 设备阻塞读必须能被叫停，否则退出时卡死 → AVIOInterruptCB
//    * 三条流水线速度不同 → 必须拆线程，否则一处慢拖累全链
//    * 网络随时会断 → 重连 + 重连后等关键帧
//
//  代码结构（每一层只对下一层负责，对应 mini-OBS 的分层）：
//
//      main.cpp            命令行 → 配置；控制运行时长
//        └── pusher.cpp    编排：4 条线程 + 2 个队列 + 重连   【知识点 22~31】
//              ├── capture.cpp   采集源（设备 / 合成）        【知识点 3~8】
//              ├── encoder.cpp   编码器（x264 / AAC）         【知识点 9~16】
//              └── muxer.cpp     封装输出（文件 / RTMP）      【知识点 17~21】
//        └── dropqueue.h   有界队列（两种"满了"策略）        【知识点 1~2】
//
//  ---------------------------------------------------------------------------
//  本工程刻意**不输出任何日志**。
//  想确认跑出了什么，用文件本身验证（见下），或者自己加一行 printf 到
//  pusher.cpp 里你想观察的位置 —— 调用链是干净的，加日志不会破坏结构。
//  ---------------------------------------------------------------------------
//  环境：Qt 6.11.0 (MinGW 13.1.0) + FFmpeg 8.1
//        纯 C++17，不链接 Qt 库（只用 Qt 的工具链）
//
//  用法（详见 --help）：
//     # 1. 纯合成源，不需要任何硬件：最省事的全链路自检
//     capture_push --synthetic-video --synthetic-audio --output out.mp4 --duration 5
//
//     # 2. 真实摄像头（dshow）+ 合成音频
//     capture_push --video "video=AMD Privacy View camera" --synthetic-audio \
//                  --output out.mp4 --duration 5
//
//     # 3. 推到本地 SRS / nginx-rtmp（先起服务端）
//     capture_push --video "video=..." --audio "audio=..." \
//                  --output rtmp://127.0.0.1/live/demo --duration 30
//
//  验证产物：
//     ffprobe -show_format -show_streams out.mp4
//     ffplay out.mp4
//     ffprobe -show_packets out.flv | findstr pts_time     # 看包时间戳是否单调
//
//  退出码（程序不打印任何东西，只能靠它判断结果）：
//     0  正常结束
//     1  命令行参数错误
//     2  视频采集源打开失败（设备名写错 / 被占用 / 没插）
//     3  视频编码器打开失败（这份 FFmpeg 没编进 libx264？）
//     4  输出打开失败（路径不可写 / 推流地址连不上）
// ===========================================================================

#include "pusher.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

// -----------------------------------------------------------------------------
// 退出码：程序静默运行，所以把失败原因编码进退出码，方便脚本判断
// -----------------------------------------------------------------------------
enum ExitCode {
    kOk             = 0,
    kBadArgs        = 1,
    kVideoSrcFail   = 2,
    kVideoEncFail   = 3,
    kOutputFail     = 4,
};

// Ctrl+C 置位。必须用 std::atomic（信号处理器里只能碰这种无锁的简单类型）
std::atomic<bool> g_interrupted{ false };

void onSignal(int)
{
    g_interrupted.store(true);
}

void printUsage(const char* prog)
{
    std::printf(
        "用法: %s --output <file|rtmp://...> [选项]\n"
        "\n"
        "必须给一个输出：\n"
        "  --output <path|url>     落盘：out.mp4 / out.flv\n"
        "                          推流：rtmp://127.0.0.1/live/demo\n"
        "  --format <name>         容器格式，留空按 output 猜（.mp4→mp4，rtmp://→flv）\n"
        "\n"
        "视频源（二选一）：\n"
        "  --video <device>        dshow 设备名，形如 \"video=AMD Privacy View camera\"\n"
        "  --synthetic-video       用内置合成画面，不需要摄像头\n"
        "  --video-format <fmt>    采集后端，Windows 默认 dshow\n"
        "  --size <WxH>            提议分辨率（默认 1280x720，设备可能不照办）\n"
        "  --fps <n>               提议帧率（默认 30）\n"
        "\n"
        "音频源（可选）：\n"
        "  --audio <device>        dshow 设备名，形如 \"audio=Microphone (...)\"\n"
        "  --synthetic-audio       用内置 440Hz 正弦波\n"
        "  --audio-format <fmt>    采集后端，Windows 默认 dshow\n"
        "  --audio-rate <n>        提议采样率（默认 48000）\n"
        "  --audio-channels <n>    提议声道数（默认 2）\n"
        "\n"
        "编码参数（学习用的旋钮）：\n"
        "  --bitrate <bps>         视频码率（默认 4000000）\n"
        "  --audio-bitrate <bps>   音频码率（默认 128000）\n"
        "  --preset <name>         x264 preset（默认 veryfast）\n"
        "  --gop <n>               GOP 大小（默认 = fps，即 1 秒一个关键帧）\n"
        "  --max-bframes <n>       B 帧数（默认 0，推流建议保持 0）\n"
        "  --no-zerolatency        关掉 tune=zerolatency（默认开）\n"
        "  --cbr                   把码率钉住（接近 CBR）\n"
        "\n"
        "运行控制：\n"
        "  --duration <sec>        跑多少秒，0 = 一直跑到 Ctrl+C（默认 0）\n"
        "  --no-reconnect          断线不重连（默认无限重连）\n"
        "  --reconnect-delay <ms>  首次重连等待（默认 1000，之后指数退避）\n"
        "  --help                  打印本帮助\n"
        "\n"
        "查设备名（本程序不打印，用 ffmpeg 查）：\n"
        "  ffmpeg -list_devices true -f dshow -i dummy\n"
        "\n"
        "例：\n"
        "  %s --synthetic-video --synthetic-audio --output out.mp4 --duration 5\n"
        "  %s --video \"video=OBS Virtual Camera\" --synthetic-audio --output out.mp4 --duration 5\n"
        "  %s --video \"video=...\" --audio \"audio=...\" --output rtmp://127.0.0.1/live/demo\n",
        prog, prog, prog, prog);
}

/* 解析 "1280x720" 这种尺寸字符串 */
bool parseSize(const char* s, int* w, int* h)
{
    if (!s) return false;
    const char* x = std::strchr(s, 'x');
    if (!x) x = std::strchr(s, 'X');
    if (!x) return false;
    const int ww = std::atoi(std::string(s, static_cast<size_t>(x - s)).c_str());
    const int hh = std::atoi(x + 1);
    if (ww <= 0 || hh <= 0) return false;
    *w = ww;
    *h = hh;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    // ---------------------------------------------------------------- 默认配置
    PusherConfig cfg;

#if defined(_WIN32)
    cfg.video.inputFormat = "dshow";
    cfg.audio.inputFormat = "dshow";
#else
    cfg.video.inputFormat = "v4l2";
    cfg.audio.inputFormat = "alsa";
    cfg.video.device      = "/dev/video0";
    cfg.audio.device      = "default";
#endif

    cfg.withVideo = false;    // 由 --video / --synthetic-video 打开
    cfg.withAudio = false;    // 由 --audio / --synthetic-audio 打开

    // ---------------------------------------------------------------- 命令行
    bool wantHelp = false;
    int  durationSeconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("参数 %s 缺少取值\n", name);
                wantHelp = true;
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--help" || a == "-h") { wantHelp = true; }
        else if (a == "--output")         { if (const char* v = next("--output")) cfg.output.url = v; }
        else if (a == "--format")         { if (const char* v = next("--format")) cfg.output.format = v; }
        else if (a == "--duration")       { if (const char* v = next("--duration")) durationSeconds = std::atoi(v); }
        else if (a == "--video")          { if (const char* v = next("--video"))  { cfg.video.device = v; cfg.withVideo = true; } }
        else if (a == "--video-format")   { if (const char* v = next("--video-format")) cfg.video.inputFormat = v; }
        else if (a == "--synthetic-video"){ cfg.video.synthetic = true; cfg.withVideo = true; }
        else if (a == "--size")           { if (const char* v = next("--size")) parseSize(v, &cfg.video.width, &cfg.video.height); }
        else if (a == "--fps")            { if (const char* v = next("--fps")) cfg.video.fps = std::atoi(v); }
        else if (a == "--audio")          { if (const char* v = next("--audio")) { cfg.audio.device = v; cfg.withAudio = true; } }
        else if (a == "--audio-format")   { if (const char* v = next("--audio-format")) cfg.audio.inputFormat = v; }
        else if (a == "--synthetic-audio"){ cfg.audio.synthetic = true; cfg.withAudio = true; }
        else if (a == "--audio-rate")     { if (const char* v = next("--audio-rate")) cfg.audio.sampleRate = std::atoi(v); }
        else if (a == "--audio-channels") { if (const char* v = next("--audio-channels")) cfg.audio.channels = std::atoi(v); }
        else if (a == "--bitrate")        { if (const char* v = next("--bitrate")) cfg.encoder.videoBitrate = std::atoi(v); }
        else if (a == "--audio-bitrate")  { if (const char* v = next("--audio-bitrate")) cfg.encoder.audioBitrate = std::atoi(v); }
        else if (a == "--preset")         { if (const char* v = next("--preset")) cfg.encoder.preset = v; }
        else if (a == "--gop")            { if (const char* v = next("--gop")) cfg.encoder.gopSize = std::atoi(v); }
        else if (a == "--max-bframes")    { if (const char* v = next("--max-bframes")) cfg.encoder.maxBFrames = std::atoi(v); }
        else if (a == "--no-zerolatency") { cfg.encoder.zerolatency = false; }
        else if (a == "--cbr")            { cfg.encoder.cbr = true; }
        else if (a == "--no-reconnect")   { cfg.reconnectMaxAttempts = 1; }
        else if (a == "--reconnect-delay"){ if (const char* v = next("--reconnect-delay")) cfg.reconnectDelayMs = std::atoi(v); }
        else {
            std::printf("未知参数: %s\n", a.c_str());
            wantHelp = true;
        }
    }

    if (wantHelp || argc == 1) {
        printUsage(argv[0]);
        return kBadArgs;
    }

    // ---------------------------------------------------------------- 参数校验
    if (cfg.output.url.empty()) {
        printUsage(argv[0]);
        return kBadArgs;
    }
    if (!cfg.withVideo && !cfg.withAudio) {
        std::printf("至少要有一路输入：--video / --synthetic-video / --audio / --synthetic-audio\n");
        return kBadArgs;
    }
    if (cfg.reconnectDelayMs < 0) cfg.reconnectDelayMs = 1000;

    // ---------------------------------------------------------------- Ctrl+C
    std::signal(SIGINT, onSignal);

    // ---------------------------------------------------------------- 启动
    {
        Pusher pusher(cfg);
        if (!pusher.start()) {
            /*
             * 静默运行，所以只能靠退出码区分失败原因。
             * start() 内部已经做过"音频失败退化成纯视频"的处理，
             * 走到这里的 false 只可能是视频源 / 视频编码器 / 输出三者之一。
             */
            switch (pusher.startFailure()) {
                case Pusher::StartFailure::VideoSource:  return kVideoSrcFail;
                case Pusher::StartFailure::VideoEncoder: return kVideoEncFail;
                case Pusher::StartFailure::Output:       return kOutputFail;
                case Pusher::StartFailure::None:         break;
            }
            return kBadArgs;
        }

        // ------------------------------------------------------------ 运行
        /* 【知识点 32】主线程在这里只做"计时/等待"，不参与数据通路
         * ---------------------------------------------------------------------------
         * 采集、编码、写出都由工作线程负责，主线程干预它们反而会引入锁。
         * 它只做两件事：等够时长、收到 Ctrl+C。
         * 真正的收尾全部在 pusher.stop() 里按固定顺序完成（见 pusher.cpp）。
         * --------------------------------------------------------------------------- */
        const auto t0 = std::chrono::steady_clock::now();
        while (pusher.isRunning() && !g_interrupted.load()) {
            if (durationSeconds > 0) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - t0).count();
                if (elapsed >= durationSeconds) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        pusher.stop();    // 幂等
    }

    return kOk;
}

/*
 * ===========================================================================
 * 附：这一层为什么可以做到"完全不输出"
 * ===========================================================================
 * 参考工程（pusher/）为了边调边看，打了大量 qInfo/qWarning：
 * 打开成功、每秒统计、每个包的时间戳、重连次数……
 * 那些日志对"调通"很有用，但会把**调用链**淹掉，读代码时很难看清
 * "一帧数据到底经过了哪几步"。
 *
 * 本工程的做法：
 *   - 正常路径不发一行日志，让"数据流"成为唯一线索；
 *   - 失败用**退出码**表达（见文件头），不靠日志；
 *   - 想观察中间状态时，自己在 pusher.cpp 的 while 循环里加 printf，
 *     那几处正好是四个线程各自的"心跳点"。
 *
 * 如果你要把这套代码并回播放器工程（pusher/），把 printf 换成 qInfo 即可，
 * 分层和线程模型都不用动。
 * ===========================================================================
 */
