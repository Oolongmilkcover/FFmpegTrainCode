#include "audioencoder.h"

#include <cinttypes>
#include <cstdio>
#include <string>

extern "C" {
#include <libavutil/channel_layout.h>   // av_channel_layout_default
#include <libavutil/error.h>            // av_strerror
}

namespace {

std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

} // namespace

AudioEncoder::~AudioEncoder()
{
    DeInit();
}

// =============================================================================
//  【知识点 6 / 7】初始化 AAC 编码器
// =============================================================================
int AudioEncoder::InitAAC(int channels, int sample_rate, int bit_rate)
{
    DeInit();

    channels_    = channels;
    sample_rate_ = sample_rate;
    bit_rate_    = bit_rate;

    /* 返回值在 FFmpeg 8.1 是 const AVCodec* */
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        std::printf("找不到 AAC 编码器\n"
                    "可用 `ffmpeg -encoders | findstr aac` 确认\n");
        return -1;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::printf("avcodec_alloc_context3 失败\n");
        return -1;
    }

    /* 同上一个文件：mp4 需要把 AudioSpecificConfig 放进 extradata */
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    codec_ctx_->bit_rate    = bit_rate_;
    codec_ctx_->sample_rate = sample_rate_;

    /*
     * 【知识点 6】为什么 sample_fmt 必须是 fltp
     * -----------------------------------------------------------------
     * native aac 编码器**只接受一个**采样格式：AV_SAMPLE_FMT_FLTP
     *   float 类型 + planar（分平面）布局。
     *
     * 而 mp4 里解出来的 PCM 通常是 s16 交错（packed），
     * 所以中间必须有一道「s16 packed → fltp planar」的转换 ——
     * 这就是 AudioResampler 存在的唯一原因。
     *
     * 想确认某个编码器支持哪些格式：
     *   avcodec_get_supported_config(nullptr, codec,
     *                                AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfgs, &n)
     * 或者命令行 `ffmpeg -h encoder=aac`。
     * -----------------------------------------------------------------
     */
    codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;

    /*
     * 【知识点 7】声道布局：FFmpeg 8.1 的写法 ★原版的直接编译错误★
     * -----------------------------------------------------------------
     * 4.2.1 时代（原版代码）：
     *     codec_ctx_->channels       = channels_;
     *     codec_ctx_->channel_layout = av_get_default_channel_layout(channels_);
     *
     * FFmpeg 8.1：
     *   - codec_ctx_->channels / channel_layout 两个字段**都已删除**
     *   - av_get_default_channel_layout() 函数**也已删除**
     *   - 一切走 ch_layout（AVChannelLayout 结构体）
     *
     * 正确写法就是下面这一行：按声道数取该声道数的标准布局
     *     1 → mono   2 → stereo   6 → 5.1
     *
     * 为什么不能只记「声道数」？因为布局还规定了声道的顺序和含义：
     * 2 声道的 stereo 是「左/右」，2 声道的 downmix 是「立体声左/立体声右」，
     * 二者不同。给错了编码器的声道耦合决策会错。
     *
     * 三条铁律（跨三个工程都一样）：
     *   ① 比较用 av_channel_layout_compare()，不能用 == 也不能 memcmp
     *   ② 复制用 av_channel_layout_copy()，不能按值赋值（CUSTOM 布局会 double free）
     *   ③ 栈上声明的布局，用过 copy/from_* 之后要 av_channel_layout_uninit()
     *      而嵌在 AVCodecContext / AVFrame 里的由对应 *_free 负责
     * -----------------------------------------------------------------
     */
    av_channel_layout_default(&codec_ctx_->ch_layout, channels_);

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        std::printf("avcodec_open2 失败: %s\n", errStr(ret).c_str());
        DeInit();
        return -1;
    }

    /*
     * open 之后 frame_size 才有意义（AAC 恒为 1024）。
     * 另外 avcodec_open2 会回填 codec_ctx_->time_base = {1, sample_rate}，
     * 所以后面 Encode 里的换算是「微秒 → 1/44100」。
     */
    std::printf("AudioEncoder 初始化成功: %dHz %dch %dkbps, "
                "frame_size=%d, 编码器 time_base=%d/%d\n",
                sample_rate_, codec_ctx_->ch_layout.nb_channels, bit_rate_ / 1024,
                codec_ctx_->frame_size,
                codec_ctx_->time_base.num, codec_ctx_->time_base.den);
    return 0;
}

void AudioEncoder::DeInit()
{
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);   // 会把内部 ch_layout 一起 uninit
    }
}

int AudioEncoder::Encode(AVFrame* frame, int stream_index,
                         int64_t pts, int64_t time_base,
                         std::vector<AVPacket*>& packets)
{
    if (!codec_ctx_) {
        std::printf("AudioEncoder 未初始化\n");
        return -1;
    }

    /*
     * 时间基换算：微秒 → 编码器时间基（{1,44100}）。
     * 注意音频的 time_base 是 avcodec_open2 回填的，不是我们设的 ——
     * 这也是为什么时间基换算必须「现查 codec_ctx_->time_base」而不是硬编码。
     */
    const AVRational src_tb = AVRational{ 1, static_cast<int>(time_base) };
    if (frame) {
        frame->pts = av_rescale_q(pts, src_tb, codec_ctx_->time_base);
    }

    int ret = avcodec_send_frame(codec_ctx_, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        std::printf("avcodec_send_frame 失败: %s\n", errStr(ret).c_str());
        return -1;
    }

    /* 同 videoencoder：一次 send 可能出多个包，循环收；出错绝不 push */
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

        packet->stream_index = stream_index;

        /* 日志限流：只打印前 3 个包 */
        if (logged_ < 3) {
            std::printf("  [audio] pkt size=%d pts=%" PRId64 " dts=%" PRId64 "\n",
                        packet->size, packet->pts, packet->dts);
        }
        ++logged_;

        packets.push_back(packet);
    }
    return result;
}

int AudioEncoder::GetFrameSize()
{
    return codec_ctx_ ? codec_ctx_->frame_size : 0;
}

int AudioEncoder::GetSampleFormat()
{
    return codec_ctx_ ? codec_ctx_->sample_fmt : AV_SAMPLE_FMT_NONE;
}

/* 声道数：8.1 里唯一来源是 ch_layout.nb_channels */
int AudioEncoder::GetChannels()
{
    return codec_ctx_ ? codec_ctx_->ch_layout.nb_channels : -1;
}

int AudioEncoder::GetSampleRate()
{
    return codec_ctx_ ? codec_ctx_->sample_rate : -1;
}
