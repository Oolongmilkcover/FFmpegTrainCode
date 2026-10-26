#include "muxer.h"

#include <string>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

namespace {

std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

/* AVIO 的中断回调：让"卡在网络上"的写操作能被叫停 */
int interruptCb(void* opaque)
{
    auto* abortFlag = static_cast<std::atomic<bool>*>(opaque);
    return (abortFlag && abortFlag->load()) ? 1 : 0;
}

/*
 * 【知识点 38】推流时为什么必须**显式**指定容器格式
 * ---------------------------------------------------------------------------
 * 直觉上 avformat_alloc_output_context2 能自己猜格式：给 "out.mp4" 猜出 mp4。
 * 但它猜的依据是**文件扩展名**，而 "rtmp://127.0.0.1/live/demo" 没有扩展名，
 * 于是返回 AVERROR(EINVAL)（实测 -22），整个推流根本起不来。
 *
 * 实测：
 *     av_guess_format(NULL, "out.mp4",                    NULL) → mp4   ✓
 *     av_guess_format(NULL, "rtmp://127.0.0.1/live/demo", NULL) → NULL  ✗
 *
 * 所以按协议头自己映射：
 *     rtmp:// / rtsp:// / srt://  →  flv（直播流的事实标准）
 *     其它（带扩展名的本地文件）  →  交给 FFmpeg 按扩展名猜
 *
 * 参考工程把这个映射写在 main.cpp 的 guessFormat() 里；
 * 本工程把它收进 muxer —— "用什么容器"本来就是输出层该决定的事。
 * ---------------------------------------------------------------------------
 */
std::string effectiveFormat(const OutputConfig& cfg)
{
    if (!cfg.format.empty()) return cfg.format;          // 命令行显式指定，优先

    if (cfg.url.rfind("rtmp", 0) == 0
        || cfg.url.rfind("rtsp", 0) == 0
        || cfg.url.rfind("srt",  0) == 0) {
        return "flv";
    }
    return {};    // 空字符串 = 让 FFmpeg 按扩展名猜（.mp4 / .flv / .ts ...）
}

class FormatMuxer : public IMuxer
{
public:
    ~FormatMuxer() override { close(); }

    bool open(const OutputConfig& cfg, std::atomic<bool>* abortFlag) override
    {
        close();
        m_abortFlag = abortFlag;

        /* ① 建输出上下文。格式不能只靠扩展名猜，见【知识点 38】 */
        const std::string format = effectiveFormat(cfg);
        int ret = avformat_alloc_output_context2(
            &m_out, nullptr,
            format.empty() ? nullptr : format.c_str(),
            cfg.url.c_str());
        if (ret < 0 || !m_out) { close(); return false; }

        /* ② 中断回调（网络输出时必须有；落盘时挂着也无害） */
        m_out->interrupt_callback.callback = &interruptCb;
        m_out->interrupt_callback.opaque   = m_abortFlag;

        /* ③ 打开输出 IO。像 "null" 这类格式是 AVFMT_NOFILE，不需要 IO */
        if (!(m_out->oformat->flags & AVFMT_NOFILE)) {
            AVDictionary* opts = nullptr;
            /* rw_timeout 单位是**微秒**：网络卡住超过这个时间就让 avio 报错返回，
             * 而不是无限期挂着（配合中断回调，关程序时也能立刻退出） */
            av_dict_set(&opts, "rw_timeout",
                        std::to_string(static_cast<long long>(cfg.rwTimeoutMs) * 1000).c_str(), 0);

            ret = avio_open2(&m_out->pb, cfg.url.c_str(), AVIO_FLAG_WRITE,
                             &m_out->interrupt_callback, &opts);
            av_dict_free(&opts);
            if (ret < 0) { close(); return false; }
        }

        return true;
    }

    AVStream* addStream(AVCodecParameters* par, AVRational timeBase) override
    {
        if (!m_out || !par) return nullptr;

        AVStream* st = avformat_new_stream(m_out, nullptr);
        if (!st) return nullptr;

        /* 把编码器参数**拷贝**进流（含 extradata：SPS/PPS、AudioSpecificConfig）。
         * 漏了这步，容器头里没有解码信息，播放器会打不开或花屏。 */
        if (avcodec_parameters_copy(st->codecpar, par) < 0) return nullptr;

        // 让 muxer 自己选 codec_tag（mp4 里 h264 应该是 avc1）
        st->codecpar->codec_tag = 0;

        /* 记住"进来的包用哪个时间基"。写包时要拿它和 st->time_base 做换算。
         * st->time_base 这里只是**建议**，writeHeader 之后 muxer 会改写它。 */
        st->time_base = timeBase;
        m_inTimeBases.push_back(timeBase);

        return st;
    }

    bool writeHeader() override
    {
        if (!m_out) return false;

        if (avformat_write_header(m_out, nullptr) < 0) return false;

        /* 【知识点 19】写头之后 st->time_base 才是最终值
         * -----------------------------------------------------------------
         * flv 会把每路流都改成 {1,1000}（毫秒）；
         * mp4 常常改成 {1,90000} 或 {1,12800}（为了整除帧率）。
         * 所以写包时绝不能缓存写头前的时间基，必须现查。
         * ----------------------------------------------------------------- */
        m_wroteHeader = true;
        return true;
    }

