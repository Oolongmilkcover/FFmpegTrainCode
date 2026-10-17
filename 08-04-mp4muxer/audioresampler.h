// =============================================================================
//  audioresampler.h —— 音频重采样 / 采样格式转换（s16 packed → fltp planar）
//
//  职责：AAC 编码器只吃 fltp，而输入 PCM 是 s16 交错，所以中间转一道。
//
//  【知识点 8】swr_alloc_set_opts2 —— FFmpeg 8.1 的重采样器新 API
//  【知识点 9】重采样输出帧的分配：AVFrame 的音频三要素
// =============================================================================
#ifndef AUDIORESAMPLER_H
#define AUDIORESAMPLER_H

#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

/*
 * 【知识点 9】分配一帧「编码器要的」音频帧
 *
 * 音频帧由三要素完全确定，缺一不可：
 *     format     AV_SAMPLE_FMT_FLTP
 *     ch_layout  声道布局（这里按声道数取标准布局）
 *     nb_samples 每声道多少个采样点（= 编码器的 frame_size，AAC 是 1024）
 *
 * 另外 sample_rate 也要填：虽然 av_frame_get_buffer 不依赖它，
 * 但下游（swr_convert / 编码器校验）会读它，漏填容易出怪问题。
 *
 * 返回值：成功返回帧（用完 FreePcmFrame），失败返回 nullptr。
 */
AVFrame* AllocFltpPcmFrame(int channels, int nb_samples, int sample_rate);

void FreePcmFrame(AVFrame* frame);

class AudioResampler
{
public:
    AudioResampler() = default;
    ~AudioResampler();        // 析构里调 DeInit

    // 源固定为 s16 packed；目标固定为 fltp planar（因为 AAC 只吃这个）
    int  InitFromS16ToFLTP(int in_channels, int in_sample_rate,
                          int out_channels, int out_sample_rate);
    void DeInit();

    /*
     * 把一块 s16 交错 PCM 转成 fltp 分平面，写进 out_frame。
     * 返回实际写出的采样点数（可能 != out_frame->nb_samples），<0 表示失败。
     */
    int ResampleFromS16ToFLTP(const uint8_t* in_data, AVFrame* out_frame);

private:
    int         in_channels_    = 0;
    int         in_sample_rate_ = 0;
    int         out_channels_   = 0;
    int         out_sample_rate_= 0;
    SwrContext* ctx_            = nullptr;   // 名字带 swr_，避免和 avformat 的 ctx 混淆
};

#endif // AUDIORESAMPLER_H
