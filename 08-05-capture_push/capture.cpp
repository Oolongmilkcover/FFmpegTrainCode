#include "capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>   // dshow / v4l2 / alsa 这些"输入设备"都在这个库
#include <libavformat/avformat.h>   // avformat_open_input / av_read_frame
#include <libavutil/imgutils.h>     // av_image_* 系列
#include <libavutil/mathematics.h>  // av_rescale
#include <libavutil/opt.h>          // av_opt_set
#include <libavutil/time.h>         // av_gettime_relative（做实时节流用）
#include <libswscale/swscale.h>     // 任意像素格式 → yuv420p
}

namespace {

/* 自己定义 π：严格的 C++17（关闭 GNU 扩展）下 M_PI 不保证存在 */
constexpr double kPi = 3.14159265358979323846;

/*
 * 【知识点 33】合成源为什么必须自己"节流"
 * ---------------------------------------------------------------------------
 * 真实摄像头是**实时源**：它按 30fps 到点才给你一帧，你读得再快也得等。
 * 而合成源是一段代码，`while(1) { 生成一帧 }` 会在一秒钟里造出几千帧 ——
 * 这不是"性能好"，而是**语义错了**：
 *
 *   - 时间轴会离谱：30fps 下 3000 帧 = 100 秒的"视频"，而你只跑了 5 秒；
 *   - 队列/背压的意义全没了：编码器根本追不上，帧队列一路丢最旧；
 *   - 推流场景更荒谬：真实网络只有几 Mbps，你却想推几百 Mbps。
 *
 * 所以合成源必须模拟设备的节奏：算出"这一帧本该在什么时候出现"，
 * 然后睡到那个时刻。这也顺带让"丢帧/背压"的实验条件变得真实。
 *
 * 注意睡觉得分片，并检查 stop 标志 —— 否则 stop() 时线程会睡满一整帧才退出。
 * ---------------------------------------------------------------------------
 */
bool sleepUntil(int64_t dueUsSinceStart, int64_t startUs, const std::atomic<bool>& stop)
{
    for (;;) {
        const int64_t nowUs = av_gettime_relative() - startUs;
        const int64_t remain = dueUsSinceStart - nowUs;
        if (remain <= 0) return true;              // 到点了

        if (stop.load()) return false;             // 被叫停，立刻放弃

        // 分片睡：最多 5ms 醒一次，这样退出很灵敏，也不会占满 CPU
        std::this_thread::sleep_for(
            std::chrono::microseconds(remain > 5000 ? 5000 : remain));
    }
}

/*
 * 【知识点 35】节流的起点为什么必须"惰性"记录
 * ---------------------------------------------------------------------------
 * open() 只负责打开设备，真正开始出帧要等 Pusher 把编码器、输出都开好、
 * 把线程拉起来 —— 中间可能隔 1 秒以上（dshow 打开两个设备就要 1~2 秒）。
 *
 * 如果基准时刻记在 open() 里，那么"due = 帧号/fps"算出来的前 30 帧早就过期了，
 * 它们会被瞬间全部放出（1 秒的帧在同一毫秒内产生），
 * 结果就是：跑 4 秒，输出却有 5 秒的视频。
 *
 * 所以起点要记在**第一帧真正要出的时候**。这也让两路源的起点更接近，
 * 音视频对齐更准。
 * ---------------------------------------------------------------------------
 */
int64_t lazyStartUs(int64_t& startUs)
{
    if (startUs == 0) startUs = av_gettime_relative();
    return startUs;
}

/* FFmpeg 的错误码要自己转字符串。
 * 注意 C++ 里不能用 av_err2str()：它用了 C 的复合字面量，编译不过。 */
std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

/*
 * 【知识点 6】中断回调 —— 让阻塞的 I/O 能被叫停
 * ---------------------------------------------------------------------------
 * 设备上的 avformat_open_input / av_read_frame 会长时间阻塞（等设备出帧）。
 * 注册这个回调之后，FFmpeg 会在内部轮询它；返回非 0 就立刻放弃当前 I/O
 * （返回 AVERROR_EXIT），线程就能正常退出来了。
 *
 * opaque 指向 Pusher 里的一个 std::atomic<bool>，所以"叫停"这一动作
 * 从任何线程做都是安全的（只是 store 一个 bool）。
 * ---------------------------------------------------------------------------
 */
int interruptCb(void* opaque)
{
    auto* stop = static_cast<std::atomic<bool>*>(opaque);
    return (stop && stop->load()) ? 1 : 0;
}

/*
 * 【知识点 7】一个 AVFormatContext + 一个解码器 = 一路设备输入
 * ---------------------------------------------------------------------------
 * 视频和音频的采集流程其实一模一样：
 *
 *     打开设备 → 拿到一个 packet → 送给解码器 → 取出一帧 AVFrame
 *
 * 区别只在"打开时的选项"和"拿到帧之后要不要转像素格式"。
 * 所以把这段公共管子抽出来，视频/音频各自只留自己的那点差异 ——
 * 这也是参考工程里两个源类各自复制了一遍的东西，本工程合并掉了。
 *
 * 为什么设备还要"解码"？
 *   摄像头常见输出 MJPEG / H.264，麦克风输出 pcm_s16le —— 它们对 FFmpeg 来说
 *   都是"压缩或打包过的码流"，要变成 AVFrame 都必须过一遍解码器。
 *   pcm_s16le 的解码器只是把 packet 拆成 AVFrame，几乎零成本。
 * ---------------------------------------------------------------------------
 */
class DeviceStream
{
public:
    ~DeviceStream() { close(); }