    bool writePacket(AVPacket* pkt) override
    {
        if (!m_out || !pkt) return false;

        if (pkt->stream_index < 0
            || static_cast<size_t>(pkt->stream_index) >= m_inTimeBases.size()
            || pkt->stream_index >= static_cast<int>(m_out->nb_streams)) {
            av_packet_free(&pkt);
            return false;
        }

        const AVRational srcTb = m_inTimeBases[pkt->stream_index];
        const AVRational dstTb = m_out->streams[pkt->stream_index]->time_base;

        /* 【知识点 20】时间基换算：编码器时间基 → 输出流时间基
         * -----------------------------------------------------------------
         * pts/dts/duration 三个都要换，不能只换 pts：
         * 无 B 帧时 dts == pts，但一旦开 B 帧就不等。
         * 一步到位用 av_packet_rescale_ts，比自己算三个 av_rescale_q 清楚。
         * ----------------------------------------------------------------- */
        av_packet_rescale_ts(pkt, srcTb, dstTb);

        /* 位置信息对输出没意义，置 -1 避免 muxer 去做无谓的 seek */
        pkt->pos = -1;

        /* 【知识点 21】av_interleaved_write_frame 的两个行为
         * -----------------------------------------------------------------
         * ① 它按 **dts** 排序并内部缓冲，保证写进文件的包时间递增
         *    —— 这就是"交织"的核心。所以"调用顺序"和"文件里的顺序"不完全一样。
         * ② 成功后它会把 pkt **清零**（data=NULL,size=0,pts=NOPTS），
         *    因为引用被 move 进 muxer 缓冲了。所以想打印原始值必须提前存下来。
         * ----------------------------------------------------------------- */
        const int ret = av_interleaved_write_frame(m_out, pkt);

        /* 结构体本身仍归我们释放（内容已被 muxer 取走，这里不是 double free） */
        av_packet_free(&pkt);

        if (ret < 0) {
            /* 一般是网络坏了。这里只把失败告诉调用方，由它决定重连 ——
             * muxer 不该自己判断"是不是该重连"（那是编排层的策略）。 */
            return false;
        }
        return true;
    }

    void writeTrailer() override
    {
        if (!m_out || !m_wroteHeader || m_wroteTrailer) return;

        /* 【知识点 18 的坑】写 trailer 之前必须把"叫停标志"清掉
         * -----------------------------------------------------------------
         * abort 的目的是打断"卡住的数据写"。但 trailer 是收尾动作，
         * mp4 的 moov（每帧位置/大小的索引表）就在这一步写出去。
         * 如果标志还立着，中断回调会让 av_write_trailer 返回 AVERROR_EXIT，
         * 结果就是：程序正常退出，文件却因为缺 moov 而打不开。
         * ----------------------------------------------------------------- */
        if (m_abortFlag) m_abortFlag->store(false);

        av_write_trailer(m_out);
        m_wroteTrailer = true;
    }

    void close() override
    {
        if (m_out) {
            // 先关 IO（会 flush 缓冲），再释放上下文。
            // 注意：输出上下文用 avformat_free_context，**不是** avformat_close_input
            //（后者是解封装用的，拿它关输出上下文属于未定义行为）
            if (m_out->pb && !(m_out->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&m_out->pb);
            }
            avformat_free_context(m_out);
            m_out = nullptr;
        }
        m_inTimeBases.clear();
        m_wroteHeader  = false;
        m_wroteTrailer = false;
        m_abortFlag    = nullptr;
    }

private:
    AVFormatContext*  m_out = nullptr;
    std::vector<AVRational> m_inTimeBases;   // 每路流"输入侧"的时间基
    std::atomic<bool>* m_abortFlag = nullptr; // 借用，不拥有
    bool m_wroteHeader  = false;
    bool m_wroteTrailer = false;
};

} // namespace

std::unique_ptr<IMuxer> createMuxer()
{
    // 以后加 SRT / HTTP-FLV / RTSP 输出，也都在这里按 format 分支
    return std::make_unique<FormatMuxer>();
}

/*
 * 附：编码器 time_base 和 stream time_base 的关系（一个常见困惑）
 * ---------------------------------------------------------------------------
 * 同一个"第 10 帧"，在不同人眼里的数值完全不同：
 *
 *   编码器 time_base = {1,25}     → pts = 10
 *   mp4 流 time_base  = {1,12800} → pts = 10 * 12800/25 = 5120
 *   flv 流 time_base  = {1,1000}  → pts = 10 * 1000/25  = 400（毫秒）
 *
 * 它们表示的是**同一时刻**，只是刻度不同。av_packet_rescale_ts 干的就是换刻度。
 * 不换会怎样：容器拿 10 当 1/1000 秒解释 → 整段视频只播 10 毫秒。
 * ---------------------------------------------------------------------------
 */
