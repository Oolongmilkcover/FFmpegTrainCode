// =============================================================================
//  audioencoder.h —— AAC 音频编码器（对应 pusher 的 audio encoder 层）
//
//  职责：把一帧 fltp PCM 编成 AAC 的 AVPacket。
//
//  【知识点 6】AAC 编码器的参数与「只吃 fltp」
//  【知识点 7】FFmpeg 8.1 的声道布局新 API：channels/channel_layout → ch_layout
// =============================================================================
#ifndef AUDIOENCODER_H
#define AUDIOENCODER_H

#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class AudioEncoder
{
public:
    AudioEncoder() = default;
    ~AudioEncoder();          // 析构里调 DeInit

    // 打开 AAC 编码器；channels 只给声道数，布局用标准默认（2→stereo）
    int  InitAAC(int channels, int sample_rate, int bit_rate);
    void DeInit();

    /*
     * 编码一帧（或冲刷）。frame == nullptr 表示冲刷。
     * pts/time_base 是调用方时间轴上的位置（本工程为微秒）。
     * packets 承接本次产出的 0..N 个包，所有权交给调用方。
     */
    int Encode(AVFrame* frame, int stream_index,
               int64_t pts, int64_t time_base, std::vector<AVPacket*>& packets);

    AVCodecContext* GetCodecContext() { return codec_ctx_; }

    // 一帧需要多少个采样点（AAC 恒为 1024）；0 表示未初始化
    int GetFrameSize();
    // 编码器要求的采样格式（AAC 是 AV_SAMPLE_FMT_FLTP）
    int GetSampleFormat();
    // 声道数（FFmpeg 8.1 从 ch_layout.nb_channels 读）
    int GetChannels();
    int GetSampleRate();

private:
    int             channels_    = 2;
    int             sample_rate_ = 44100;
    int             bit_rate_    = 128 * 1024;
    int64_t         logged_      = 0;         // 已打印的包数（限流）
    AVCodecContext* codec_ctx_   = nullptr;
};

#endif // AUDIOENCODER_H
