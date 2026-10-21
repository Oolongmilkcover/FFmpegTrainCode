// =============================================================================
//  capture.h —— 采集源（mini-OBS 的 "source" 层）
//
//  【知识点 3】采集层要解决的三件事
//  ---------------------------------------------------------------------------
//  1) **格式不统一**：摄像头给什么五花八门
//         YUY2 / NV12 / MJPEG / H264 ...
//     而编码器只吃 yuv420p，所以采集层统一出口就是 yuv420p（视频）
//     和"设备原样 PCM"（音频）。
//
//  2) **参数是"协商"出来的**
//     我们在 open() 里给的分辨率/帧率/采样率只是**提议**，设备可以拒绝，
//     也可能给个近似的值。所以 open() 成功后必须用 actual 值（width()/fps()）
//     回填给编码器 —— 拿提议值去配编码器，编出来就是花屏或时长错乱。
//
//  3) **阻塞读必须有出口**
//     设备上的 av_read_frame 会一直阻塞等下一帧。程序要退出时如果线程卡在里面，
//     就只能强杀进程 —— 文件写不出 trailer、mp4 的 moov 就没了。
//     所以接口里有 requestStop()，实现里挂 AVIOInterruptCB。
// ---------------------------------------------------------------------------
//
//  【知识点 4】为什么音视频是两个独立的源，而不是一个"采集器"
//  ---------------------------------------------------------------------------
//  * 每个 AVFormatContext 都不是线程安全的。音视频各开一个上下文 →
//    各自只被一个线程碰，这一层就完全不需要锁（并发正确性靠"归属明确"保证）。
//  * 两者的节奏、阻塞特性、丢帧策略都不同（视频可丢、音频不能丢），
//    分开之后互不干扰。
//  * 音画对齐不在这层做：两边都从 0 开始生成单调的微秒时间戳，
//    真正对齐交给推流端的统一时钟（见 pusher.cpp）。
// =============================================================================
#ifndef PUSH_CAPTURE_H
#define PUSH_CAPTURE_H

#include <memory>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

// ============================== 视频采集 ==============================

struct VideoSourceConfig
{
    // 设备名：
    //   Windows(dshow) → "video=AMD Privacy View camera"（注意 video= 前缀，这是 dshow 的命名格式）
    //   Linux(v4l2)    → "/dev/video0"
    // 留空 + synthetic=true 时用内置合成源，不需要任何硬件
    std::string device;

    // 采集后端：Windows → "dshow"，Linux → "v4l2"
    std::string inputFormat;

    // 下面三个是"提议"，设备不一定照办；open() 之后要读 width()/height()/fps() 取真值
    int width  = 1280;
    int height = 720;
    int fps    = 30;

    // 用内置合成源（彩色渐变 + 移动竖条）。没有摄像头时也能把整条链路跑通
    bool synthetic = false;
};

class IVideoSource
{
public:
    virtual ~IVideoSource() = default;

    virtual bool open(const VideoSourceConfig& cfg) = 0;
    virtual void close() = 0;

    /* 让阻塞中的 readFrame() 尽快返回（给 stop 用）。
     * 实现里把它接到 AVFormatContext 的 interrupt_callback 上。 */
    virtual void requestStop() = 0;

    /* 取一帧，**统一转换成 yuv420p**。
     * 返回的帧带引用：调用方负责 av_frame_free。
     * 返回 nullptr = 出错 / 已 requestStop / 设备结束。
     * 帧上的 pts 是我们自己生成的单调微秒值（不用设备时间戳，见知识点 5）。 */
    virtual AVFrame* readFrame() = 0;

    // open() 之后有效：设备实际协商出来的参数
    virtual int width()  const = 0;
    virtual int height() const = 0;
    virtual int fps()    const = 0;
};

std::unique_ptr<IVideoSource> createVideoSource(const VideoSourceConfig& cfg);

// ============================== 音频采集 ==============================

struct AudioSourceConfig
{
    // Windows(dshow) → "audio=Microphone (High Definition Audio Device)"
    // Linux(alsa)    → "default" 或 "hw:0"
    std::string device;

    // 采集后端：Windows → "dshow"，Linux → "alsa"
    std::string inputFormat;

    int sampleRate = 48000;   // 提议值
    int channels   = 2;       // 提议值

    bool synthetic = false;   // 用内置正弦波（不需要麦克风）
};

class IAudioSource
{
public:
    virtual ~IAudioSource() = default;

    virtual bool open(const AudioSourceConfig& cfg) = 0;
    virtual void close() = 0;
    virtual void requestStop() = 0;

    /* 取一帧 PCM（原生格式，常见 s16）。
     * 这一层**不做重采样**：目标格式应该由编码器决定，采集层不该猜。
     * 转换交给编码器内部的 swresample（见 encoder.cpp）。 */
    virtual AVFrame* readFrame() = 0;

    virtual int             sampleRate() const = 0;
    virtual int             channels()   const = 0;
    virtual AVSampleFormat  sampleFormat() const = 0;

    // 合成源每帧固定这么多采样点；设备源按设备给的来（这里给个建议值）
    virtual int nominalSamplesPerFrame() const = 0;
};

std::unique_ptr<IAudioSource> createAudioSource(const AudioSourceConfig& cfg);

/*
 * 【知识点 5】时间戳为什么不用设备给的，而是自己生成
 * ---------------------------------------------------------------------------
 * 设备时间戳有两个麻烦：
 *   - 单位不统一：dshow 给的是 100ns，v4l2 给的是自己的 timebase，alsa 又不同；
 *   - 起点不一致：音视频两个设备各自从陌生的初值开始，无法直接比较。
 *
 * 我们统一约定：**两路都从 0 开始，按固定步长累加微秒**
 *     视频：pts = 帧序号 * 1000000 / fps
 *     音频：pts = 采样点累计 * 1000000 / sampleRate
 *
 * 好处：两路天然同源，可以直接比大小（推流端就是靠这个决定先送谁）；
 *      而且完全确定性，同一个输入每次跑出来的时间轴都一样，方便对照实验。
 * 代价：如果设备实际帧率不稳（比如 29.97 被当成 30），会缓慢漂移。
 *      真实产品可以拿设备时间戳做校正，本工程为了讲清主干就不引入这个复杂度。
 * ---------------------------------------------------------------------------
 */
constexpr int kMicrosPerSecond = 1000000;

#endif // PUSH_CAPTURE_H
