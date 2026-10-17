#include "videoencoder.h"

#include <cinttypes>
#include <cstdio>
#include <string>

extern "C" {
#include <libavutil/error.h>    // av_strerror
#include <libavutil/imgutils.h> // av_image_fill_arrays
#include <libavutil/opt.h>      // av_opt_set
}

namespace {

/* 把负错误码翻译成可读字符串 */
std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

} // namespace

VideoEncoder::~VideoEncoder()
{
    DeInit();
}

// =============================================================================
//  【知识点 2】视频编码器的参数
// =============================================================================
int VideoEncoder::InitH264(int width, int height, int fps, int bit_rate)
{
    DeInit();               // 允许重复调用 Init：先清掉上一次的状态

    width_    = width;
    height_   = height;
    fps_      = fps;
    bit_rate_ = bit_rate;

    /*
     * 【知识点 2.1】找编码器：用 by_name 还是 by_id？
     *
     *   avcodec_find_encoder(AV_CODEC_ID_H264)
     *       按格式 ID 找 → 返回注册表里第一个匹配的实现。H.264 有十几个实现
     *       （libx264 / h264_nvenc / h264_qsv / h264_amf / h264_mf / libopenh264…），
     *       返回哪个**不确定**。
     *
     *   本工程沿用了原版的 avcodec_find_encoder(AV_CODEC_ID_H264)，
     *   因为不需要设 x264 私有选项。一旦要用 preset/tune，
     *   就必须换成 avcodec_find_encoder_by_name("libx264")，
     *   否则换到 nvenc 上"preset=medium"这种档位名根本不认。
     *
     * ⚠ FFmpeg 8.1 的返回值是 **const AVCodec***（4.x 是非 const）。
     */
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        std::printf("找不到 H.264 编码器（这份 FFmpeg 可能没编进 libx264）\n"
                    "可用 `ffmpeg -encoders | findstr 264` 确认\n");
        return -1;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::printf("avcodec_alloc_context3 失败\n");
        return -1;
    }

    /*
     * 【知识点 2.2】AV_CODEC_FLAG_GLOBAL_HEADER 必须在 avcodec_open2 之前设
     * -----------------------------------------------------------------
     * 含义：把 SPS/PPS 从码流里挪出去，放到 codec_ctx_->extradata，
     *       由封装层统一写一次（mp4/flv 都这么要求）。
     *
     * 不设的话，每个 I 帧前面都会重复带一遍 SPS/PPS ——
     * 那样输出的是「自包含裸流」，适合存 .h264，但 mp4 muxer 会拿不到 extradata。
     *
     * 严谨的写法是「看容器要不要」：
     *     if (oc->oformat->flags & AVFMT_GLOBALHEADER) 才设
     * 本工程固定封装成 mp4，所以直接设上。
     * -----------------------------------------------------------------
     */
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    codec_ctx_->bit_rate  = bit_rate_;
    codec_ctx_->width     = width_;
    codec_ctx_->height    = height_;
    codec_ctx_->framerate = AVRational{ fps_, 1 };

    /*
     * 【知识点 3】time_base —— 本工程最关键的一个设计决定
     * -----------------------------------------------------------------
     * 原版把视频编码器的 time_base 设成 {1, 1000000}（微秒），
     * 而不是常见的 {1, fps}。
     *
     * 为什么？因为音频的时间戳天然是「采样点」（1/44100 秒的粒度），
     * 视频是「帧」（1/25 秒的粒度）。两者要放在同一个 while 循环里
     * 比较「谁的时间更早」，就必须先统一到一条时间轴上。
     *
     * 本工程的选择：**统一用微秒做timebase**（time_base = 1/1000000 秒）。
     *   优点：精度足够（1 微秒），两种流可以直接比大小；
     *   代价：pts 的数值变得很大（40 秒 = 40,000,000）。
     *
     * 另一条常见路线是「各用各的自然时间基」（视频 1/fps、音频 1/sample_rate），
     * 比较时用 av_compare_ts 自动换算。两种都行，本工程选了前者，
     * 因为它让「累加 pts」的代码只有一种写法（见 main.cpp）。
     *
     * ⚠ 注意：这里设的是**编码器**的时间基。写到 mp4 时，
     *   流（AVStream）的时间基还会被 muxer 改写（通常是 1/90000 之类），
     *   所以送包前必须再换算一次（见 muxer.cpp 的 SendPacket）。
     * -----------------------------------------------------------------
     */
    codec_ctx_->time_base = AVRational{ 1, 1000000 };

    /*
     * 【知识点 2.3】GOP 与 B 帧
     *   gop_size     = fps → 每 1 秒一个关键帧
     *   max_b_frames = 0   → 不要 B 帧
     *
     * 为什么这里给 0？
     *   B 帧会让 dts < pts（解码顺序 ≠ 显示顺序），封装时必须按 dts 排序，
     *   而且增加编码缓冲延迟。音视频同步代码本身就够绕了，
     *   先把它关掉能让 dts == pts，排查时间戳问题时简单得多。
     *   等同步跑通了再打开 B 帧做优化。
     */
    codec_ctx_->gop_size     = fps_;
    codec_ctx_->max_b_frames = 0;

    /*
     * pix_fmt 必须和喂进来的帧格式一致，否则 avcodec_send_frame 直接报错。
     * 本工程约定输入就是 yuv420p（从 mp4 解出来的就是它）。
     */
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

    /*
     * 打开编码器。
     * 第二个参数传 codec（4.x 时代常写 NULL，表示「用 ctx->codec」，
     * 现在显式传更清楚）。第三个参数是私有选项字典，本工程不用。
     */
    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        std::printf("avcodec_open2 失败: %s\n", errStr(ret).c_str());
        DeInit();
        return -1;
    }

    /*
     * 复用同一个输入帧。
     * ⚠ 这里**故意不调** av_frame_get_buffer：
     *   因为数据来自调用方的 yuv_frame_buf，每帧用
     *   av_image_fill_arrays 把指针指过去就行（零拷贝，见知识点 4）。
     *   只有需要自己持有数据时才 get_buffer。
     */
    frame_ = av_frame_alloc();
    if (!frame_) {
        std::printf("av_frame_alloc 失败\n");
        DeInit();
        return -1;
    }
    frame_->width  = width_;
    frame_->height = height_;
    frame_->format = codec_ctx_->pix_fmt;

    std::printf("VideoEncoder 初始化成功: %dx%d @%dfps, %dkbps, time_base=%d/%d\n",
                width_, height_, fps_, bit_rate_ / 1024,
                codec_ctx_->time_base.num, codec_ctx_->time_base.den);
    return 0;
}

