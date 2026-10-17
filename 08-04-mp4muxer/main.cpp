// =============================================================================
//  08-04-mp4muxer / main.cpp
//  YUV + PCM → H.264 + AAC → MP4：音视频「同步」封装的完整流程
//
// ===========================================================================
//  这份代码要学什么
// ===========================================================================
//  前面三个工程各讲一件事：
//    08-01 音频编码（裸 PCM → AAC）
//    08-02 视频编码（裸 YUV → H.264）
//    08-03 封装    （生成合成音视频 → flv/mp4）
//
//  08-04 把三件事**组装成一个真正的转封装程序**，并且回答一个 08-03 没讲透
//  的问题：**两路流的时间戳怎么对上（音视频同步）**。
//
//  代码结构（4 个类各管一件事，对应 pusher 的分层）：
//
//      main.cpp
//        ├── VideoEncoder      YUV  → H.264 包
//        ├── AudioEncoder      PCM  → AAC 包（只吃 fltp）
//        ├── AudioResampler    s16 packed → fltp planar
//        └── Muxer             建流/写头/交织写包/写尾
//
//  知识点索引（按文件）：
//      videoencoder.cpp  【知识点 2】编码器参数  【3】时间基换算
//                        【4】零拷贝喂帧       【5】一次 send 出 N 个包
//      audioencoder.cpp  【知识点 6】AAC 只吃 fltp  【7】ch_layout 新 API
//      audioresampler.cpp【知识点 8】swr_alloc_set_opts2  【9】fltp 帧的分配
//      muxer.cpp         【知识点 10】参数交给流  【11】写头后 time_base 才定
//                        【12】包的时间基转换    【13】交织写入
//                        【14】trailer(moov)     【15】DeInit 正确姿势
//      main.cpp          【知识点 16】统一时间轴（微秒）
//                        【17】音视频交织调度
//                        【18】冲刷顺序
//                        【19】C++ 的收尾：不需要 goto cleanup
//
//  运行环境：Qt 6.11.0 (MinGW 13.1.0) + FFmpeg 8.1
//            纯 C++17，不链接 Qt 库，只用 Qt 的工具链
//
//  用法：
//      mp4muxer <in.yuv> <in.pcm> <out.mp4>
//
//  测试素材（第一次运行前执行同目录的 fetch_test_media.bat 生成）：
//      ffmpeg -i sound_in_sync_test.mp4 -pix_fmt yuv420p 720x576_yuv420p.yuv
//      ffmpeg -i sound_in_sync_test.mp4 -vn -ar 44100 -ac 2 -f s16le 44100_2_s16le.pcm
// =============================================================================

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audioencoder.h"
#include "audioresampler.h"
#include "muxer.h"
#include "videoencoder.h"

