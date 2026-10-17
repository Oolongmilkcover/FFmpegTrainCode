#include "audioresampler.h"

#include <cstdio>
#include <string>

extern "C" {
#include <libavutil/channel_layout.h>   // av_channel_layout_default / uninit
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

AudioResampler::~AudioResampler()
{
    DeInit();
}

// =============================================================================
//  【知识点 8】swr_alloc_set_opts2 —— 重采样器的新 API
// =============================================================================
/*
 * FFmpeg 8.1 里老的 swr_alloc_set_opts() **已经被删除**，原版代码：
 *
 *     ctx_ = swr_alloc_set_opts(ctx_,
 *                               av_get_default_channel_layout(out_channels),  // ← 函数已删除
 *                               AV_SAMPLE_FMT_FLTP, out_rate,
 *                               av_get_default_channel_layout(in_channels),   // ← 函数已删除
 *                               AV_SAMPLE_FMT_S16, in_rate,
 *                               0, NULL);
 *
 * 两处都得换：
 *   ① av_get_default_channel_layout(n) → av_channel_layout_default(&layout, n)
 *   ② swr_alloc_set_opts(...)          → swr_alloc_set_opts2(&ctx_, ...)
 *
 * 新 API 的签名（注意参数翻倍，因为每个声道布局是一个结构体而不是掩码）：
 *
 *   int swr_alloc_set_opts2(struct SwrContext **ps,
 *                           const AVChannelLayout *out_ch_layout,
 *                           enum AVSampleFormat    out_sample_fmt,
 *                           int                    out_sample_rate,
 *                           const AVChannelLayout *in_ch_layout,
 *                           enum AVSampleFormat    in_sample_fmt,
 *                           int                    in_sample_rate,
 *                           int log_offset, void *log_ctx);
 *
 * 三个必须注意的点：
 *   1) 第一个参数是**二级指针**，传 &ctx_（它内部自己分配 SwrContext）
 *   2) 失败时它会把 *ps 设成 NULL 并自己释放，不需要我们再 free
 *   3) 栈上的 AVChannelLayout 用完要 av_channel_layout_uninit()
 *      （NATIVE 布局其实没有堆内存，uninit 是空操作，但按规范写，换 CUSTOM 也不会漏）
 */
int AudioResampler::InitFromS16ToFLTP(int in_channels, int in_sample_rate,
                                      int out_channels, int out_sample_rate)
{
    DeInit();

    in_channels_     = in_channels;
    in_sample_rate_  = in_sample_rate;
    out_channels_    = out_channels;
    out_sample_rate_ = out_sample_rate;

    /* 输入/输出布局：按声道数取标准布局（2 → stereo） */
    AVChannelLayout in_layout{};
    AVChannelLayout out_layout{};
    av_channel_layout_default(&in_layout, in_channels_);
    av_channel_layout_default(&out_layout, out_channels_);

    int ret = swr_alloc_set_opts2(&ctx_,
                                  /* 目标：AAC 编码器要的 */
                                  &out_layout, AV_SAMPLE_FMT_FLTP, out_sample_rate_,
                                  /* 源：PCM 文件实际的 */
                                  &in_layout,  AV_SAMPLE_FMT_S16,  in_sample_rate_,
                                  0, nullptr);

    /* 栈上布局用完就释放（不管成败都要做） */
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (ret < 0 || !ctx_) {
        std::printf("swr_alloc_set_opts2 失败: %s\n", errStr(ret).c_str());
        return -1;
    }

    /*
     * swr_init 会真正计算重采样滤波器系数（比较耗时），
     * 所以只在初始化时调一次，绝不能放进编码循环里。
     */
    ret = swr_init(ctx_);
    if (ret < 0) {
        std::printf("swr_init 失败: %s\n", errStr(ret).c_str());
        DeInit();
        return -1;
    }

    std::printf("AudioResampler 初始化成功: s16/%dch/%dHz -> fltp/%dch/%dHz\n",
                in_channels_, in_sample_rate_, out_channels_, out_sample_rate_);
    return 0;
}

int AudioResampler::ResampleFromS16ToFLTP(const uint8_t* in_data, AVFrame* out_frame)
{
    if (!ctx_ || !in_data || !out_frame) {
        return -1;
    }

    /*
     * swr_convert 的输入参数类型是 `const uint8_t **`（平面指针数组）。
     *
     * 【C++ 类型坑】不能写 const_cast<const uint8_t**>(in_data) ——
     *   const_cast 只能增删「指针本身」的限定符，
     *   没法把 `uint8_t*` 变成 `const uint8_t**`（两个不同类型）。
     *   正确做法是用数组接一层，数组退化成二级指针，类型天然匹配。
     *
     * 输入是 s16 **packed（交错）**，只有一个平面，所以数组长度 1。
     * 如果输入是 planar（如 fltp），就要按声道数给 N 个指针。
     */
    const uint8_t* in_planes[1] = { in_data };

    /*
     * swr_convert(ctx, out, out_count, in, in_count)
     *   out_count 传「目标缓冲区能装多少采样点」（= out_frame->nb_samples）
     *   返回值是**实际写出的采样点数**，不一定等于 in_count：
     *     采样率相同时通常相等；不同时 swr 内部 FIFO 会缓存尾巴。
     *   所以必须用返回值，不能假设。
     *
     * 本工程 44100→44100 没变采样率，所以这里是恒等变换 ——
     * 这次重采样本质上只做「s16 packed → fltp planar」的**格式转换**。
     */
    const int out_samples = swr_convert(ctx_, out_frame->data, out_frame->nb_samples,
                                       in_planes, out_frame->nb_samples);
    if (out_samples <= 0) {
        std::printf("swr_convert 失败或没有输出: %d\n", out_samples);
        return -1;
    }
    return out_samples;
}

void AudioResampler::DeInit()
{
    if (ctx_) {
        swr_free(&ctx_);   // 会把 ctx_ 置 NULL，重复调用安全
    }
}

// =============================================================================
//  【知识点 9】分配 / 释放 fltp 帧
// =============================================================================
AVFrame* AllocFltpPcmFrame(int channels, int nb_samples, int sample_rate)
{
    AVFrame* pcm = av_frame_alloc();
    if (!pcm) {
        std::printf("av_frame_alloc 失败\n");
        return nullptr;
    }

    /* 三要素 + sample_rate，全部设好才能 get_buffer */
    pcm->format = AV_SAMPLE_FMT_FLTP;

    /*
     * FFmpeg 8.1：AVFrame 的 channels / channel_layout 字段都已删除，
     * 只剩 ch_layout。原版的写法：
     *     pcm->channels       = channels;                          // ✗ 编译错误
     *     pcm->channel_layout = av_get_default_channel_layout(n);  // ✗ 函数已删除
     *
     * ★ 注意这里**不能**写 av_channel_layout_copy(&pcm->ch_layout, ...)，
     *   因为 av_frame_alloc 之后 ch_layout 是全 0 的合法空布局，
     *   用 av_channel_layout_default 直接构造即可（它内部会先 uninit）。
     */
    av_channel_layout_default(&pcm->ch_layout, channels);

    pcm->sample_rate = sample_rate;
    pcm->nb_samples  = nb_samples;

    int ret = av_frame_get_buffer(pcm, 0);
    if (ret < 0) {
        std::printf("av_frame_get_buffer 失败: %s\n", errStr(ret).c_str());
        av_frame_free(&pcm);
        return nullptr;
    }
    return pcm;
}

void FreePcmFrame(AVFrame* frame)
{
    if (frame) {
        av_frame_free(&frame);
    }
}