    bool open(const std::string& inputFormat,
              const std::string& device,
              const std::vector<std::pair<std::string, std::string>>& options,
              AVMediaType wantType,
              std::atomic<bool>* stopFlag)
    {
        close();

        /*
         * 注册所有输入设备。FFmpeg 5.0 之后它会自动注册，但显式调用无害，
         * 而且能避免"换了构建后突然找不到 dshow"这类问题。
         */
        avdevice_register_all();

        m_fmt = avformat_alloc_context();
        if (!m_fmt) return false;

        // 先挂中断回调，再打开 —— 打开本身就可能阻塞
        m_fmt->interrupt_callback.callback = &interruptCb;
        m_fmt->interrupt_callback.opaque   = stopFlag;

        const AVInputFormat* ifmt = av_find_input_format(inputFormat.c_str());
        if (!ifmt) return false;                 // 这份 FFmpeg 没编进该后端（比如 Linux 上没 dshow）

        AVDictionary* opts = nullptr;
        for (const auto& kv : options) {
            av_dict_set(&opts, kv.first.c_str(), kv.second.c_str(), 0);
        }

        // device 是"设备名"，dshow 要求形如 "video=xxx" / "audio=xxx"
        int ret = avformat_open_input(&m_fmt, device.c_str(), ifmt, &opts);
        av_dict_free(&opts);                     // 打开之后就没用了，必须释放
        if (ret < 0) {
            close();
            return false;
        }

        ret = avformat_find_stream_info(m_fmt, nullptr);
        if (ret < 0) {
            close();
            return false;
        }

        // 找第一路目标类型的流（dshow 一个上下文通常只有一路）
        for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
            if (m_fmt->streams[i]->codecpar->codec_type == wantType) {
                m_streamIndex = static_cast<int>(i);
                break;
            }
        }
        if (m_streamIndex < 0) {
            close();
            return false;
        }

        AVCodecParameters* par = m_fmt->streams[m_streamIndex]->codecpar;

        // 打开软解码器；设备流基本都是未压缩/帧内编码，软解足够快
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        if (!dec) {
            close();
            return false;
        }

        m_dec = avcodec_alloc_context3(dec);
        if (!m_dec) { close(); return false; }

        ret = avcodec_parameters_to_context(m_dec, par);
        if (ret < 0) { close(); return false; }