namespace {

// -----------------------------------------------------------------------------
// 编码参数（对应原版的 #define —— C++ 里用 constexpr，有类型、不会宏污染）
// -----------------------------------------------------------------------------
constexpr int kYuvWidth  = 720;
constexpr int kYuvHeight = 576;
constexpr int kYuvFps    = 25;

constexpr int kVideoBitRate = 500 * 1024;

constexpr int kPcmChannels   = 2;
constexpr int kPcmSampleRate = 44100;
constexpr int kAudioBitRate  = 128 * 1024;

/*
 * 【知识点 16】统一时间轴：用微秒
 * -----------------------------------------------------------------
 * 音频和视频的「自然时间基」不一样：
 *     音频一次处理 1024 个采样点 → 天然单位是 1/44100 秒 ≈ 22.7 微秒
 *     视频一次处理 1 帧         → 天然单位是 1/25 秒 = 40 毫秒
 *
 * 要把两路放进同一个 while 循环比较「谁的时间更早」，必须先统一到一条轴上。
 * 本工程选 **1/1000000 秒（微秒）**：精度足够细，两种流能直接比大小。
 *
 * 于是：
 *     audio_pts（微秒） = 已送出的采样点数 / 44100 * 1000000
 *     video_pts（微秒） = 已送出的帧号     / 25    * 1000000
 *
 * 送进编码器时再 av_rescale_q 换算成编码器自己的 time_base
 * （视频 {1,1000000} 恒等；音频 {1,44100}）。
 * 写进 mp4 时 muxer 还要再换算一次到 stream 的 time_base。
 * -----------------------------------------------------------------
 */
constexpr int kUnifiedTimeBase = 1000000;   // 1 秒 = 1000000 个时间单位

/* 关闭 FILE* 的删除器，配合 unique_ptr 做 RAII */
struct FileCloser {
    void operator()(FILE* fp) const
    {
        if (fp) std::fclose(fp);
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr OpenRead(const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    return FilePtr(fp);   // 失败时持有 nullptr
}

void Usage(const char* prog)
{
    std::printf("用法: %s <in.yuv> <in.pcm> <out.mp4>\n", prog);
    std::printf("  例: %s 720x576_yuv420p.yuv 44100_2_s16le.pcm out.mp4\n", prog);
    std::printf("\n素材生成（同目录 fetch_test_media.bat 也做了这件事）:\n");
    std::printf("  ffmpeg -i sound_in_sync_test.mp4 -pix_fmt yuv420p 720x576_yuv420p.yuv\n");
    std::printf("  ffmpeg -i sound_in_sync_test.mp4 -vn -ar 44100 -ac 2 -f s16le 44100_2_s16le.pcm\n");
}

/*
 * 【知识点 19】C++ 的收尾：不需要 goto cleanup
 * -----------------------------------------------------------------
 * 08-02 / 08-03 里为了「中途失败也能统一释放」，把主体塞进立即执行的 lambda。
 * 这个工程用了另一条更彻底的路：**RAII**
 *
 *   - 文件          → std::unique_ptr<FILE, FileCloser>
 *   - 裸 PCM/YUV 缓冲 → std::vector<uint8_t>（自动释放，不用 malloc/free）
 *   - 编码器/重采样/muxer → 各自类的析构函数里调 DeInit
 *
 * 这样任何 return 都会自动按「构造的逆序」析构，一行清理代码都不用写。
 * 这是 C++ 相比 C 最实用的优势，也是 pusher 里各组件采用的风格。
 * -----------------------------------------------------------------
 */
int Run(const std::string& inYuv, const std::string& inPcm, const std::string& outMp4)
{
    FilePtr in_yuv = OpenRead(inYuv);
    if (!in_yuv) {
        std::printf("打不开 YUV 文件: %s\n", inYuv.c_str());
        return -1;
    }
    FilePtr in_pcm = OpenRead(inPcm);
    if (!in_pcm) {
        std::printf("打不开 PCM 文件: %s\n", inPcm.c_str());
        return -1;
    }

    // =========================================================================
    //  1) 视频编码器
    // =========================================================================
    VideoEncoder video_encoder;
    if (video_encoder.InitH264(kYuvWidth, kYuvHeight, kYuvFps, kVideoBitRate) < 0) {
        std::printf("VideoEncoder::InitH264 失败\n");
        return -1;
    }

    /* 一帧 YUV420P 的字节数 = Y(w*h) + U(w*h/4) + V(w*h/4) = w*h*3/2 */
    const int y_frame_size = kYuvWidth * kYuvHeight;
    const int u_frame_size = kYuvWidth * kYuvHeight / 4;
    const int v_frame_size = kYuvWidth * kYuvHeight / 4;
    const int yuv_frame_size = y_frame_size + u_frame_size + v_frame_size;

    std::vector<uint8_t> yuv_frame_buf(static_cast<size_t>(yuv_frame_size));

    // =========================================================================
    //  2) 音频编码器 + 重采样器
    // =========================================================================
    AudioEncoder audio_encoder;
    if (audio_encoder.InitAAC(kPcmChannels, kPcmSampleRate, kAudioBitRate) < 0) {
        std::printf("AudioEncoder::InitAAC 失败\n");
        return -1;
    }

    /*
     * 一次读多少字节的原始 PCM：
     *   每采样点字节数 × 声道数 × 每声道采样点数
     *   = 2（s16） × 2 × 1024（AAC frame_size）
     *   = 4096 字节
     *
     * 为什么用编码器的 frame_size 而不是随便定个大小？
     * 因为 AAC 每帧必须正好 1024 个采样点。文件的采样总数一般不是 1024 的整数倍，
     * 所以最后一块会短 —— 本工程直接丢弃尾部不足一帧的部分（见下面的说明）。
     */
    const int pcm_frame_size = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16)
                             * kPcmChannels
                             * audio_encoder.GetFrameSize();
    if (pcm_frame_size <= 0) {
        std::printf("pcm_frame_size 计算错误: %d\n", pcm_frame_size);
        return -1;
    }
    std::vector<uint8_t> pcm_frame_buf(static_cast<size_t>(pcm_frame_size));

    /* s16 packed → fltp planar；输出声道/采样率跟编码器保持一致 */
    AudioResampler audio_resampler;
    if (audio_resampler.InitFromS16ToFLTP(kPcmChannels, kPcmSampleRate,
                                          audio_encoder.GetChannels(),
                                          audio_encoder.GetSampleRate()) < 0) {
        std::printf("AudioResampler::InitFromS16ToFLTP 失败\n");
        return -1;
    }

    // =========================================================================
    //  3) MP4 封装器：建流 → 开 IO → 写头
    // =========================================================================
    Muxer mp4_muxer;
    if (mp4_muxer.Init(outMp4) < 0) {
        std::printf("Muxer::Init 失败\n");
        return -1;
    }

    /*
     * 顺序很重要：
     *   ① AddStream 必须在编码器 **open 之后** ——
     *      因为要拷 codec_ctx 的 extradata(SPS/PPS、AudioSpecificConfig)，
     *      而 extradata 是 avcodec_open2 才生成的。
     *   ② Open 必须在 SendHeader **之前** —— IO 得先能写。
     *   ③ SendHeader 之后 stream 的 time_base 才最终确定。
     */
    if (mp4_muxer.AddStream(video_encoder.GetCodecContext()) < 0) {
        std::printf("AddStream(video) 失败\n");
        return -1;
    }
    if (mp4_muxer.AddStream(audio_encoder.GetCodecContext()) < 0) {
        std::printf("AddStream(audio) 失败\n");
        return -1;
    }
    if (mp4_muxer.Open() < 0) {
        std::printf("Muxer::Open 失败\n");
        return -1;
    }
    if (mp4_muxer.SendHeader() < 0) {
        std::printf("Muxer::SendHeader 失败\n");
        return -1;
    }

    const int audio_index = mp4_muxer.GetAudioStreamIndex();
    const int video_index = mp4_muxer.GetVideoStreamIndex();

    // =========================================================================
    //  4) 主循环：读一帧 → 编码 → 封装（按时间戳交织）
    // =========================================================================

    /* 两路各自的时间轴游标（单位：微秒，见知识点 16） */
    double audio_pts = 0.0;
    double video_pts = 0.0;

    /* 每送一帧，时间轴前进多少微秒 */
    const double audio_frame_duration =
        1.0 * audio_encoder.GetFrameSize() / kPcmSampleRate * kUnifiedTimeBase;
    const double video_frame_duration =
        1.0 / kYuvFps * kUnifiedTimeBase;

    bool audio_finished = false;
    bool video_finished = false;

    int64_t video_frames = 0;
    int64_t audio_frames = 0;
    int64_t loop_count   = 0;

    std::vector<AVPacket*> packets;
    packets.reserve(8);

    /*
     * 【知识点 17】音视频交织调度
     * -----------------------------------------------------------------
     * 目标：让两路的时间戳尽量交替前进，而不是「先把视频全写完再写音频」。
     * 后者会破坏 muxer 的 dts 单调性检查（音频 dts 从 0 开始就倒退了），
     * 播放器 seek 也会乱。
     *
     * 判据：**谁的时间戳更靠后，就先补谁**。
     *   audio_pts > video_pts  → 音频跑在前面了，赶快补视频
     *   audio_pts <= video_pts → 视频在前面，补音频
     *
     * 另外两路先后结束时：
     *   - 某一路读到文件末尾 → 标记 finished，并**立刻冲刷**它的编码器
     *     （把内部压着的帧吐出来），之后就不再碰它
     *   - 两路都 finished → 退出循环
     *
     * 原版的三重条件写成：
     *     (video_finish != 1 && audio_pts > video_pts) ||
     *     (video_finish != 1 && audio_finish == 1)
     * 等价于下面这个更好读的版本。
     * -----------------------------------------------------------------
     */
    while (!audio_finished || !video_finished) {
        const bool do_video = !video_finished &&
                              (audio_finished || audio_pts > video_pts);

        /*
         * 日志限流：调度决策只打印前 8 轮 + 之后每 60 轮一次。
         * 想看每一轮就把条件改成 true —— 你会清楚看到两路时间轴如何互相追赶。
         */
        if (++loop_count <= 8 || loop_count % 60 == 0) {
            std::printf("apts:%0.0f vpts:%0.0f  → 处理%s\n",
                        audio_pts / 1000, video_pts / 1000, do_video ? "视频" : "音频");
        }

        if (do_video) {
            const size_t read_len = std::fread(yuv_frame_buf.data(), 1,
                                              yuv_frame_size, in_yuv.get());
            if (read_len < static_cast<size_t>(yuv_frame_size)) {
                /*
                 * 尾部的不足一帧数据直接丢弃。
                 * （裸 YUV 没有头部，没法判断「半帧」是损坏还是正常结束；
                 *   要精确处理就得靠容器里的帧数，那就变成解封装了。）
                 */
                video_finished = true;
                std::printf("YUV 读取完毕（最后一块 %zu/%d 字节）\n",
                            read_len, yuv_frame_size);
            }

            if (!video_finished) {
                if (video_encoder.Encode(yuv_frame_buf.data(), yuv_frame_size,
                                         video_index,
                                         static_cast<int64_t>(video_pts),
                                         kUnifiedTimeBase, packets) < 0) {
                    std::printf("video Encode 失败\n");
                    return -1;
                }
                ++video_frames;
            } else {
                /* 【知识点 18】冲刷：send(nullptr)，把编码器内部残留全部吐出 */
                std::printf("flush video encoder\n");
                if (video_encoder.Encode(nullptr, 0, video_index,
                                         static_cast<int64_t>(video_pts),
                                         kUnifiedTimeBase, packets) < 0) {
                    std::printf("video flush 失败\n");
                    return -1;
                }
            }

            /* 时间轴前进一格（注意：用「整数化后」的值不影响判据的精度） */
            video_pts += video_frame_duration;
        } else {
            const size_t read_len = std::fread(pcm_frame_buf.data(), 1,
                                              pcm_frame_size, in_pcm.get());
            if (read_len < static_cast<size_t>(pcm_frame_size)) {
                audio_finished = true;
                std::printf("PCM 读取完毕（最后一块 %zu/%d 字节）\n",
                            read_len, pcm_frame_size);
            }

            if (!audio_finished) {
                /*
                 * 分配一帧 fltp 承接重采样结果。
                 * nb_samples 必须 = 编码器 frame_size（1024），
                 * 否则 swr 的输出和编码器期望的对不上。
                 */
                AVFrame* fltp_frame = AllocFltpPcmFrame(kPcmChannels,
                                                       audio_encoder.GetFrameSize(),
                                                       kPcmSampleRate);
                if (!fltp_frame) {
                    std::printf("AllocFltpPcmFrame 失败\n");
                    return -1;
                }

                /* s16 packed → fltp planar */
                if (audio_resampler.ResampleFromS16ToFLTP(pcm_frame_buf.data(),
                                                          fltp_frame) < 0) {
                    std::printf("ResampleFromS16ToFLTP 失败\n");
                    FreePcmFrame(fltp_frame);
                    return -1;
                }

                if (audio_encoder.Encode(fltp_frame, audio_index,
                                         static_cast<int64_t>(audio_pts),
                                         kUnifiedTimeBase, packets) < 0) {
                    std::printf("audio Encode 失败\n");
                    FreePcmFrame(fltp_frame);
                    return -1;
                }
                FreePcmFrame(fltp_frame);   // 编码器已拷走数据，可立即释放
                ++audio_frames;
            } else {
                std::printf("flush audio encoder\n");
                if (audio_encoder.Encode(nullptr, audio_index,
                                         static_cast<int64_t>(audio_pts),
                                         kUnifiedTimeBase, packets) < 0) {
                    std::printf("audio flush 失败\n");
                    return -1;
                }
            }

            audio_pts += audio_frame_duration;
        }

        /* 把本次产出的包全部交给 muxer（SendPacket 内部会 free 掉它们） */
        for (AVPacket* pkt : packets) {
            if (mp4_muxer.SendPacket(pkt) < 0) {
                /*
                 * SendPacket 失败时它自己已经 free 了这个包，
                 * 所以这里只需清空容器，避免二次释放。
                 */
                packets.clear();
                return -1;
            }
        }
        packets.clear();
    }

    // =========================================================================
    //  5) 写 trailer（mp4 的 moov 在这里，绝不能漏）
    // =========================================================================
    if (mp4_muxer.SendTrailer() < 0) {
        std::printf("Muxer::SendTrailer 失败\n");
        return -1;
    }

    /*
     * 显式关闭输出。
     *
     * 为什么这里要手动调一次 DeInit（明明析构里也会调）？
     *   av_write_trailer 之后数据还在 AVIOContext 的缓冲里，
     *   要等 avio_closep 才真正落盘。而下面要读文件大小，
     *   不先关闭就会读到 0 字节（本工程第一版就踩了这个）。
     *
     * DeInit 是幂等的（指针用完置 nullptr），析构里再调一次无害。
     */
    mp4_muxer.DeInit();

    // =========================================================================
    //  6) 汇总
    // =========================================================================
    std::printf("\n===== 封装完成 =====\n");
    std::printf("视频   : %" PRId64 " 帧，时间轴 %.3f 秒\n",
                video_frames, video_pts / kUnifiedTimeBase);
    std::printf("音频   : %" PRId64 " 帧（每帧 %d 采样点），时间轴 %.3f 秒\n",
                audio_frames, audio_encoder.GetFrameSize(),
                audio_pts / kUnifiedTimeBase);

    /* C++17 <filesystem>：文件已关闭，这里读到的是真实大小 */
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(outMp4, ec);
    if (!ec) {
        std::printf("输出   : %s  (%.2f KB)\n", outMp4.c_str(),
                    static_cast<double>(file_size) / 1024.0);
    }

    /*
     * 音视频是否「同步」看两个时间轴是否接近：
     * 两条流长度不一致是正常的（音视频轨长度本来就可能差几十毫秒），
     * 但如果差到秒级，就是时间戳算错了。
     */
    const double drift = (audio_pts - video_pts) / kUnifiedTimeBase;
    std::printf("时间轴差: %.3f 秒（音 - 视，正常应在 1 帧以内）\n", drift);

    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        Usage(argv[0]);
        return -1;
    }

