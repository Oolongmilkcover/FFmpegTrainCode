// =============================================================================
//  videoencoder.h —— H.264 视频编码器（对应 pusher 的 video encoder 层）
//
//  职责：把一帧 yuv420p 裸数据编成 H.264 的 AVPacket。
//
//  【知识点 2】视频编码器的参数
//  【知识点 3】时间基换算：调用方的「统一时间轴」→ 编码器自己的 time_base
//  【知识点 4】零拷贝喂帧：av_image_fill_arrays 把外部 buffer 挂到 AVFrame 上
//  【知识点 5】一次 send 可能产出 N 个包 → Encode 变成「往 vector 里塞」
// =============================================================================
#ifndef VIDEOENCODER_H
#define VIDEOENCODER_H

#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class VideoEncoder
{
public:
    VideoEncoder() = default;
    ~VideoEncoder();          // 析构里调 DeInit，避免调用方忘记释放

    // 打开编码器；width/height 必须和 YUV 实际尺寸一致，且为偶数
    int  InitH264(int width, int height, int fps, int bit_rate);
    void DeInit();

    /*
     * 编码一帧（或冲刷）。
     *
     * yuv_data == nullptr 表示冲刷（flush）：
     *   把编码器内部压着的帧全部吐出来，不冲刷会丢尾部若干帧。
     *
     * pts/time_base 描述「这一帧在调用方时间轴上的位置」：
     *   time_base = 1000000 表示 pts 的单位是「微秒」。
     *   函数内部会换算到 codec_ctx_->time_base。
     *
     * packets 是出参：一次 send 可能产出多个包，所以用 vector 承接。
     *   包的所有权交给调用方，用完必须自己 av_packet_free。
     *
     * 返回 0 成功，<0 失败。
     */
    int Encode(const uint8_t* yuv_data, int yuv_size, int stream_index,
               int64_t pts, int64_t time_base, std::vector<AVPacket*>& packets);

    AVCodecContext* GetCodecContext() { return codec_ctx_; }
    int             GetWidth()  const { return width_; }
    int             GetHeight() const { return height_; }
    int             GetFps()    const { return fps_; }

private:
    int             width_     = 0;
    int             height_    = 0;
    int             fps_       = 25;
    int             bit_rate_  = 500 * 1024;
    int64_t         logged_    = 0;         // 已打印的包数（限流，避免刷屏）

    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame*        frame_     = nullptr;   // 复用的输入帧（不拥有 yuv 数据）
};

#endif // VIDEOENCODER_H