        ret = avcodec_open2(m_dec, dec, nullptr);
        if (ret < 0) {
            close();
            return false;
        }

        return true;
    }

    void close()
    {
        if (m_dec) { avcodec_free_context(&m_dec); }
        if (m_fmt) { avformat_close_input(&m_fmt); }   // 输入上下文用 close_input（配对：open_input）
        m_streamIndex = -1;
    }

    /*
     * 取下一帧。返回新分配的 AVFrame（调用方 av_frame_free），失败/结束返回 nullptr。
     *
     * 为什么要套一层循环：一次 av_read_frame 可能
     *   - 读到的是别的流（这里只有一路，但仍要判断）
     *   - 送进解码器后还不够解出一帧（EAGAIN）
     * 所以要"读到能出帧为止"。
     */
    AVFrame* nextFrame()
    {
        if (!m_fmt || !m_dec) return nullptr;

        for (;;) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) return nullptr;

            int ret = avcodec_receive_frame(m_dec, frame);
            if (ret == 0) return frame;                  // 拿到一帧

            av_frame_free(&frame);
            if (ret == AVERROR_EOF) return nullptr;      // 真正结束
            if (ret != AVERROR(EAGAIN)) return nullptr;  // 其它错误

            // 解码器还需要输入，去读下一个 packet
            AVPacket* pkt = av_packet_alloc();
            if (!pkt) return nullptr;

            ret = av_read_frame(m_fmt, pkt);
            if (ret < 0) {                               // 含 AVERROR_EXIT（被中断）
                av_packet_free(&pkt);
                return nullptr;
            }

            if (pkt->stream_index == m_streamIndex) {
                // 送进解码器；返回 EAGAIN 说明解码器还没消化完，下一轮 receive 会拿到
                avcodec_send_packet(m_dec, pkt);
            }
            av_packet_free(&pkt);
        }
    }

    const AVCodecParameters* codecpar() const
    {
        return (m_fmt && m_streamIndex >= 0)
                   ? m_fmt->streams[m_streamIndex]->codecpar
                   : nullptr;
    }

private:
    AVFormatContext* m_fmt         = nullptr;
    AVCodecContext*  m_dec         = nullptr;
    int              m_streamIndex = -1;
};

/*
 * 【知识点 8】像素格式转换：任何格式 → yuv420p
 * ---------------------------------------------------------------------------
 * 摄像头可能给 YUYV / NV12 / MJPEG 解出来的 yuvj422p 等等，
 * 而 x264 编码器只认 yuv420p（或 nv12 等少数几个）。
 * swscale 就是干这个的：一次 sws_getContext，之后每帧 sws_scale。
 *
 * 注意 dstW/dstH 这里和源尺寸一样（只转格式、不缩放）。
 * 以后要做"设备 4K、推 1080p"，只改这两个参数即可，代码不用动。
 * ---------------------------------------------------------------------------
 */
class FrameConverter
{
public:
    ~FrameConverter() { reset(); }

    // 源格式/尺寸变了就重建上下文（设备中途改格式的情况确实存在）
    bool ensure(int srcW, int srcH, AVPixelFormat srcFmt, int dstW, int dstH)
    {
        if (m_sws && srcW == m_srcW && srcH == m_srcH && srcFmt == m_srcFmt
            && dstW == m_dstW && dstH == m_dstH) {
            return true;                       // 没变，复用
        }
        reset();

        m_sws = sws_getContext(srcW, srcH, srcFmt,
                               dstW, dstH, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) return false;

        m_srcW = srcW; m_srcH = srcH; m_srcFmt = srcFmt;
        m_dstW = dstW; m_dstH = dstH;
        return true;
    }

    void reset()
    {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    }

