// =============================================================================
//  muxer.h —— MP4 封装器（对应 pusher 的 muxer 层）
//
//  职责：建流 → 写头 → 收包（换算时间基 + 交织写入）→ 写 trailer。
//
//  【知识点 10】add stream：把编码器参数交给容器
//  【知识点 11】写头之后 stream 的 time_base 才最终确定
//  【知识点 12】包的时间基转换（codec time_base → stream time_base）
//  【知识点 13】av_interleaved_write_frame：按 dts 交织
//  【知识点 14】trailer：mp4 的 moov 在这里，不写文件就废
//  【知识点 15】DeInit 的正确姿势（原版用错了函数）
// =============================================================================
#ifndef MUXER_H
#define MUXER_H

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class Muxer
{
public:
    Muxer() = default;
    ~Muxer();                 // 析构里兜底 DeInit，异常路径也不会漏

    // 按文件名/扩展名猜容器（.mp4 → mp4）。返回 <0 表示失败
    int  Init(const std::string& url);
    // 释放容器与 IO；可重复调用
    void DeInit();

    // 用编码器上下文建一路流；内部按 codec_type 区分音/视频
    int  AddStream(AVCodecContext* codec_ctx);

    int  SendHeader();
    int  SendPacket(AVPacket* packet);   // 接管 packet 所有权，内部会 free
    int  SendTrailer();

    int  Open();                         // avio_open（文件或网络地址）

    int  GetAudioStreamIndex() const { return audio_index_; }
    int  GetVideoStreamIndex() const { return video_index_; }

private:
    AVFormatContext* fmt_ctx_ = nullptr;
    std::string      url_;

    // 记下每路流「编码器上下文」和「目标 stream」，写包时要用它们做时间基换算
    AVCodecContext* aud_codec_ctx_ = nullptr;
    AVStream*       aud_stream_    = nullptr;
    AVCodecContext* vid_codec_ctx_ = nullptr;
    AVStream*       vid_stream_    = nullptr;

    int audio_index_ = -1;
    int video_index_ = -1;

    int64_t packets_written_ = 0;   // 已写入包数（限流日志 + 汇总）
};

#endif // MUXER_H
