// =============================================================================
//  encoder.h —— 编码层（mini-OBS 的 "encoder" 层）
//
//  【知识点 9】为什么编码器要包成接口 + 工厂
//  ---------------------------------------------------------------------------
//  上层（Pusher）只关心三件事：喂帧、收包、flush。它不该知道用的是
//  libx264 还是 h264_nvenc，也不该知道 AAC 内部怎么重采样。
//
//  所以这里定义 IVideoEncoder / IAudioEncoder 两个纯接口，
//  具体实现藏在 encoder.cpp 的匿名命名空间里，外面只能通过工厂拿到：
//      createVideoEncoder(cfg) → 唯一指针
//
//  要加硬件编码（NVENC / QSV / VideoToolbox）时，只在工厂里按
//  cfg.videoCodec 分支即可，Pusher 一行都不用改。
// ---------------------------------------------------------------------------
//
//  【知识点 10】PacketSink —— 编码器把包"交出去"的那根管子
//  ---------------------------------------------------------------------------
//  ① 为什么需要它：一次 encode() 可能产出 0 个、1 个、好几个包
//     （有 B 帧/前瞻时包会延迟出；flush 时一次吐一堆）。
//     而函数返回值只有一个 bool（喂帧成不成），装不下"这批包"，
//     所以需要第二条出口 —— 这就是 PacketSink。
//
//         bool encode(AVFrame*)     ← 出口 1：成不成
//         void sink(AVPacket*)      ← 出口 2：出来了什么
//
//  ② 它是什么：std::function<void(AVPacket*)>，即"任何能接收一个 AVPacket*
//     的可调用对象"——函数指针、lambda、仿函数都行。它不规定接收者是谁。
//
//  ③ 两侧怎么用：
//         生产侧（encoder.cpp）:  if (sink) sink(pkt);   // 产出一个就交一个
//         消费侧（pusher.cpp）:   enc->setSink([this](AVPacket* pkt) { ... });
//     编码器不认识队列、也不认识 muxer，它只知道"有人要包，我按这个签名交出去"。
//
//  ④ 为什么不用别的写法：
//         std::vector<AVPacket*> encode(...)  每次构造容器；所有权不清（谁 free？）
//         virtual void onPacket(AVPacket*)    编码器得被继承，Pusher 要为视频/音频
//                                             各写一个类，还不能用 lambda 现场拼
//         把队列指针直接传给编码器             编码器就绑死在"队列"这个具体类型上
//     回调 = 依赖倒置：编码器只依赖一个**签名**，不依赖具体接收者。
//     想改成写文件 / 发网络 / 丢进环形缓冲，都只改消费侧的 lambda。
//
//     对照 08-04：那里用的是"出参"
//         Encode(..., std::vector<AVPacket*>& packets)
//     因为主循环是单线程、有明确的"编一批 → 写一批"节奏。
//     本工程编码跑在独立线程上、要持续往共享队列里塞，没有"一批"的自然边界，
//     所以用回调更合适。两种写法都对，取决于**谁掌握节奏**。
//
//  ⑤ 所有权（最容易错的地方）：**借用，只在回调期间有效**
//         receive_packet(pkt) → sink(pkt) → 【你的代码】→ 返回
//         → av_packet_unref(pkt)（编码器收回引用）→ 下一轮复用同一块结构体
//     所以要在回调之外继续用这个包，必须 av_packet_clone：
//         AVPacket* copy = av_packet_clone(pkt);  // 只给码流加引用，不拷数据
//         copy->stream_index = ...;              // 顺手补上流下标（见知识点 12.1）
//         queue->push(copy);                     // 队列负责 av_packet_free
//     直接存原指针 → unref 之后 data 没了 / 被下一包覆盖 → 随机崩溃或写出垃圾数据。
//
//  ⑥ 线程：sink 在**编码所在线程**被调用
//         视频包 → encodeLoop 线程
//         音频包 → audioCaptureLoop 线程（音频是就地编码的）
//     lambda 里做的事必须线程安全；也别在里面做耗时操作 ——
//     包队列若是"阻塞"模式（落盘场景），队列满时 sink 会把编码线程
//     （音频则是**采集线程**）卡住。
// ---------------------------------------------------------------------------
// =============================================================================
#ifndef PUSH_ENCODER_H
#define PUSH_ENCODER_H