    // 转成新的 yuv420p 帧（带引用，调用方 av_frame_free）。pts 由调用方设置
    AVFrame* convert(const AVFrame* src)
    {
        if (!m_sws || !src) return nullptr;

        // 源本来就是同尺寸 yuv420p：直接加一次引用返回，连拷贝都省了
        if (src->format == AV_PIX_FMT_YUV420P && src->width == m_dstW && src->height == m_dstH) {
            AVFrame* same = av_frame_alloc();
            if (!same) return nullptr;
            if (av_frame_ref(same, src) < 0) { av_frame_free(&same); return nullptr; }
            return same;
        }

        AVFrame* dst = av_frame_alloc();
        if (!dst) return nullptr;
        dst->format = AV_PIX_FMT_YUV420P;
        dst->width  = m_dstW;
        dst->height = m_dstH;

        // 第二个参数是对齐字节数：给 SIMD 用 32 比较稳
        if (av_frame_get_buffer(dst, 32) < 0) {
            av_frame_free(&dst);
            return nullptr;
        }

        sws_scale(m_sws, src->data, src->linesize, 0, src->height,
                  dst->data, dst->linesize);
        return dst;
    }

private:
    SwsContext*  m_sws    = nullptr;
    int          m_srcW   = 0;
    int          m_srcH   = 0;
    AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
    int          m_dstW   = 0;
    int          m_dstH   = 0;
};

// ============================================================================================
// 视频源实现
// ============================================================================================

/* 真实设备（dshow / v4l2） */
class DeviceVideoSource : public IVideoSource
{
public:
    ~DeviceVideoSource() override { close(); }

    bool open(const VideoSourceConfig& cfg) override
    {
        m_stop = false;

        std::vector<std::pair<std::string, std::string>> opts;
        if (cfg.fps > 0)      opts.emplace_back("framerate", std::to_string(cfg.fps));
        if (cfg.width > 0 && cfg.height > 0) {
            opts.emplace_back("video_size",
                              std::to_string(cfg.width) + "x" + std::to_string(cfg.height));
        }

        if (!m_stream.open(cfg.inputFormat, cfg.device, opts, AVMEDIA_TYPE_VIDEO, &m_stop)) {
            return false;
        }

        // 用设备实际给的尺寸/帧率回填 —— 这是我们后面配编码器的依据
        const AVCodecParameters* par = m_stream.codecpar();
        if (!par) return false;
        m_width  = par->width;
        m_height = par->height;
        m_fps    = parseFps(par, cfg.fps);

        if (!m_conv.ensure(par->width, par->height,
                           static_cast<AVPixelFormat>(par->format),
                           par->width, par->height)) {
            return false;
        }

        m_startUs = 0;   // 惰性记录：等第一帧真要出的时候再定基准（见 lazyStartUs）
        return true;
    }

    void close() override
    {
        m_conv.reset();
        m_stream.close();
        m_width = m_height = 0;
    }

    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {
        AVFrame* raw = m_stream.nextFrame();
        if (!raw) return nullptr;

        // 设备可能中途改格式（少见但要防）
        if (!m_conv.ensure(raw->width, raw->height,
                           static_cast<AVPixelFormat>(raw->format),
                           raw->width, raw->height)) {
            av_frame_free(&raw);
            return nullptr;
        }

        AVFrame* yuv = m_conv.convert(raw);
        av_frame_free(&raw);
        if (!yuv) return nullptr;

        yuv->format = AV_PIX_FMT_YUV420P;
        yuv->width  = m_width;
        yuv->height = m_height;

        /*
         * 【知识点 34】帧率限制（frame rate limiter）—— 为什么放在"读完"之后
         * ---------------------------------------------------------------------------
         * 设备分两类：
         *   真摄像头：av_read_frame 本来就阻塞到下一帧（约 1/fps），读完就不需要再等；
         *   虚拟摄像头（OBS / AMD Privacy View 之类）：**不等帧率**，
         *       你读多快它给多快。实测 4 秒能拿到 1600+ 帧（≈370fps）。
         *
         * 如果不管它，用"帧号/fps"算出来的时间轴会跑飞：
         * 4 秒的运行会生成 50 多秒的"视频"。
         *
         * 所以这里按"这一帧本该出现的时刻"补一次等待：
         *   先读（真设备会自己等）→ 再补睡（虚拟设备才需要补）。
         * 这样两类设备的行为就统一了，输出时长 == 实际运行时长。
         *
         * 生产环境还可以做得更精细：用设备给的时间戳（pkt->pts + stream time_base）
         * 换算，并对"快于声明帧率"的帧做丢帧而不是等待。本工程取最直观的一种。
         * ---------------------------------------------------------------------------
         */
        const int frameHz = m_fps > 0 ? m_fps : 30;
        const int64_t due = m_frameIndex * kMicrosPerSecond / frameHz;
        const int64_t startUs = lazyStartUs(m_startUs);
        if (!sleepUntil(due, startUs, m_stop)) {
            av_frame_free(&yuv);
            return nullptr;
        }

        // 自己生成单调微秒时间戳（见 capture.h 知识点 5）
        yuv->pts      = due;
        yuv->duration = kMicrosPerSecond / frameHz;
        ++m_frameIndex;
        return yuv;
    }

