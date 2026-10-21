// =============================================================================
//  pusher.h —— 推流端主控（mini-OBS 的编排层）
//
//  【知识点 22】线程模型 —— 和播放器镜像对称，但背压方向相反
//  ---------------------------------------------------------------------------
//   [视频采集线程] ──AVFrame──► 帧队列(满了丢最旧) ──► [编码线程] ──AVPacket──┐
//                                                                              ├─► 包队列 ──► [写出线程]
//   [音频采集线程] ──（就地编码，不走帧队列）────────────────AVPacket──────────┘
//
//  为什么音频绕过帧队列（这是刻意的，不是偷懒）：
//    视频丢一帧只是画面抖一下；音频丢一段是"咔哒"一声，体感差得多。
//    麦克风本身带缓冲，编码器偶尔慢一点它会自然反压，所以音频就地编码、不丢。
//
//  【知识点 23】三条硬约定（写的时候别破）
//  ---------------------------------------------------------------------------
//  1. 采集是"实时源"：帧队列满了只能丢最旧，绝不能阻塞采集（否则延迟越堆越大）；
//     包队列相反 —— 落盘时阻塞（文件要完整），推流时才改成丢包（网络卡住不能堆延迟）。
//  2. 跨线程共享只用"归属明确的队列" + 原子量；
//     每个 AVFormatContext / AVCodecContext 只被一个线程碰。
//  3. 停止顺序（见 stop()）：停采集 → 关帧队列 → 编码线程 flush → 关包队列
//     → 写完剩余包 → writeTrailer。
// =============================================================================
#ifndef PUSH_PUSHER_H
#define PUSH_PUSHER_H

#include "capture.h"
#include "dropqueue.h"
#include "encoder.h"
#include "muxer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

struct PusherConfig
{
    VideoSourceConfig video;
    AudioSourceConfig audio;

    // 打开采集源之后，会用**实际协商出来的**宽高/帧率/音频格式回填这个配置
    EncoderConfig encoder;

    // url 为空 = 只采集不输出（本工程的命令行要求必须给，但接口保留了这种可能）
    OutputConfig output;

    bool withVideo = true;
    bool withAudio = false;

    int frameQueueCapacity  = 8;    // 采集 → 编码（故意小：攒太多只会加延迟）
    int packetQueueCapacity = 32;   // 编码 → 写出

    // ---- 断线重连 ----
    int reconnectMaxAttempts = 0;     // 0 = 一直重试（直播的常规选择）
    int reconnectDelayMs     = 1000;  // 首次等待；之后指数退避，上限 15 秒
};

// 队列类型别名，写起来短一点
using PusherFrameQueue  = DropQueue<AVFrame*, AvFrameDeleter>;
using PusherPacketQueue = DropQueue<AVPacket*, AvPacketDeleter>;

class Pusher
{
public:
    /* 本工程刻意不输出日志，所以 start() 失败时要把原因单独暴露出来，
     * 让 main 能映射成不同的退出码（见 main.cpp 的 ExitCode）。
     * 这也是"库不打印、由调用方决定怎么呈现"的常见分层做法。 */
    enum class StartFailure {
        None,
        VideoSource,     // 采集设备打不开
        VideoEncoder,    // x264 打不开
        Output,          // 输出（文件/网络）打不开
    };

    explicit Pusher(PusherConfig cfg);
    ~Pusher();

    Pusher(const Pusher&) = delete;
    Pusher& operator=(const Pusher&) = delete;

    bool start();                   // 打开采集源/编码器/输出，起所有线程
    void stop();                    // 有序停止（见头部的停止顺序），幂等
    bool isRunning() const { return m_running.load(); }

    StartFailure startFailure() const { return m_startFailure; }

private:
    void videoCaptureLoop();        // 读帧 → 帧队列（满了丢最旧）
    void audioCaptureLoop();        // 读 PCM → 就地编码 → 包队列
    void encodeLoop();              // 帧队列 → 视频编码 → 包队列；退出前 flush
    void pushLoop();                // 包队列 → muxer；失败时重连；结束时写 trailer

    bool openOutput();              // 建 muxer + 建流 + 写头（首次和重连共用）
    bool reconnect();               // 带退避的重连

    PusherConfig m_cfg;

    std::atomic<bool> m_running{ false };

    /* 输出 I/O 的"叫停"信号：只给 muxer 用（它挂到 AVIOInterruptCB 上）。
     * 为什么和"停采集"分成两个标志：
     *   停采集之后还要把剩余包写完、尤其 trailer 必须写完，那时不能打断 I/O。
     *   详见 muxer.cpp 的 writeTrailer()。
     * 采集侧的叫停由各采集源自己的 requestStop() 负责（见 capture.h）。 */
    std::atomic<bool> m_abortIo{ false };

    std::atomic<bool> m_waitingKeyframe{ false };  // 重连后要等一个关键帧才能继续写
    bool m_outputGivenUp = false;                  // 重连次数用尽，之后只丢包不重试
    StartFailure m_startFailure = StartFailure::None;

    std::unique_ptr<IVideoSource> m_video;
    std::unique_ptr<IAudioSource> m_audio;

    std::unique_ptr<IVideoEncoder> m_videoEnc;
    std::unique_ptr<IAudioEncoder> m_audioEnc;

    /* muxer 只被推流线程碰，所以裸 unique_ptr 就够，不需要锁。
     * 重连时由推流线程自己换掉它。 */
    std::unique_ptr<IMuxer> m_muxer;
    bool m_outputReady  = false;   // 只在推流线程读写
    int  m_reconnects   = 0;

    /* 每路流在容器里的下标。由 openOutput() 按"实际建了几路"算出：
     *   视频+音频 → 视频 0、音频 1
     *   只有音频 → 音频 0
     * 编码器产包时不知道这个下标，由下面两个 sink 在 clone 之后写上（见知识点 12.1）。 */
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    std::unique_ptr<PusherFrameQueue>  m_frameQueue;
    std::unique_ptr<PusherPacketQueue> m_packetQueue;

    std::thread m_videoThread;
    std::thread m_audioThread;
    std::thread m_encodeThread;
    std::thread m_pushThread;
};

#endif // PUSH_PUSHER_H