void VideoEncoder::DeInit()
{
    if (frame_) {
        av_frame_free(&frame_);         // 内部会 uninit ch_layout 等
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
}

// =============================================================================
//  【知识点 3 / 4 / 5】编码一帧
// =============================================================================
int VideoEncoder::Encode(const uint8_t* yuv_data, int yuv_size, int stream_index,
                         int64_t pts, int64_t time_base,
                         std::vector<AVPacket*>& packets)
{
    if (!codec_ctx_ || !frame_) {
        std::printf("VideoEncoder 未初始化\n");
        return -1;
    }

    /*
     * 【知识点 3】时间基换算
     * -----------------------------------------------------------------
     * 调用方给的 pts 单位是 time_base（本工程是 {1,1000000} 微秒），
     * 编码器要的是 codec_ctx_->time_base（本工程也是 {1,1000000}，
     * 所以这次换算是恒等变换 —— 但代码必须写，因为一旦两边不同
     * 就会出现「时长离谱」的文件，而且是很难查的那种错）。
     *
     * av_rescale_q(a, bq, cq) = a * bq / cq，全程用 64 位整数 + 防溢出。
     */
    const AVRational src_tb = AVRational{ 1, static_cast<int>(time_base) };
    frame_->pts = av_rescale_q(pts, src_tb, codec_ctx_->time_base);

    int ret = 0;
    if (yuv_data) {
        /*
         * 【知识点 4】零拷贝喂帧：av_image_fill_arrays
         * -----------------------------------------------------------------
         * 它**不拷贝数据**，只是按 yuv420p 的布局把 data[0..2] 指向
         * yuv_data 的各平面首地址，并算出 linesize。
         *
         *   平面 Y : yuv_data
         *   平面 U : yuv_data + w*h
         *   平面 V : yuv_data + w*h + w*h/4
         *
         * 前提：yuv_data 是「紧凑排布、无行padding」的裸 YUV，
         *       且 align 参数传 1（不做对齐补偿）。
         *
         * ⚠ 生命周期要求：yuv_data 必须活到 avcodec_send_frame() 返回之后。
         *   本工程的 yuv_frame_buf 是 main 里常驻的缓冲区，下一轮才覆写，
         *   而 send 是同步返回的，所以安全。
         *   libx264 内部会把数据拷进自己的帧缓冲，所以 send 之后就可以复用。
         *
         * ⚠ 与 08-03 那个坑的区别：
         *   08-03 里 frame 先调过 av_frame_get_buffer（拥有引用计数缓冲区），
         *   再被 av_image_fill_arrays 改写 data 指针 → 内部缓冲区泄漏 + 野指针。
         *   这里 frame 从没 get_buffer，data 本来就是空的，
         *   所以「整体接管外部 buffer」是正确用法。
         * -----------------------------------------------------------------
         */
        const int need_size = av_image_fill_arrays(
            frame_->data, frame_->linesize, yuv_data,
            static_cast<AVPixelFormat>(frame_->format),
            frame_->width, frame_->height, 1);
        if (need_size != yuv_size) {
            std::printf("yuv 尺寸不符: need=%d, 实际=%d\n", need_size, yuv_size);
            return -1;
        }
        ret = avcodec_send_frame(codec_ctx_, frame_);
    } else {
        /* yuv_data == nullptr → 冲刷。之后这批包吐完，编码器就 EOF 了 */
        ret = avcodec_send_frame(codec_ctx_, nullptr);
    }

    /* AVERROR_EOF：已经冲刷过了还继续冲刷，属于正常幂等，不算错 */
    if (ret < 0 && ret != AVERROR_EOF) {
        std::printf("avcodec_send_frame 失败: %s\n", errStr(ret).c_str());
        return -1;
    }

    /*
     * 【知识点 5】一次 send 可能产出 N 个包
     * -----------------------------------------------------------------
     * 所以必须循环 receive，直到：
     *   AVERROR(EAGAIN) —— 现在没包了，请继续送输入（正常）
     *   AVERROR_EOF     —— 已经彻底结束（正常）
     *
     * 和原版相比修掉的一个 bug：
     *   原版在 ret < 0 的分支里 av_packet_free(&packet)（此时 packet 变成 NULL），
     *   但没有 break，接着执行 packets.push_back(packet) → 把一个 NULL 塞进容器，
     *   后面 SendPacket(NULL) 立刻空指针崩溃。
     *   正确做法是「出错就 free + break」，绝不 push。
     * -----------------------------------------------------------------
     */
    int result = 0;
    while (true) {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            result = -1;
            break;
        }

        ret = avcodec_receive_packet(codec_ctx_, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&packet);
            result = 0;
            break;
        }
        if (ret < 0) {
            std::printf("avcodec_receive_packet 失败: %s\n", errStr(ret).c_str());
            av_packet_free(&packet);
            result = -1;
            break;
        }

        /*
         * 包属于哪一路流。编码器不会填这个字段，必须由我们填 ——
         * muxer 就是靠它找到对应的 AVStream 做时间基换算的。
         */
        packet->stream_index = stream_index;

        /*
         * 日志限流：只打印前 3 个包 + 每个关键帧。
         * 5 秒的片段会产生 125 个包，全打出来会把有用信息淹掉。
         */
        if (logged_ < 3 || (packet->flags & AV_PKT_FLAG_KEY)) {
            std::printf("  [video] pkt size=%d pts=%" PRId64 " dts=%" PRId64 " key=%d\n",
                        packet->size, packet->pts, packet->dts,
                        (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
        }
        ++logged_;

        packets.push_back(packet);   // 所有权交给调用方
    }
    return result;
}