    int width()  const override { return m_width; }
    int height() const override { return m_height; }
    int fps()    const override { return m_fps; }

private:
    /* 帧率优先取流里声明的 avg_frame_rate；没有就退回我们提议的值 */
    static int parseFps(const AVCodecParameters* par, int fallback)
    {
        if (par->framerate.num > 0 && par->framerate.den > 0) {
            const int v = static_cast<int>(av_rescale(par->framerate.num, 1, par->framerate.den));
            if (v > 0) return v;
        }
        return fallback > 0 ? fallback : 30;
    }

    DeviceStream   m_stream;
    FrameConverter m_conv;
    std::atomic<bool> m_stop{ false };
    int     m_width  = 0;
    int     m_height = 0;
    int     m_fps    = 30;
    int64_t m_frameIndex = 0;
    int64_t m_startUs    = 0;      // 帧率限制的基准
};

/*
 * 内置合成视频源 —— 画彩色渐变 + 移动竖条。
 *
 * 为什么需要它：没有摄像头 / 摄像头被别的程序占用 / CI 环境里，
 * 有它才能把"采集 → 编码 → 封装"整条链路跑通并验证。
 * 它直接往 yuv420p 帧里画，不经过任何 FFmpeg 输入设备，所以不依赖硬件。
 */
class SyntheticVideoSource : public IVideoSource
{
public:
    ~SyntheticVideoSource() override { close(); }

    bool open(const VideoSourceConfig& cfg) override
    {
        m_stop.store(false);
        m_width  = cfg.width  > 0 ? cfg.width  : 1280;
        m_height = cfg.height > 0 ? cfg.height : 720;
        m_fps    = cfg.fps    > 0 ? cfg.fps    : 30;
        m_frameIndex = 0;

        m_startUs = 0;   // 惰性记录（见 lazyStartUs）
        return true;
    }