    const std::string in_yuv_name  = argv[1];
    const std::string in_pcm_name  = argv[2];
    const std::string out_mp4_name = argv[3];

    std::printf("输入 YUV : %s\n", in_yuv_name.c_str());
    std::printf("输入 PCM : %s\n", in_pcm_name.c_str());
    std::printf("输出 MP4 : %s\n\n", out_mp4_name.c_str());

    return Run(in_yuv_name, in_pcm_name, out_mp4_name);
}

/*
 * ===========================================================================
 * 附：从本工程到 pusher 的对应关系
 * ===========================================================================
 *   本工程                          pusher 里
 *   ──────────────────────         ─────────────────────────────────
 *   VideoEncoder               →   X264VideoEncoder（加 preset/tune、微秒输入）
 *   AudioEncoder               →   AacAudioEncoder
 *   AudioResampler             →   AacAudioEncoder 内部用 swr 完成同样的转换
 *   Muxer                      →   FormatMuxer（多一层 rw_timeout / 中断回调 /
 *                                  断线重连，因为输出是网络而不是文件）
 *   main 的 while 循环          →   Pusher 里拆成三条线程 + 有界队列
 *                                  （采集线程 → 编码线程 → 推流线程）
 *
 * 【本工程刻意简化掉的东西 —— 真实推流必须补上】
 *   1. 没有网络超时（rw_timeout）和中断回调 → 断网会卡死在写操作上
 *   2. 没有重连（muxer 写失败后应销毁重建，并重新发关键帧）
 *   3. 用文件模拟了「实时采集」，没做帧率节流（真实采集要按 fps 节奏喂帧）
 *   4. 音视频在同一线程里串行编码 → 实时场景要拆线程，否则一路慢就拖累另一路
 * ===========================================================================
 */
