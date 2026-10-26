// =============================================================================
//  muxer.h —— 封装/输出层（mini-OBS 的 "output" 层）
//
//  【知识点 17】同一个实现，既能落盘也能推流
//  ---------------------------------------------------------------------------
//  落盘和推流在 FFmpeg 眼里几乎是同一件事：都是"按容器格式把包写出去"，
//  区别只在最后那个 URL 是本地路径还是 rtmp://，以及两件必须补上的事：
//
//      rw_timeout        网络写可能永久阻塞，必须设超时（单位微秒）
//      AVIOInterruptCB   关程序时要能立刻打断阻塞中的写，而不是干等超时
//
//  所以本工程只有一个 FormatMuxer 实现，靠 OutputConfig 区分两种用法。
// ---------------------------------------------------------------------------
//
//  【知识点 18】为什么把"叫停标志"从外面传进来，而不是自己维护
//  ---------------------------------------------------------------------------
//  推流线程可能正卡在网络写上，此时别的线程要能让它出来。做法是给
//  AVFormatContext 挂一个 interrupt_callback，它定期读一个 std::atomic<bool>，
//  返回 true 就放弃当前 I/O。
//
//  这个 flag 由 Pusher 拥有（它才知道"用户要求停止了"），muxer 只借用指针。
//  这样做还有个额外好处：muxer 对象只被推流线程碰，不需要任何锁。
//
//  ⚠ 但有个陷阱：**写 trailer 之前必须把这个标志清掉**。
//    因为 abort 的目的是打断"卡住的数据写"，而 trailer 是收尾动作
//    （mp4 的 moov 就在里面），必须让它认真写完。
//    这是参考工程踩过的坑，见 muxer.cpp 里 writeTrailer() 的实现。
// =============================================================================
#ifndef PUSH_MUXER_H
#define PUSH_MUXER_H

#include <atomic>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

struct OutputConfig
{
    // "out.mp4" 或 "rtmp://127.0.0.1/live/demo"
    std::string url;

    // 容器格式："flv" / "mp4" / ...。留空则按 url 猜：
    //   扩展名 .mp4 → mp4；协议 rtmp:// → flv
    std::string format;

    // 网络读写超时（毫秒）。落盘时无所谓，推流时是保命参数
    int rwTimeoutMs = 5000;
};

class IMuxer
{
public:
    virtual ~IMuxer() = default;

    /* abortFlag 可为空（落盘场景就不需要）。非空时 muxer 只读它、
     * 并在 writeTrailer 前把它清掉（见【知识点 18】）。 */
    virtual bool open(const OutputConfig& cfg, std::atomic<bool>* abortFlag) = 0;

    /* 建一路流。timeBase 是"送进来的包用的时间基"（= 编码器的 time_base），
     * muxer 必须记住它，才能在 writePacket 里把时间戳换算到输出流自己的时间基
     * （FLV 是毫秒；mp4 常被改写成 1/90000 或 1/12800 —— 所以必须"现查现换算"）。 */
    virtual AVStream* addStream(AVCodecParameters* par, AVRational timeBase) = 0;

    virtual bool writeHeader() = 0;

    /* 写一包（内部按 dts 交织）。
     * **接管所有权**：内部 av_interleaved_write_frame 之后会 av_packet_free。
     * 可能阻塞（网络）→ 只在推流线程调用。 */
    virtual bool writePacket(AVPacket* pkt) = 0;

    virtual void writeTrailer() = 0;

    virtual void close() = 0;
};

std::unique_ptr<IMuxer> createMuxer();

#endif // PUSH_MUXER_H