    void close() override
    {
        m_width = m_height = 0;
    }

    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {
        if (m_width <= 0 || m_height <= 0) return nullptr;   // 还没 open

        /* 实时节流：睡到"这一帧本该出现的时刻"（见 sleepUntil 的说明）。
         * 这一步让合成源的行为和真实摄像头一致 —— 每次 readFrame 大约间隔 1/fps 秒。 */
        const int64_t due = m_frameIndex * kMicrosPerSecond / (m_fps > 0 ? m_fps : 30);
        const int64_t startUs = lazyStartUs(m_startUs);
        if (!sleepUntil(due, startUs, m_stop)) return nullptr;   // 被叫停

        /* 【知识点 39】为什么每帧新分配一个 AVFrame，而不是复用一个成员 buffer
         * ---------------------------------------------------------------------------
         * 因为返回的帧会**离开这个函数**：它被 push 进帧队列，可能过几十毫秒
         * 才被编码线程取走。如果复用同一块 buffer，下一次 readFrame 往里一画，
         * 队列里那个还没被编码的帧就跟着变了 —— 编码器拿到的是"半新半旧"的画面，
         * 表现为撕裂 / 乱帧。这类 bug 还很难查：它只在"编码比采集慢"时出现。
         *
         * 所以每帧 av_frame_alloc + av_frame_get_buffer，所有权交给调用方，
         * 由调用方 av_frame_free。代价是每帧一次 1.3MB（720p）的 alloc/free；
         * 真要优化就用**帧池**（预分配 N 个，用完归还），而**不是**复用同一个。
         * --------------------------------------------------------------------------- */
        AVFrame* out = av_frame_alloc();
        if (!out) return nullptr;
        out->format = AV_PIX_FMT_YUV420P;
        out->width  = m_width;
        out->height = m_height;
        if (av_frame_get_buffer(out, 32) < 0) { av_frame_free(&out); return nullptr; }

        draw(out, static_cast<int>(m_frameIndex));

        out->pts      = m_frameIndex * kMicrosPerSecond / m_fps;
        out->duration = kMicrosPerSecond / m_fps;
        ++m_frameIndex;
        return out;
    }

    int width()  const override { return m_width; }
    int height() const override { return m_height; }
    int fps()    const override { return m_fps; }

private:
    /*
     * 画一帧。注意访问像素**必须用 linesize 跨行**，不能用 width：
     * 对齐会让每行实际字节数大于 width，用 width 索引会出斜纹。
     * （这个坑在 08-02 的【知识点 16】里详细讲过）
     */
    static void draw(AVFrame* f, int index)
    {
        const int w = f->width, h = f->height;

        // Y 平面：横向渐变 + 随帧号滚动，画面会"流动"
        for (int y = 0; y < h; ++y) {
            uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
            for (int x = 0; x < w; ++x) {
                row[x] = static_cast<uint8_t>((x + y + index * 4) & 0xFF);
            }
        }

        // 移动竖条：让"帧是否在变"一眼可见
        const int barX = (index * 8) % (w > 0 ? w : 1);
        for (int y = 0; y < h; ++y) {
            uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
            for (int dx = 0; dx < 8 && barX + dx < w; ++dx) {
                row[barX + dx] = 235;
            }
        }

        // U/V 平面：中性色（128）偏向一点点彩色，避免全灰
        for (int p = 1; p <= 2; ++p) {
            const int cw = (w + 1) / 2;
            const int ch = (h + 1) / 2;
            for (int y = 0; y < ch; ++y) {
                uint8_t* row = f->data[p] + static_cast<ptrdiff_t>(y) * f->linesize[p];
                for (int x = 0; x < cw; ++x) {
                    row[x] = static_cast<uint8_t>(128 + (p == 1 ? 20 : -20) + (index & 7));
                }
            }
        }
    }

    std::atomic<bool> m_stop{ false };
    int     m_width  = 0;
    int     m_height = 0;
    int     m_fps    = 30;
    int64_t m_frameIndex = 0;
    int64_t m_startUs    = 0;      // 节流基准
};

// ============================================================================================
// 音频源实现
// ============================================================================================

/* 真实设备（dshow / alsa）。不做重采样，原样交出设备的 PCM */
class DeviceAudioSource : public IAudioSource
{
public:
    ~DeviceAudioSource() override { close(); }

