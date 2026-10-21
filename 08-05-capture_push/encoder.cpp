#include "encoder.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/audio_fifo.h>      // 音频凑帧：AAC 每帧必须正好 frame_size 个采样点
#include <libavutil/channel_layout.h>  // av_channel_layout_default
#include <libavutil/error.h>           // av_strerror
#include <libavutil/opt.h>             // av_opt_set：编码器私有选项
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>  // 设备给 s16 → AAC 要 fltp
}

namespace {

std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

/*
 * 【知识点 12】把编码器当前能产出的包全部取出来，交给 sink
 * ---------------------------------------------------------------------------
 * 视频和音频两条路完全一样（送完输入 → 循环收包），所以提出来共用：
 * 调用方只要说"哪个编码器、交给谁"。
 *
 * 两个返回码必须区分（这个地方新手几乎都会写错）：
 *   AVERROR(EAGAIN) = 还要更多输入才会出包 —— **不是错误**
 *   AVERROR_EOF     = 已经 flush 干净了      —— **也不是错误**
 *
 * 而 pkt 的结构体是复用的：sink 之后 av_packet_unref 交回引用，下一轮继续用。
 * 这也解释了【知识点 10】里"回调里的指针只在回调期间有效"。
 *
 * 【知识点 12.1】stream_index 不在这里设
 * ---------------------------------------------------------------------------
 * 编码器不知道自己会被放进第几路流 —— 那取决于"容器里建了几路流、按什么顺序"，
 * 是编排层（Pusher）的事。所以这里只交出包，由 sink 在 clone 之后写上正确的下标。
 * 参考工程在编码器里写死 video=0 / audio=1，只推视频+音频时没问题，
 * 但一旦"只采音频"就会错位（音频变成 0 号流，包却写着 1）。
 * ---------------------------------------------------------------------------
 */
bool drainToSink(AVCodecContext* cc, const PacketSink& sink)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    bool ok = true;
    while (true) {
        const int ret = avcodec_receive_packet(cc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;   // 正常结束
        if (ret < 0) { ok = false; break; }

        if (sink) sink(pkt);        // 借用：上层要留必须自己 clone
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return ok;
}

/*
 * ===========================================================================================
 *  x264 视频编码器
 * ===========================================================================================
 *  职责一句话：把采集来的 yuv420p 帧，编成 H.264 的 AVPacket。
 *
 *  四件关键事（下面代码里都标了 [重点]）：
 *    1) 参数：宽高 / pix_fmt / time_base / framerate / gop / bframes / bitrate
 *    2) 时间基换算：输入帧 pts 是微秒，编码器要 {1,fps}（帧号）
 *    3) 零拷贝喂帧：av_frame_ref 只复制引用，只改 pts，不动调用方的帧
 *    4) flush：send_frame(nullptr) 把内部压着的包全吐出来，否则丢尾部几帧
 */
class X264VideoEncoder : public IVideoEncoder
{
public:
    ~X264VideoEncoder() override { close(); }

    bool open(const EncoderConfig& cfg) override
    {
        close();
        m_cfg = cfg;

        // ① 找编码器：用 by_name，明确要 libx264（见 encoder.h 的说明）
        const AVCodec* codec = avcodec_find_encoder_by_name(cfg.videoCodec.c_str());
        if (!codec) return false;

        m_cc = avcodec_alloc_context3(codec);
        if (!m_cc) return false;

        // ② 基本参数
        m_cc->width  = cfg.width;
        m_cc->height = cfg.height;

        /* [重点] pix_fmt 必须和喂进来的帧格式一致。
         * 采集层已经统一转成 yuv420p，所以这里固定它。 */
        m_cc->pix_fmt = AV_PIX_FMT_YUV420P;

        /* [重点] 编码器时间基 = {1, fps}，即"pts 用帧号表示"。 */
        m_cc->time_base = AVRational{ 1, cfg.fps > 0 ? cfg.fps : 30 };

        /* 帧率会写进码流 VUI，播放器靠它推算显示时长，别漏 */
        m_cc->framerate = AVRational{ cfg.fps > 0 ? cfg.fps : 30, 1 };

        /* 关键帧间隔：0 表示没指定，按 1 秒算 */
        m_cc->gop_size     = cfg.gopSize > 0 ? cfg.gopSize : (cfg.fps > 0 ? cfg.fps : 30);
        m_cc->max_b_frames = cfg.maxBFrames;
        m_cc->bit_rate     = cfg.videoBitrate;

        /* ③ SPS/PPS 要作为 extradata 交给 muxer（mp4/flv 都要求）。
         * 不设这个标志，文件在某些播放器里会花屏或直接打不开。 */
        m_cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // ④ x264 私有选项（priv_data 就是 x264 的参数表）
        if (!cfg.preset.empty()) {
            av_opt_set(m_cc->priv_data, "preset", cfg.preset.c_str(), 0);
        }
        if (cfg.zerolatency) {
            // 关掉前瞻和多帧缓冲，换低延迟 —— 直播必选
            av_opt_set(m_cc->priv_data, "tune", "zerolatency", 0);
        }
        if (cfg.cbr) {
            /* "接近 CBR"：把码率上下限钉在目标值，并打开 HRD 占用率模型。
             * 严格 CBR 还要让 x264 填 padding 包，用 x264-params 指定即可。 */
            m_cc->rc_min_rate    = cfg.videoBitrate;
            m_cc->rc_max_rate    = cfg.videoBitrate;
            m_cc->rc_buffer_size = cfg.videoBitrate;          // 约 1 秒缓冲
            av_opt_set(m_cc->priv_data, "x264-params", "nal-hrd=cbr:force-cfr=1", 0);
        }

        // ⑤ 打开
        if (avcodec_open2(m_cc, codec, nullptr) < 0) {
            close();
            return false;
        }

        /* ⑥ 导出一份 AVCodecParameters 给 muxer 建流用
         *    （muxer 只需要"描述"，不需要这个编码器实例） */
        m_par = avcodec_parameters_alloc();
        if (!m_par) { close(); return false; }
        if (avcodec_parameters_from_context(m_par, m_cc) < 0) {
            close();
            return false;
        }

        return true;
    }

    void close()
    {
        if (m_par) avcodec_parameters_free(&m_par);
        if (m_cc)  avcodec_free_context(&m_cc);
    }

    void setSink(PacketSink sink) override { m_sink = std::move(sink); }

    bool encode(AVFrame* inYuv420p) override
    {
        if (!m_cc || !inYuv420p) return false;

        // 防御：格式/尺寸不一致会报错甚至崩，直接丢掉这一帧
        if (inYuv420p->format != m_cc->pix_fmt
            || inYuv420p->width  != m_cc->width
            || inYuv420p->height != m_cc->height) {
            return false;
        }

        /* [重点] 零拷贝换时间基：
         * av_frame_ref 只把"像素缓冲的引用"复制一份（不拷像素），
         * 于是在新帧上改 pts 不会污染调用方的帧。 */
        AVFrame* feed = av_frame_alloc();
        if (!feed) return false;
        if (av_frame_ref(feed, inYuv420p) < 0) { av_frame_free(&feed); return false; }

        /* [重点] 时间基换算：{1,1000000}(微秒) → {1,fps}(帧号)。
         * 不换算的后果：时长离谱、播放器 seek 乱跳。 */
        feed->pts = av_rescale_q(inYuv420p->pts, m_cfg.inputTimeBase, m_cc->time_base);
        feed->duration = inYuv420p->duration > 0
                             ? av_rescale_q(inYuv420p->duration, m_cfg.inputTimeBase, m_cc->time_base)
                             : 1;   // 通常是"一帧一格"

        const int ret = avcodec_send_frame(m_cc, feed);
        // send 之后就能还引用：编码器内部已把自己需要的数据引用住（依然零拷贝）
        av_frame_free(&feed);
        if (ret < 0) return false;

        // 送进去一帧，可能立刻能取出一包，也可能要等几帧才出包
        return drainToSink(m_cc, m_sink);
    }

    void flush() override
    {
        if (!m_cc) return;
        avcodec_send_frame(m_cc, nullptr);          // 冲刷信号
        drainToSink(m_cc, m_sink);                  // 把剩下的包全吐出来
    }

    AVCodecParameters* codecpar() override { return m_par; }
    AVRational timeBase() const override { return m_cc ? m_cc->time_base : AVRational{ 1, 25 }; }

private:
    EncoderConfig      m_cfg;
    PacketSink         m_sink;
    AVCodecContext*    m_cc  = nullptr;
    AVCodecParameters* m_par = nullptr;
};

/*
 * ===========================================================================================
 *  AAC 音频编码器
 * ===========================================================================================
 *  比视频编码器多两道工序，原因都来自 AAC 的硬约束：
 *
 *  1) **只吃 fltp**（float + 分平面），而设备给的是 s16 交错
 *     → 用 libswresample 转格式（顺带也能变采样率/变声道数）
 *
 *  2) **每帧必须正好 1024 个采样点**
 *     而设备一次给的采样数不固定（dshow 常见 1024/2048，alsa 更随意）
 *     → 用 AVAudioFifo 做缓冲，攒够 1024 才送一帧
 *
 *  这两步是所有"实时音频编码"都要做的，参考工程里也是这么处理的。
 */
class AacAudioEncoder : public IAudioEncoder
{
public:
    ~AacAudioEncoder() override { close(); }

    bool open(const EncoderConfig& cfg) override
    {
        close();
        m_cfg = cfg;

        const AVCodec* codec = avcodec_find_encoder_by_name(cfg.audioCodec.c_str());
        if (!codec) return false;

        m_cc = avcodec_alloc_context3(codec);
        if (!m_cc) return false;

        m_cc->bit_rate    = cfg.audioBitrate;
        m_cc->sample_rate = cfg.audioInSampleRate;

        /* 【知识点 13】sample_fmt 必须是 fltp
         * -----------------------------------------------------------------
         * native aac 编码器只接受 AV_SAMPLE_FMT_FLTP 这一个采样格式。
         * 严格做法是查编码器能力（avcodec_get_supported_config），
         * 这里直接写死并在注释里说明，是因为 AAC 的这个限制非常稳定。
         * ----------------------------------------------------------------- */
        m_cc->sample_fmt = AV_SAMPLE_FMT_FLTP;

        /* 【知识点 14】FFmpeg 8.1 的声道布局：只有 ch_layout 一个入口
         * -----------------------------------------------------------------
         * 4.x 时代是 ctx->channels + ctx->channel_layout = av_get_default_channel_layout(n)。
         * 8.1 把这两个字段和这个函数都删了，改成：
         *     av_channel_layout_default(&ctx->ch_layout, 声道数)
         * 它按声道数给标准布局（1→mono，2→stereo，6→5.1）。
         * ----------------------------------------------------------------- */
        av_channel_layout_default(&m_cc->ch_layout, cfg.audioInChannels > 0 ? cfg.audioInChannels : 2);

        // mp4 要把 AudioSpecificConfig 放进 extradata
        m_cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (avcodec_open2(m_cc, codec, nullptr) < 0) {
            close();
            return false;
        }

        m_par = avcodec_parameters_alloc();
        if (!m_par) { close(); return false; }
        if (avcodec_parameters_from_context(m_par, m_cc) < 0) {
            close();
            return false;
        }

        /* ---- 建重采样器：设备格式 → 编码器要求的格式 ---- */
        AVChannelLayout inLayout{};
        av_channel_layout_default(&inLayout, cfg.audioInChannels > 0 ? cfg.audioInChannels : 2);

        /* 【知识点 15】swr_alloc_set_opts2 —— 8.1 只有这个版本
         * 旧的 swr_alloc_set_opts() 已被删除。新签名把声道布局从 uint64 掩码
         * 换成了 AVChannelLayout 结构体，所以参数看起来"翻了一倍"。 */
        int ret = swr_alloc_set_opts2(&m_swr,
                                      &m_cc->ch_layout, m_cc->sample_fmt, m_cc->sample_rate,
                                      &inLayout, cfg.audioInSampleFmt, cfg.audioInSampleRate,
                                      0, nullptr);
        av_channel_layout_uninit(&inLayout);     // 栈上布局用完要释放

        if (ret < 0 || !m_swr) { close(); return false; }
        if (swr_init(m_swr) < 0) { close(); return false; }   // 初始化滤波器系数，别放进循环

        /* ---- FIFO：把不定长的输入攒成"正好 frame_size" ---- */
        m_fifo = av_audio_fifo_alloc(m_cc->sample_fmt, m_cc->ch_layout.nb_channels, 1);
        if (!m_fifo) { close(); return false; }

        /* ---- 复用的"输出帧"：容量 = frame_size ---- */
        m_frame = av_frame_alloc();
        if (!m_frame) { close(); return false; }
        m_frame->format      = m_cc->sample_fmt;
        m_frame->sample_rate = m_cc->sample_rate;
        m_frame->nb_samples  = m_cc->frame_size;
        av_channel_layout_copy(&m_frame->ch_layout, &m_cc->ch_layout);
        if (av_frame_get_buffer(m_frame, 0) < 0) { close(); return false; }

        m_nextPts = 0;
        return true;
    }

    void close()
    {
        if (m_frame) av_frame_free(&m_frame);
        if (m_fifo)  av_audio_fifo_free(m_fifo);
        if (m_swr)   swr_free(&m_swr);
        if (m_par)   avcodec_parameters_free(&m_par);
        if (m_cc)    avcodec_free_context(&m_cc);
    }

    void setSink(PacketSink sink) override { m_sink = std::move(sink); }

    bool encode(AVFrame* pcm) override
    {
        if (!m_cc || !m_swr || !m_fifo || !pcm) return false;

        // ---------------- 第一步：转格式，喂进 FIFO ----------------
        /*
         * swr_get_out_samples：这次最多可能产出多少采样点。
         * 采样率不同时输入输出不是 1:1，所以不能拿输入数当输出数。
         */
        const int outMax = swr_get_out_samples(m_swr, pcm->nb_samples);
        if (outMax <= 0) return false;

        /* 临时帧：只当"转运缓冲"用。容量不够就重建（设备采样数会变） */
        if (!m_tmp || m_tmp->nb_samples < outMax) {
            if (m_tmp) av_frame_free(&m_tmp);
            m_tmp = av_frame_alloc();
            if (!m_tmp) return false;
            m_tmp->format      = m_cc->sample_fmt;
            m_tmp->sample_rate = m_cc->sample_rate;
            m_tmp->nb_samples  = outMax;
            av_channel_layout_copy(&m_tmp->ch_layout, &m_cc->ch_layout);
            if (av_frame_get_buffer(m_tmp, 0) < 0) { av_frame_free(&m_tmp); return false; }
        }

        /* swr_convert 的第 4 个参数是 const uint8_t**。
         * C++ 里不能 const_cast<uint8_t*>(→const uint8_t**)，用数组接一层最干净。 */
        const uint8_t* inPlanes[AV_NUM_DATA_POINTERS] = { nullptr };
        for (int i = 0; i < AV_NUM_DATA_POINTERS && i < m_cc->ch_layout.nb_channels; ++i) {
            inPlanes[i] = pcm->data[i];
        }
        // 输入是 packed 格式时只有一个平面，直接把 data[0] 放进去
        if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(pcm->format)) == 0) {
            inPlanes[0] = pcm->data[0];
        }

        const int got = swr_convert(m_swr, m_tmp->data, outMax, inPlanes, pcm->nb_samples);
        if (got < 0) return false;

        if (got > 0) {
            // 把转好的采样点全部塞进 FIFO（它自己会扩容）
            if (av_audio_fifo_write(m_fifo, reinterpret_cast<void**>(m_tmp->data), got) < got) {
                return false;
            }
        }

        // ---------------- 第二步：攒够一帧就送编码器 ----------------
        bool ok = true;
        while (av_audio_fifo_size(m_fifo) >= m_cc->frame_size) {
            if (av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(m_frame->data),
                                   m_cc->frame_size) < m_cc->frame_size) {
                ok = false;
                break;
            }

            /* 【知识点 16】音频 pts 用"累计采样点数"推
             * -----------------------------------------------------------------
             * 编码器时间基是 {1, sample_rate}，所以 pts 就是采样点序号。
             * 这里用 m_nextPts 累加（而不是从输入帧的 pts 换算），
             * 因为 FIFO 会重排采样点，输入帧的边界已经没有意义了。
             * ----------------------------------------------------------------- */
            m_frame->pts = m_nextPts;
            m_nextPts   += m_cc->frame_size;

            if (avcodec_send_frame(m_cc, m_frame) < 0) { ok = false; break; }
            if (!drainToSink(m_cc, m_sink)) { ok = false; break; }
        }
        return ok;
    }

    void flush() override
    {
        if (!m_cc) return;
        avcodec_send_frame(m_cc, nullptr);
        drainToSink(m_cc, m_sink);
    }

    AVCodecParameters* codecpar() override { return m_par; }
    AVRational timeBase() const override { return m_cc ? m_cc->time_base : AVRational{ 1, 44100 }; }

private:
    EncoderConfig      m_cfg;
    PacketSink         m_sink;
    AVCodecContext*    m_cc  = nullptr;
    AVCodecParameters* m_par = nullptr;
    SwrContext*        m_swr = nullptr;
    AVAudioFifo*       m_fifo = nullptr;
    AVFrame*           m_frame = nullptr;   // 复用的"整帧"输出（frame_size 个采样点）
    AVFrame*           m_tmp   = nullptr;   // 复用的中转帧（swr 输出用）
    int64_t            m_nextPts = 0;
};

} // namespace

std::unique_ptr<IVideoEncoder> createVideoEncoder(const EncoderConfig& cfg)
{
    // 以后加硬件编码（NVENC / QSV / VideoToolbox），在这里按 cfg.videoCodec 分支即可，
    // Pusher 完全不用改 —— 这就是接口存在的意义。
    (void)cfg;
    return std::make_unique<X264VideoEncoder>();
}

std::unique_ptr<IAudioEncoder> createAudioEncoder(const EncoderConfig& cfg)
{
    (void)cfg;
    return std::make_unique<AacAudioEncoder>();
}