#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
}

struct EncoderConfig
{
    // ---- 编码器名字 ----
    // 用 by_name 而不是 by_id：H.264 有十几个实现，明确指定才可控。
    // 想换硬件编码就改成 "h264_nvenc" / "h264_qsv"（参数名会不同，见文档）。
    std::string videoCodec = "libx264";
    std::string audioCodec = "aac";

    // ---- 视频 ----
    // 宽高/帧率由 Pusher 用"采集源实际协商出来的值"回填，别直接用默认值
    int width  = 1280;
    int height = 720;
    int fps    = 30;

    int videoBitrate = 4000000;      // 4 Mbps
    int gopSize      = 0;            // 0 = 打开时按 fps 算（1 秒一个关键帧）
    int maxBFrames   = 0;            // 推流给 0：B 帧会让 dts<pts 且加延迟
    std::string preset = "veryfast"; // x264 档位：越快压缩率越低
    bool zerolatency = true;         // tune=zerolatency：关前瞻/多帧缓冲，直播必选
    bool cbr         = false;        // 是否把码率钉住（接近 CBR）

    // ---- 音频 ----
    // 这三个描述"喂进来的 PCM 是什么格式"，由声音源实际协商结果回填。
    // AAC 只吃 fltp，编码器内部会用 swresample 转，所以这里允许任意输入格式。
    int            audioInSampleRate = 48000;
    int            audioInChannels   = 2;
    AVSampleFormat audioInSampleFmt  = AV_SAMPLE_FMT_S16;
    int            audioBitrate      = 128000;

    /* 【知识点 11】输入帧的 pts 用哪个时间基
     * ---------------------------------------------------------------------------
     * 本工程约定：采集层产出的帧，pts 单位是**微秒**（{1, 1000000}）。
     * 而视频编码器的时间基是 {1, fps}（pts 用帧号表示）——
     * 两者不同，所以 encode() 里必须 av_rescale_q 换算一次。
     *
     * 为什么不在采集层就把 pts 换成帧号？
     *   因为音频的时间基是 {1, sample_rate}，两路本来就不一样。
     *   让"输入侧"统一用微秒，是让两条路能互相比较的前提（见 pusher.cpp 的交织逻辑）。
     * --------------------------------------------------------------------------- */
    AVRational inputTimeBase{ 1, 1000000 };
};

// 产出一包时回调。**指针只在回调期间有效**，要留必须 av_packet_clone（见【知识点 10】）
using PacketSink = std::function<void(AVPacket*)>;

class IVideoEncoder
{
public:
    virtual ~IVideoEncoder() = default;

    virtual bool open(const EncoderConfig& cfg) = 0;
    virtual void setSink(PacketSink sink) = 0;

    // 喂一帧 yuv420p；返回 false 表示这一帧没被接受（格式不符或出错）
    virtual bool encode(AVFrame* yuv420p) = 0;

    // 收尾：把编码器内部还压着的包全部吐出来（推流结束时必须调）
    virtual void flush() = 0;

    // 给 muxer 建流用的参数（avcodec_parameters_from_context 的结果）
    virtual AVCodecParameters* codecpar() = 0;

    /* 编码器的时间基。muxer 建流、写包时都要用它做时间戳换算，
     * 所以由编码器自己报出来，别让上层去猜（换硬件编码时它可能不一样）。 */
    virtual AVRational timeBase() const = 0;
};

class IAudioEncoder
{
public:
    virtual ~IAudioEncoder() = default;

    virtual bool open(const EncoderConfig& cfg) = 0;
    virtual void setSink(PacketSink sink) = 0;

    // 喂一帧 PCM（格式见 cfg.audioInSampleFmt）；内部会重采样/凑帧
    virtual bool encode(AVFrame* pcm) = 0;
    virtual void flush() = 0;

    virtual AVCodecParameters* codecpar() = 0;
    virtual AVRational timeBase() const = 0;
};

std::unique_ptr<IVideoEncoder> createVideoEncoder(const EncoderConfig& cfg);
std::unique_ptr<IAudioEncoder> createAudioEncoder(const EncoderConfig& cfg);

#endif // PUSH_ENCODER_H