    bool open(const AudioSourceConfig& cfg) override
    {
        m_stop.store(false);

        std::vector<std::pair<std::string, std::string>> opts;
        if (cfg.sampleRate > 0) opts.emplace_back("sample_rate", std::to_string(cfg.sampleRate));
        if (cfg.channels   > 0) opts.emplace_back("channels",    std::to_string(cfg.channels));

        /*
         * 【知识点 37】dshow 的 audio_buffer_size（毫秒）—— 一个很实际的旋钮
         * ---------------------------------------------------------------------------
         * 实测：dshow 音频默认按 **500ms 一块**（每块 24000 个采样）交付。后果有两个：
         *   - 音频时间轴粒度是 0.5 秒，所以"跑 10 秒"的文件音频可能长 10.5 秒
         *     （最后一个完整块结束在哪就截到哪）；
         *   - 端到端延迟至少 0.5 秒，直播里很致命。
         *
         * 调到 100ms 后粒度/延迟都降到 1/5，代价是读取更频繁、CPU 略升。
         * 实时推流一般设 50~200ms。
         * ---------------------------------------------------------------------------
         */
        opts.emplace_back("audio_buffer_size", "100");

        if (!m_stream.open(cfg.inputFormat, cfg.device, opts, AVMEDIA_TYPE_AUDIO, &m_stop)) {
            return false;
        }

        const AVCodecParameters* par = m_stream.codecpar();
        if (!par) return false;

        m_sampleRate = par->sample_rate > 0 ? par->sample_rate : cfg.sampleRate;
        m_channels   = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : cfg.channels;

        /*
         * 设备给的采样格式：从 codecpar->format 读（int → AVSampleFormat）
         * 常见 s16（dshow）或 s16/float（alsa）。
         * 编码器那边会用 swresample 把它转成 AAC 需要的 fltp。
         */
        m_sampleFmt = par->format == AV_SAMPLE_FMT_NONE
                          ? AV_SAMPLE_FMT_S16
                          : static_cast<AVSampleFormat>(par->format);
        return true;
    }

    void close() override
    {
        m_stream.close();
        m_sampleRate = 0;
        m_channels   = 0;
    }

    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {
        AVFrame* f = m_stream.nextFrame();
        if (!f) return nullptr;

        /*
         * 【知识点 36】音频为什么也要节流
         * ---------------------------------------------------------------------------
         * 一开始我以为"麦克风由驱动按采样率节流，不用管"。实测发现不对：
         * dshow 的音频设备在打开时就开始往自己的缓冲里灌数据，
         * 而 open() 到"真正开始读"之间隔着编码器/输出的初始化（约 1 秒）。
         * 于是第一次 readFrame 就把这 1 秒的存货全拿到了 ——
         * 结果：跑 4 秒，音频却有 5 秒。
         *
         * 所以音频也按"第 N 个采样点本该在什么时刻"来节流：
         * 真麦克风本来就阻塞到数据就绪，这里的等待是空操作；
         * 预先缓冲过的设备则被拉回实时节奏。
         * ---------------------------------------------------------------------------
         */
        const int rate = m_sampleRate > 0 ? m_sampleRate : 48000;
        const int64_t due = m_sampleCount * kMicrosPerSecond / rate;
        const int64_t startUs = lazyStartUs(m_startUs);
        if (!sleepUntil(due, startUs, m_stop)) {
            av_frame_free(&f);
            return nullptr;
        }

        f->pts      = due;
        f->duration = f->nb_samples * kMicrosPerSecond / rate;
        m_sampleCount += f->nb_samples;
        return f;
    }

    int            sampleRate() const override { return m_sampleRate; }
    int            channels()   const override { return m_channels; }
    AVSampleFormat sampleFormat() const override { return m_sampleFmt; }
    int            nominalSamplesPerFrame() const override { return 1024; }

private:
    DeviceStream      m_stream;
    std::atomic<bool> m_stop{ false };
    int            m_sampleRate = 0;
    int            m_channels   = 0;
    AVSampleFormat m_sampleFmt  = AV_SAMPLE_FMT_S16;
    int64_t        m_sampleCount = 0;
    int64_t        m_startUs     = 0;   // 惰性记录的节流基准
};

/*
 * 内置合成音频源 —— 正弦扫频。
 * 没有麦克风也能验证"音频 → AAC → 容器"这条支路。
 * 每帧固定 1024 个 s16 采样（正好是 AAC 的一帧）。
 */
class SyntheticAudioSource : public IAudioSource
{
public:
    ~SyntheticAudioSource() override { close(); }

    bool open(const AudioSourceConfig& cfg) override
    {
        m_stop.store(false);
        m_sampleRate = cfg.sampleRate > 0 ? cfg.sampleRate : 48000;
        m_channels   = cfg.channels   > 0 ? cfg.channels   : 2;
        m_phase      = 0.0;
        m_sampleCount = 0;
        m_startUs    = 0;   // 惰性记录（见 lazyStartUs）
        return true;
    }

    void close() override { m_sampleRate = 0; m_channels = 0; }
    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {
        if (m_sampleRate <= 0) return nullptr;

        /* 实时节流：同样让合成音频按真实采样率"到点才出"（见 sleepUntil） */
        const int64_t due = m_sampleCount * kMicrosPerSecond / m_sampleRate;
        const int64_t startUs = lazyStartUs(m_startUs);
        if (!sleepUntil(due, startUs, m_stop)) return nullptr;   // 被叫停

        AVFrame* f = av_frame_alloc();
        if (!f) return nullptr;
        f->format     = AV_SAMPLE_FMT_S16;
        f->sample_rate = m_sampleRate;
        f->nb_samples  = nominalSamplesPerFrame();
        av_channel_layout_default(&f->ch_layout, m_channels);   // 8.1：声道布局只有 ch_layout

        if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return nullptr; }

        // s16 交错（packed）：L R L R ...，所以按 声道数 逐点写
        auto* samples = reinterpret_cast<int16_t*>(f->data[0]);
        const double step = 2.0 * kPi * 440.0 / m_sampleRate;   // 440Hz
        for (int i = 0; i < f->nb_samples; ++i) {
            const auto v = static_cast<int16_t>(std::sin(m_phase) * 12000);
            for (int ch = 0; ch < m_channels; ++ch) {
                samples[i * m_channels + ch] = v;
            }
            m_phase += step;
            if (m_phase > 2.0 * kPi) m_phase -= 2.0 * kPi;      // 防止浮点越滚越大
        }

        f->pts      = m_sampleCount * kMicrosPerSecond / m_sampleRate;
        f->duration = f->nb_samples * kMicrosPerSecond / m_sampleRate;
        m_sampleCount += f->nb_samples;
        return f;
    }

    int            sampleRate() const override { return m_sampleRate; }
    int            channels()   const override { return m_channels; }
    AVSampleFormat sampleFormat() const override { return AV_SAMPLE_FMT_S16; }
    int            nominalSamplesPerFrame() const override { return 1024; }

private:
    std::atomic<bool> m_stop{ false };
    int     m_sampleRate = 0;
    int     m_channels   = 0;
    int64_t m_sampleCount = 0;
    double  m_phase = 0.0;
    int64_t m_startUs = 0;      // 节流基准
};

} // namespace

// ============================================================================================
// 工厂
// ============================================================================================

std::unique_ptr<IVideoSource> createVideoSource(const VideoSourceConfig& cfg)
{
    if (cfg.synthetic) {
        return std::make_unique<SyntheticVideoSource>();
    }
    return std::make_unique<DeviceVideoSource>();
}

std::unique_ptr<IAudioSource> createAudioSource(const AudioSourceConfig& cfg)
{
    if (cfg.synthetic) {
        return std::make_unique<SyntheticAudioSource>();
    }
    return std::make_unique<DeviceAudioSource>();
}

/*
 * 附：这一层为什么"只用 FFmpeg 类型 + 标准库类型"
 * ---------------------------------------------------------------------------
 * 接口里不出现任何平台相关类型（HANDLE / ALSA 句柄 / Qt 类型），
 * 平台差异全部压在 DeviceStream 里。
 * 好处：要加 macOS 的 avfoundation、或者加一块"读取本地 yuv 文件"的假源，
 *      都只是多一个工厂分支，上层 Pusher 完全不用改。
 * ---------------------------------------------------------------------------
 */
