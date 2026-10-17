/**
 * @file    08-03-muxing_flv/main.cpp
 * @brief   封装：把 H.264 视频 + AAC 音频交织（interleave）成一个容器文件（flv/mp4/mkv…）
 *
 * ===========================================================================
 * 这份代码要学什么（对应简历项目 pusher 的 muxer 层）
 * ===========================================================================
 * 08-02 学会了「编码」。但推流/存盘还需要最后一步 —— **封装（muxing）**：
 * 把两路（或多路）压缩码流按时间戳交织成一条连续的字节流，加上容器头。
 *
 *   YUV 测试图 ─┐                        ┌─→ 08-02：裸 H.264（单路，无容器）
 *               ├─ 编码 ─→ AVPacket ─┐
 *   PCM 正弦波 ─┘                    ├─→ av_interleaved_write_frame ─→ out.flv
 *                                    │        （按 dts 排序、交织、加容器头）
 *                      时间基换算 ────┘
 *
 * 完整的封装流程（每一步都有坑，下面用「【知识点 N】」展开）：
 *
 *   ① 建输出上下文    avformat_alloc_output_context2（按扩展名/协议猜容器）
 *   ② 选编码器        avcodec_find_encoder(codec_id)
 *   ③ 建流            avformat_new_stream
 *   ④ 配编码器        宽高/采样率/声道布局/像素格式/采样格式
 *   ⑤ 开编码器        avcodec_open2   ← GLOBAL_HEADER 在这里之前要设好
 *   ⑥ 把编码参数给流  avcodec_parameters_from_context（★ SPS/PPS 等 extradata）
 *   ⑦ 开输出 IO       avio_open
 *   ⑧ 写容器头        avformat_write_header  ← ★ 之后 st->time_base 会被改写！
 *   ⑨ 交织写包        av_packet_rescale_ts + av_interleaved_write_frame
 *   ⑩ 写容器尾        av_write_trailer        ← mp4 的 moov 在这里，不写文件就废
 *
 * ===========================================================================
 * 环境：Qt 6.11.0 (MinGW 13.1.0) + FFmpeg 8.1
 *       本工程是纯 C++17，不链接 Qt 库，只用 Qt 的工具链
 * ===========================================================================
 * 用法：
 *   muxing_flv <out.flv> [format] [yuvFile] [w] [h] [fps] [duration]
 *   默认：format 按扩展名猜；无 yuvFile → 用合成测试图；fps=25 duration=5
 *
 *   例（合成测试图 + 正弦声音，生成 flv）：
 *     muxing_flv out.flv
 *   例（同上但生成 mp4，对比容器差异）：
 *     muxing_flv out.mp4
 *   例（用真实 YUV 当视频源，尺寸要自己给）：
 *     muxing_flv out.flv flv ../08-02-encode_video/yuv420p_1280x720.yuv 1280 720 25 5
 * ===========================================================================
 * @author  Liao Qingfu (原始 C 版本, 源自 FFmpeg doc/examples/muxing.c)
 *          / 改写为 C++17 + FFmpeg 8.1
 */

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>   /* AVChannelLayout 相关 */
#include <libavutil/error.h>            /* av_strerror */
#include <libavutil/imgutils.h>         /* av_image_fill_arrays / av_get_pix_fmt_name */
#include <libavutil/mathematics.h>      /* av_rescale_q / av_compare_ts */
#include <libavutil/opt.h>              /* av_opt_set */
#include <libavutil/timestamp.h>        /* av_ts_make_string / av_ts_make_time_string */
#include <libswresample/swresample.h>   /* 正弦波 s16 → AAC 要的 fltp */
#include <libswscale/swscale.h>         /* 万一编码器不吃 yuv420p */
}

namespace {

/* ========================== 全局配置 ========================== */

/* 默认生成多少秒（可在命令行覆盖，见 main 的 duration 参数）
 * 注意：真正控制时长的变量是下面的 gDuration，它在 main 解析完命令行后赋值。
 * 之所以用全局变量而不是到处传参，是因为 getVideoFrame/getAudioFrame
 * 被 encodeAndWrite 的调用链间接使用，签名加参数会越传越长。
 * 生产代码（pusher）会把这类配置放进 Config 结构体，由各组件持有。 */
constexpr double kDefaultDuration = 5.0;
constexpr int    kStreamFrameRate = 25;     /* 视频帧率 */
constexpr int    kScaleFlags      = SWS_BICUBIC;

double gDuration = kDefaultDuration;        /* 实际时长（秒），由 main 赋值 */

/*
 * 是否打印每一个包的时间戳。
 * 5 秒的示例会产生 125 个视频包 + 217 个音频包 = 342 行，太刷屏；
 * 所以默认只打印「前若干个包 + 每个关键帧」，用来观察时间基换算和交织顺序。
 * 命令行给 -v 可以打开全部打印（排查时间戳问题时会需要）。
 */
bool gVerbosePackets = false;

/* 已打印的包计数，用于「只打印前 N 个」的限流 */
int gLoggedPackets = 0;

/*
 * 【知识点 1】OutputStream —— 一路流（视频或音频）需要的全部状态
 * ---------------------------------------------------------------------------
 * 封装 muxing 的本质就是「管理 N 路流」，每路流的状态是一样的：
 *   编码器上下文 + 一帧输入缓冲 + 时间戳游标 + 重采样/缩放上下文
 * 所以抽象成一个结构体，视频和音频各一个实例。
 *
 * ★ 为什么 st->time_base 要单独讲？
 *   有三套时间基同时在用，混了就会得到时长离谱的文件：
 *     ① 你生成帧时用的时间基（这里：视频 1/fps，音频 1/sample_rate）
 *     ② 编码器 cc->time_base（一般等于 ①）
 *     ③ **输出流 st->time_base**（由容器决定：flv 是 1/1000，mp4 常被改成 1/90000…）
 *   ①② 是「输入侧」，③ 是「输出侧」。写包时必须把 ② 换算到 ③。
 *   而且 ③ 在 avformat_write_header 之后才最终确定 —— 所以要「现用现查」。
 * ---------------------------------------------------------------------------
 */
struct OutputStream {
    AVStream*       st  = nullptr;   /* 容器里的一路流 */
    AVCodecContext* enc = nullptr;   /* 这路流的编码器上下文 */

    /* 下一个要生成的帧的时间戳（单位 = enc->time_base）
     * 视频：帧号，每次 +1
     * 音频：累计采样数，每次 +nb_samples */
    int64_t nextPts = 0;

    /* 音频：累计写出的采样数（用于算帧 pts） */
    int64_t samplesCount = 0;

    AVFrame* frame    = nullptr;     /* 送给编码器的帧 */
    AVFrame* tmpFrame = nullptr;     /* 转换前的原始帧（正弦波 s16 / yuv420p 测试图） */

    /* 生成合成信号用的相位累加器（原示例用它产生扫频声音） */
    double t = 0.0, tincr = 0.0, tincr2 = 0.0;

    SwsContext* sws = nullptr;       /* 视频像素格式转换（编码器不吃 yuv420p 时用） */
    SwrContext* swr = nullptr;       /* 音频重采样/重格式（设备给 s16，AAC 要 fltp） */

    /* 统计用 */
    int64_t packetsWritten = 0;
    int64_t bytesWritten   = 0;

    /* 视频专用：真实 YUV 输入文件（nullptr 则用合成测试图） */
    FILE* yuvFile = nullptr;
    bool  yuvEof  = false;   /* 真实 YUV 读到尾了 */

    /* 这一路是否已经彻底结束（帧生成完 + 编码器缓冲也冲刷完） */
    bool finished = false;

    /* 是否已经送过冲刷信号（send_frame(nullptr)） */
    bool flushed = false;
};

/* 把负错误码翻译成可读字符串 */
std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

/*
 * ===========================================================================
 * 【知识点 2】时间基换算 —— 封装里最容易错的一步
 * ===========================================================================
 * 包从编码器出来时，pts/dts 的单位是「编码器的时间基」；
 * 容器要的是「流自己的时间基」。两者通常不一样：
 *
 *   编码器（视频）  {1, 25}      一个 pts 单位 = 1 帧
 *   流（flv 视频）  {1, 1000}    一个 pts 单位 = 1 毫秒
 *   → 第 10 帧：pts=10 → 需要换算成 400（毫秒）
 *
 *   编码器（音频）  {1, 44100}   一个 pts 单位 = 1 个采样点
 *   流（flv 音频）  {1, 1000}    一个 pts 单位 = 1 毫秒
 *   → 采样点 22050   → 需要换算成 500（毫秒）
 *
 * 换算公式（FFmpeg 帮你算）：
 *     new_ts = av_rescale_q(old_ts, oldTimeBase, newTimeBase)
 *
 * 【两个必须记住的点】
 *
 *  (1) 必须在 avformat_write_header **之后**才去读 st->time_base！
 *      因为 muxer 会在写头的时候改写它。写头之前读到的可能不是最终值。
 *      这就是本函数每次调用都「现查 st->time_base」的原因。
 *
 *  (2) 视频有 B 帧时 dts != pts，两个都要换算，不能只换 pts。
 *      av_packet_rescale_ts 会一次把 pts/dts/duration 全换好，所以推荐用它。
 *
 * 【为什么 st->time_base 会变】
 *   每个容器有自己的时间刻度：flv 用毫秒（1/1000），
 *   mp4 默认用 1/90000（MPEG-TS 的传统），mkv 用 1/1000。
 *   你设的初值只是「建议」，muxer 有权改。所以：
 *       - 不要假设它等于你设的值
 *       - 不要把它硬编码进代码
 * ===========================================================================
 */
int writeFrame(AVFormatContext* fmtCtx, AVRational encTimeBase,
               OutputStream* ost, AVPacket* pkt)
{
    AVStream* st = ost->st;

    /* 编码器时间基 → 输出流时间基（现查 st->time_base，它可能已被 muxer 改写） */
    av_packet_rescale_ts(pkt, encTimeBase, st->time_base);

    /* 告诉 muxer 这包属于哪一路流。编码器不会填这个字段，必须由我们填。 */
    pkt->stream_index = st->index;

    /* pos = -1：位置信息对输出没意义，置 -1 避免 muxer 去做无谓的 seek */
    pkt->pos = -1;

    /*
     * ★★★ 踩坑记录：av_interleaved_write_frame 成功后会把 pkt 清空 ★★★
     * -----------------------------------------------------------------
     * 它不只是「写一包」，而是把包**移动（move）**进 muxer 的缓冲：
     *   - 内部的 AVBufferRef 引用被转走
     *   - 然后调用 av_packet_unref(pkt)，把调用方的 pkt 复位成：
     *         pkt->data = nullptr
     *         pkt->size = 0
     *         pkt->pts = pkt->dts = AV_NOPTS_VALUE
     *
     * 【后果】写完再打印 pkt，会看到 size=0 / pts=NOPTS，
     *   让人误以为「写进去的是空包」。本程序因此**在调用之前**先把
     *   要打印和统计的字段存到局部变量里（下面那几行）。
     *   这个坑在官方文档里只写了「会 unref」，没强调「字段会被清零」，
     *   所以调试验证时特别容易被误导。
     *
     * 顺带说明另外两件事：
     *   (1) 想写完还继续用这个包 → 先 av_packet_ref / av_packet_clone 拷一份
     *   (2) 后面的 av_write_trailer 才是真正 flush muxer 的内部缓冲
     *
     * 【av_interleaved_write_frame 到底做了什么】
     *   (1) 按 dts 排序：多路流混在一起时，保证写进文件的包按时间递增
     *       ★ 这就是「交织（interleave）」的核心
     *   (2) 内部缓冲：为了排序会把包暂存，等 dts 到了才真写出去
     *   (3) 时间戳单调性检查：dts 倒退会报 "non-monotonous DTS"
     *
     * 【另一个函数 av_write_frame】
     *   它不排序、不缓冲，直接写。只有当你已保证顺序时才用它。
     *   多路流场景**必须**用 interleaved 版本。
     *
     * 【失败意味着什么】
     *   推流场景下它代表网络坏了（连接重置、超时）。
     *   本函数只返回错误，收拾残局交给上层 —— pusher 里 FormatMuxer
     *   的做法是标记 m_broken，让推流线程重建 muxer（顺带完成重连）。
     * -----------------------------------------------------------------
     */

    /* ★ 必须在 write 之前保存：write 之后这些字段就被清零了 */
    const int     savedSize  = pkt->size;
    const int     savedPts   = pkt->pts;
    const int     savedDts   = pkt->dts;
    const int     savedFlags = pkt->flags;
    const int     savedIndex = pkt->stream_index;

    const int ret = av_interleaved_write_frame(fmtCtx, pkt);
    if (ret < 0) {
        std::fprintf(stderr, "av_interleaved_write_frame 失败: %s\n", errStr(ret).c_str());
        return ret;
    }

    ost->packetsWritten++;

    /*
     * 【知识点 2.1】打印时间戳：为什么不能用 av_ts2str / av_ts2timestr
     * -----------------------------------------------------------------
     * libavutil/timestamp.h 提供了两个看起来很顺手的宏：
     *       av_ts2str(ts)          // 转成十进制字符串
     *       av_ts2timestr(ts, tb)  // 转成「123.456」这样的秒表示
     *   但它们的实现依赖 **C99 复合字面量**：
     *       #define av_ts2str(ts) av_ts_make_string((char[64]){0}, ts)
     *                                   ^^^^^^^^^^^^^^^^ 临时数组
     *   C++ 没有复合字面量，GCC 会报：
     *       error: taking address of temporary array
     *
     *   所以 C++ 里要直接调用底层的两个真函数，自己提供缓冲区：
     *       char *av_ts_make_string(char *buf, int64_t ts);
     *       char *av_ts_make_time_string(char *buf, int64_t ts, const AVRational *tb);
     *
     *   顺带一提：av_ts_make_string 用的是**静态内部缓冲**，
     *   同一行里调两次会互相覆盖，所以下面分开取到各自局部缓冲里再打印。
     *   这个坑在任何语言里都存在（C 也一样）。
     * -----------------------------------------------------------------
     */
    char tsBuf[AV_TS_MAX_STRING_SIZE]   = { 0 };
    char dtsBuf[AV_TS_MAX_STRING_SIZE]  = { 0 };
    char tsTime[AV_TS_MAX_STRING_SIZE]  = { 0 };
    av_ts_make_string(tsBuf,  savedPts);
    av_ts_make_string(dtsBuf, savedDts);
    av_ts_make_time_string(tsTime, savedPts, &st->time_base);

    /*
     * 打印限流：默认只打印前 14 个包，之后只看「视频关键帧」。
     * 为什么音频关键帧（AV_PKT_FLAG_KEY）不再打印？
     *   因为 AAC 每一帧都是关键帧，全打出来就是 217 行噪声。
     *   而视频关键帧（I 帧）间隔 = gop_size，正好能看出 GOP 结构。
     * 需要看每一个包时加 -v。
     */
    static constexpr int kLogHeadPackets = 14;
    const bool isKey       = (savedFlags & AV_PKT_FLAG_KEY) != 0;
    const bool isVideoKey  = isKey && savedIndex == 0;
    if (gVerbosePackets || gLoggedPackets < kLogHeadPackets || isVideoKey) {
        std::printf("  [流%d] pts=%-6s(%ss) dts=%-6s size=%-6d%s\n",
                    savedIndex, tsBuf, tsTime, dtsBuf, savedSize,
                    isKey ? "  KEY" : "");
    } else if (gLoggedPackets == kLogHeadPackets) {
        std::printf("  ...（后续只打印视频关键帧；加 -v 可显示每一个包）\n");
    }
    ++gLoggedPackets;
    return 0;
}

/*
 * ===========================================================================
 * 【知识点 4】avcodec_encode 的新旧 API 差异（本项目相对原示例的最大改动）
 * ===========================================================================
 * 原 muxing.c 用的是 **旧 API**：
 *       avcodec_encode_video2(cc, &pkt, frame, &got_packet);
 *       avcodec_encode_audio2(cc, &pkt, frame, &got_packet);
 *   这两个函数在 FFmpeg 5.0 前后被**彻底删除**，FFmpeg 8.1 里已经没有。
 *
 * 新 API 是 send/receive 两个方向（和 08-02 的视频编码完全一致）：
 *       avcodec_send_frame(cc, frame);
 *       while (1) {
 *           ret = avcodec_receive_packet(cc, pkt);
 *           if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
 *           if (ret < 0) 真错误;
 *           ...写包...
 *       }
 *
 * 【新 API 相比旧 API 的三个好处】
 *   (1) 一次 send 可能产出**多个**包（旧 API 一次只给一个），
 *       所以必须循环 receive —— 这正好匹配「交织写」的需要。
 *   (2) 语义更清楚：flush 就是 send(nullptr)，不再需要 got_packet 出参。
 *   (3) 音视频两条路的调用方式**完全一样**，可以写成一个 helper 共用。
 *
 * 【本函数把「编码 + 写包」合成一步】
 *   这是教学工程的做法（一个函数看懂全流程）。
 *   pusher 里拆成了两层：encoder 的 drainToSink() 只负责收包并回调，
 *   muxer 的 writePacket() 负责换算时间基并写出去。
 *   分层的好处：编码线程和推流线程可以解耦（中间放有界队列）。
 * ===========================================================================
 */
/*
 * 编码一帧 + 把产出的所有包交织写出去。
 *
 * 【返回值语义】本函数返回「这一路是否还需要继续调用」：
 *      true  = 还没完（可能刚送进去一帧，也可能还有缓冲的包要吐）
 *      false = 已经彻底结束（输入没了，且编码器也冲刷干净了）
 *
 * 这正是「什么时候该结束」的正确判据：**不能只看输入帧有没有用完**，
 * 编码器内部还压着帧（look-ahead / B 帧 / 线程池），必须继续 receive
 * 直到它返回 AVERROR_EOF 才算真的结束。原 C 示例用
 *       return (frame || got_packet) ? 0 : 1;
 * 表达的是同一件事，本函数用 inFrame/outPacket 两个布尔量把它写清楚。
 */
bool encodeAndWrite(AVFormatContext* fmtCtx, OutputStream* ost, AVFrame* frame,
                    AVPacket* pkt)
{
    AVCodecContext* cc = ost->enc;
    bool inFrame  = (frame != nullptr);
    bool outPacket = false;

    /*
     * ★★★ 踩坑记录：send_frame(nullptr) 之后不能再 send 任何帧 ★★★
     * -----------------------------------------------------------------
     * 一旦送过冲刷信号（frame == nullptr），编码器就进入 EOF 状态，
     * 之后再 avcodec_send_frame(cc, 非空帧) 会返回 AVERROR_EOF，
     * 日志里表现为：
     *       avcodec_send_frame 失败: End of file
     *
     * 为什么会发生？
     *   本程序的交织循环靠「返回值」判断某一路是否结束，
     *   而返回 false 之前会先送一次 nullptr 去冲刷。
     *   如果调用方没记住「已经冲刷过了」，下一次还会再走一遍
     *   「生成帧 → 送帧」，于是撞上 EOF。
     *
     * 解决：用 flushed 标志把状态记下来，冲刷后只能继续 receive，
     *       不能再 send。这也是 pusher 里 drainToSink + flush 要区分的原因。
     * -----------------------------------------------------------------
     */
    if (ost->flushed) {
        /* 已经冲刷过：只收包，不再送 */
        frame = nullptr;
        inFrame = false;
    } else if (!frame) {
        ost->flushed = true;      /* 本次就是冲刷 */
    }

    int ret = avcodec_send_frame(cc, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        std::fprintf(stderr, "avcodec_send_frame 失败: %s\n", errStr(ret).c_str());
        ost->finished = true;
        return false;
    }

    while (true) {
        ret = avcodec_receive_packet(cc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;                 /* 正常结束，不是错误 */
        if (ret < 0) {
            std::fprintf(stderr, "avcodec_receive_packet 失败: %s\n", errStr(ret).c_str());
            ost->finished = true;
            return false;
        }

        /*
         * 防御：空包直接丢弃。
         * 正常情况下编码器不会产出 size==0 的包，但加上这道判断有两个好处：
         *   1. 万一上游有 bug（比如误把未初始化的 pkt 当结果用），
         *      不会把垃圾包写进容器（flv/mp4 里出现空 tag 会让播放器卡住）
         *   2. 顺便暴露问题：正常运行时这行永远不该触发
         */
        if (pkt->size <= 0) {
            std::fprintf(stderr, "  警告: 收到空包 (size=%d pts=%" PRId64 ")，已丢弃\n",
                         pkt->size, pkt->pts);
            continue;
        }
        outPacket = true;

        /* ★ 关键：换算出容器要的时间戳，然后交织写出去 */
        if (writeFrame(fmtCtx, cc->time_base, ost, pkt) < 0) {
            ost->finished = true;
            return false;
        }
        /* 注意：av_interleaved_write_frame 已经接管并 unref 了 pkt，
         * 所以这里不需要（也不应该）再 av_packet_unref(pkt)。 */
    }

    /* 还有输入帧要送，或者刚才吐出了包（可能后面还有）→ 继续 */
    return inFrame || outPacket;
}

/*
 * ===========================================================================
 * 【知识点 5】avformat_new_stream —— 建一路流
 * ===========================================================================
 * 第二个参数传 nullptr：让 avformat 自己创建 AVCodecContext。
 * （旧代码会传一个 codec 进去，现在推荐传 nullptr，
 *   然后自己用 avcodec_alloc_context3 建，控制权更清楚。）
 *
 * 【st->id 那行是坑】
 *   原代码写 ost->st->id = oc->nb_streams - 1;
 *   因为 avformat_new_stream 内部已经把 nb_streams 加了 1，
 *   所以这里 -1 才是「本流的序号」。
 *   ★ 但要注意：st->id 和 st->index **不是一回事**！
 *       st->index 是流的数组下标（0,1,2…），muxer 用它定位流；
 *       st->id    只是给容器用的一个标签（mp4 的 track ID），可以不设。
 *   真正决定「这包属于哪路流」的是 pkt->stream_index = st->index。
 * ===========================================================================
 */
OutputStream* addStream(AVFormatContext* oc, const AVCodec* codec, AVCodecID codecId)
{
    if (!codec) {
        std::fprintf(stderr, "找不到编码器 %s\n", avcodec_get_name(codecId));
        return nullptr;
    }

    auto* ost = new OutputStream();

    ost->st = avformat_new_stream(oc, nullptr);
    if (!ost->st) {
        std::fprintf(stderr, "avformat_new_stream 失败\n");
        delete ost;
        return nullptr;
    }
    ost->st->id = static_cast<int>(oc->nb_streams) - 1;

    ost->enc = avcodec_alloc_context3(codec);
    if (!ost->enc) {
        std::fprintf(stderr, "avcodec_alloc_context3 失败\n");
        delete ost;
        return nullptr;
    }

    ost->enc->codec_id   = codecId;      /* 明确指定用哪个编码格式 */
    ost->enc->codec_type = codec->type;  /* 音频还是视频 */

    /*
     * 【知识点 6】GLOBAL_HEADER 该不该设 —— 由容器说了算
     * -----------------------------------------------------------------
     *       if (oc->oformat->flags & AVFMT_GLOBALHEADER)
     *           cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
     *
     * 这是官方 muxing.c 的标准写法，含义是：
     *   「如果这个容器要求把 SPS/PPS / AudioSpecificConfig 独立存放
     *     （flv/mp4/mkv 都要求），就把它们从码流里挪到 cc->extradata」
     *
     * flv / mp4 / mkv 都带 AVFMT_GLOBALHEADER，所以实际都会设上。
     * 这意味着后面 avcodec_parameters_from_context 会把 extradata
     * 一起复制进 st->codecpar，muxer 写头时再把它塞进容器。
     *
     * ★ 对比 08-02：那里输出裸 H.264（无容器），所以**故意不设**，
     *   让每个 I 帧自带 SPS/PPS，文件才能独立播放。
     *   这就是「容器决定放哪儿」的完整逻辑。
     * -----------------------------------------------------------------
     */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        ost->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return ost;
}

/*
 * ===========================================================================
 * 【知识点 7】音频参数：声道布局用新 API
 * ===========================================================================
 * 原代码（4.2.1 时代）用的是位掩码：
 *       cc->channel_layout = AV_CH_LAYOUT_STEREO;      // uint64_t
 *       cc->channels = av_get_channel_layout_nb_channels(cc->channel_layout);
 *
 * FFmpeg 8.1 里这两个字段已经删除，改用 AVChannelLayout 结构体：
 *       av_channel_layout_default(&cc->ch_layout, 2);          // 按声道数取标准布局
 *   或  av_channel_layout_from_mask(&cc->ch_layout, AV_CH_LAYOUT_STEREO);
 *
 * 【三条铁律】（08-01 里详细讲过，这里再强调一次因为 muxing 里到处在用）
 *   (1) 比较用 av_channel_layout_compare()，不能用 == 也不能 memcmp
 *   (2) 复制用 av_channel_layout_copy()，不能按值赋值（CUSTOM 布局会 double free）
 *   (3) 栈上声明的布局用过 copy/from_* 之后要 av_channel_layout_uninit()
 *
 * 【采样格式也要问编码器】
 *   AAC 只吃 AV_SAMPLE_FMT_FLTP。这里用 avcodec_get_supported_config()
 *   取编码器声明的第一个格式作为默认值（比硬编码更稳）。
 * ===========================================================================
 */
void openAudio(AVFormatContext* oc, const AVCodec* codec, OutputStream* ost)
{
    AVCodecContext* cc = ost->enc;

    /* ---- 采样格式：问编码器要 ---- */
    const void* cfg = nullptr;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0, &cfg, nullptr) == 0 && cfg) {
        cc->sample_fmt = *static_cast<const enum AVSampleFormat*>(cfg);
    } else {
        cc->sample_fmt = AV_SAMPLE_FMT_FLTP;   /* 兜底 */
    }

    /* ---- 采样率：优先 44100，编码器不支持就用它声明的第一个 ---- */
    cc->sample_rate = 44100;
    {
        const void* rates = nullptr;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE,
                                         0, &rates, nullptr) == 0 && rates) {
            const int* p = static_cast<const int*>(rates);
            bool has44100 = false;
            for (; *p != 0; ++p)
                if (*p == 44100) has44100 = true;
            if (!has44100)
                cc->sample_rate = *static_cast<const int*>(rates);
            /* 打印出来便于对照：AAC 支持 96000/88200/64000/48000/44100/... */
            std::printf("  编码器 %s 支持的采样率: ", codec->name);
            for (p = static_cast<const int*>(rates); *p != 0; ++p)
                std::printf("%d ", *p);
            std::printf("\n");
        }
    }

    cc->bit_rate = 64000;                                   /* 音频码率 */

    /* ---- 声道布局：新 API ---- */
    av_channel_layout_default(&cc->ch_layout, 2);           /* 2 → stereo */

    /*
     * 【知识点 8】音频流的 time_base 为什么用 {1, sample_rate}
     * -----------------------------------------------------------------
     * 音频的时间戳天然就是「第几个采样点」，所以最自然的时间基就是
     *       1 个 pts 单位 = 1 个采样点 = 1/sample_rate 秒
     *
     * 于是：pts = 采样点序号，换算时间 = pts / sample_rate 秒。
     *
     * 另一种常见做法是用固定时基（如 {1,1000} 毫秒）：
     *       cc->time_base = {1, sample_rate};   ← 本项目
     *   对比视频：time_base = {1, fps}，pts = 帧号。
     *   两者的「自然单位」不同，这正是需要时间基换算的根本原因。
     *
     * ★ 注意：这里设的是**编码器**的时间基。
     *   流的时间基（st->time_base）在写头时会被 muxer 改成容器自己的规矩。
     * -----------------------------------------------------------------
     */
    ost->st->time_base = AVRational{ 1, cc->sample_rate };
    cc->time_base      = ost->st->time_base;

    int ret = avcodec_open2(cc, codec, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "打开音频编码器失败: %s\n", errStr(ret).c_str());
        std::exit(1);
    }

    /* ---- 生成扫频正弦波用的相位参数（原示例的算法，保留） ---- */
    ost->t     = 0.0;
    ost->tincr = 2 * M_PI * 110.0 / cc->sample_rate;
    ost->tincr2 = 2 * M_PI * 110.0 / cc->sample_rate / cc->sample_rate;

    const int nbSamples = cc->frame_size ? cc->frame_size : 1024;
    std::printf("  音频 frame_size=%d（每帧采样点数，AAC=1024）\n", nbSamples);

    /*
     * 【知识点 9】两个音频帧的分工
     * -----------------------------------------------------------------
     *   tmpFrame : 「信号源」帧，格式是我们方便生成的（s16 交错）
     *   frame    : 「编码器要的」帧，格式由编码器决定（fltp 分平面）
     *
     * 中间靠 swr 转换。这就是「设备给 s16、AAC 要 fltp」的标准处理方式，
     * 也是 pusher 里 AacAudioEncoder 必须做的事。
     * -----------------------------------------------------------------
     */
    ost->frame = av_frame_alloc();
    ost->frame->format = cc->sample_fmt;
    av_channel_layout_copy(&ost->frame->ch_layout, &cc->ch_layout);
    ost->frame->sample_rate = cc->sample_rate;
    ost->frame->nb_samples  = nbSamples;
    if (av_frame_get_buffer(ost->frame, 0) < 0) {
        std::fprintf(stderr, "音频 frame buffer 分配失败\n");
        std::exit(1);
    }

    ost->tmpFrame = av_frame_alloc();
    ost->tmpFrame->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_copy(&ost->tmpFrame->ch_layout, &cc->ch_layout);
    ost->tmpFrame->sample_rate = cc->sample_rate;
    ost->tmpFrame->nb_samples  = nbSamples;
    if (av_frame_get_buffer(ost->tmpFrame, 0) < 0) {
        std::fprintf(stderr, "音频 tmpFrame buffer 分配失败\n");
        std::exit(1);
    }

    /*
     * 【知识点 10】把编码参数交给容器 —— 这一步很容易漏，漏了必坏
     * -----------------------------------------------------------------
     *       avcodec_parameters_from_context(st->codecpar, cc);
     *
     * 它拷贝什么？
     *   视频：codec_id / 宽高 / pix_fmt / **extradata(SPS/PPS)** / framerate
     *   音频：codec_id / 采样率 / 声道布局 / sample_fmt / **extradata(ASC)**
     *
     * 为什么必须做？
     *   容器头里要写这些信息，否则播放器拿到文件不知道该怎么解。
     *   尤其是 extradata —— 设了 GLOBAL_HEADER 之后 SPS/PPS 只在
     *   cc->extradata 里，muxer 就靠 st->codecpar->extradata 把它写进容器。
     *   漏了这一步，flv/mp4 会生成「无法解码」或直接报错的文件。
     *
     * ★ 注意是 **copy**，不是接管：st->codecpar 有自己的 extradata 副本，
     *   之后释放 cc 不会影响 st。
     * -----------------------------------------------------------------
     */
    ret = avcodec_parameters_from_context(ost->st->codecpar, cc);
    if (ret < 0) {
        std::fprintf(stderr, "avcodec_parameters_from_context 失败: %s\n", errStr(ret).c_str());
        std::exit(1);
    }
    std::printf("  音频流已配置: %d Hz, %d 声道, %s, extradata=%d 字节\n",
                cc->sample_rate, cc->ch_layout.nb_channels,
                av_get_sample_fmt_name(cc->sample_fmt), ost->st->codecpar->extradata_size);

    /* ---- 建重采样器：s16 交错 → 编码器要的格式 ---- */
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, 2);
    ret = swr_alloc_set_opts2(&ost->swr,
                              &cc->ch_layout, cc->sample_fmt, cc->sample_rate,
                              &inLayout,      AV_SAMPLE_FMT_S16, cc->sample_rate,
                              0, nullptr);
    av_channel_layout_uninit(&inLayout);    /* ★ 栈上布局用完必须释放 */
    if (ret < 0 || !ost->swr) {
        std::fprintf(stderr, "swr_alloc_set_opts2 失败\n");
        std::exit(1);
    }
    if (swr_init(ost->swr) < 0) {
        std::fprintf(stderr, "swr_init 失败\n");
        std::exit(1);
    }
}

/* 用扫频正弦波填满一帧 s16 交错 PCM（原示例的算法） */
void fillSine(AVFrame* frame, OutputStream* ost)
{
    auto* q = reinterpret_cast<int16_t*>(frame->data[0]);
    const int nbCh = frame->ch_layout.nb_channels;
    for (int i = 0; i < frame->nb_samples; ++i) {
        const auto v = static_cast<int16_t>(std::sin(ost->t) * 10000);
        for (int ch = 0; ch < nbCh; ++ch)
            *q++ = v;
        ost->t     += ost->tincr;
        ost->tincr += ost->tincr2;
    }
}

/*
 * 生成一帧音频。
 * 【时间戳规则】pts = 采样点序号，每次 += nb_samples。
 *   AAC 一帧 1024 个采样点，44100Hz → 每帧 23.2 ms
 */
AVFrame* getAudioFrame(OutputStream* ost)
{
    /*
     * 结束条件：用 av_compare_ts 比较「已生成到的时间」和「总时长」。
     *   av_compare_ts(a, tbA, b, tbB) 返回 a、b 换算到同一时间基后的比较结果。
     *   这里 {1,1} 表示「以秒为单位」，等价于比较 a*tbA 秒 和 b*tbB 秒。
     *
     * 这样写的好处：不用自己算「该生成多少帧」，也不用关心单位换算，
     * 换采样率/换帧率代码都不用改。
     */
    if (av_compare_ts(ost->nextPts, ost->enc->time_base,
                      static_cast<int64_t>(gDuration), AVRational{ 1, 1 }) >= 0)
        return nullptr;

    fillSine(ost->tmpFrame, ost);

    /* 源帧的 pts 用采样点数计（时间基 {1, sample_rate}） */
    ost->tmpFrame->pts = ost->nextPts;
    ost->nextPts += ost->tmpFrame->nb_samples;
    return ost->tmpFrame;
}

/* 生成一帧音频 → 经 swr 转换 → 编码 → 写包。返回 false 表示这一路已结束 */
bool writeAudioFrame(AVFormatContext* oc, OutputStream* ost, AVPacket* pkt)
{
    AVCodecContext* cc = ost->enc;
    AVFrame* src = getAudioFrame(ost);

    if (src) {
        /*
         * 【知识点 11】av_frame_make_writable —— 每帧写之前必须调
         *   编码器可能持有上一帧的引用（引用计数共享 buffer）。
         *   直接覆写会破坏编码器正在用的数据。
         *   这个函数检查引用计数：独占就直接返回，共享就重新分配并拷贝。
         */
        if (av_frame_make_writable(ost->frame) < 0)
            std::exit(1);

        /*
         * 【知识点 12】swr_convert 的返回值和容量
         *   out_count 传的是「目标缓冲区能装多少采样点」（这里 = nb_samples）。
         *   返回值是**实际写出的采样点数**，不一定等于输入采样点数：
         *     - 采样率相同时，通常相等
         *     - 采样率不同时，swr 内部 FIFO 会缓存尾巴，返回值会变化
         *   所以必须用返回值更新 frame->nb_samples，不能假设。
         */
        /*
         * 【C++ 类型坑】swr_convert 的第 4 个参数是 `const uint8_t**`，
         *   而 src->data 是 `uint8_t*[8]`。用数组接一层就天然匹配，
         *   比写 `const_cast<const uint8_t**>(...)` 干净且安全
         *   （const_cast 根本不能把 `uint8_t**` 转成 `const uint8_t**`）。
         *   这里输入是 packed s16，所以只有 1 个平面。
         */
        const uint8_t* inData[1] = { src->data[0] };
        const int outSamples = swr_convert(ost->swr,
                                           ost->frame->data, ost->frame->nb_samples,
                                           inData, src->nb_samples);
        if (outSamples < 0) {
            std::fprintf(stderr, "swr_convert 失败\n");
            std::exit(1);
        }
        if (outSamples == 0)
            return true;      /* 全被 swr 缓存了，这一轮不产出，但还没结束 */

        ost->frame->nb_samples = outSamples;

        /*
         * 用「累计采样数」算 pts 并换算到编码器时间基。
         * 这里 samplesCount 是「已经处理过的采样点总数」，
         * 换算 {1,sample_rate} → cc->time_base。
         * 因为两者通常相同，这次换算其实是恒等变换；
         * 但如果以后改成固定时基（如 1/1000 毫秒），这行就是必须的。
         */
        ost->frame->pts = av_rescale_q(ost->samplesCount,
                                       AVRational{ 1, cc->sample_rate },
                                       cc->time_base);
        ost->samplesCount += ost->frame->nb_samples;
    }

    /* src 为 nullptr 时表示冲刷（把编码器内部残留全部吐出） */
    const bool more = encodeAndWrite(oc, ost, src ? ost->frame : nullptr, pkt);
    if (!more)
        ost->finished = true;
    return more;
}

/*
 * ===========================================================================
 * 【知识点 13】视频参数
 * ===========================================================================
 * 逐个说明（对比 08-02 可以看清哪些是「容器要求的」）：
 *
 *   width/height     分辨率。原示例用 352x288（CIF），本程序默认同。
 *                    用真实 YUV 时可命令行指定。
 *   max_b_frames     1：原示例的值。B 帧能提升压缩率，但让 dts<pts，
 *                    封装时要按 dts 排序（见 av_interleaved_write_frame）。
 *                    ★ 推流场景应该设 0。
 *   st->time_base    {1, fps}，和编码器一致 —— 一帧一个 pts 单位。
 *   cc->gop_size     fps → 每秒一个 I 帧。
 *   cc->pix_fmt      编码器要的像素格式。★ 必须从编码器问，
 *                    不能假设它吃 yuv420p（虽然 libx264 吃）。
 * ===========================================================================
 */
void openVideo(AVFormatContext* oc, OutputStream* ost,
               const std::string& yuvFile, int width, int height, int fps)
{
    AVCodecContext* cc = ost->enc;

    cc->width         = width;
    cc->height        = height;
    cc->bit_rate      = 4000000;                 /* 4 Mbps */
    cc->max_b_frames  = 1;                       /* 推流应改 0 */
    cc->gop_size      = fps;
    cc->time_base     = AVRational{ 1, fps };
    cc->framerate     = AVRational{ fps, 1 };

    /*
     * 问编码器第一个支持的像素格式（比硬编码 AV_PIX_FMT_YUV420P 更稳）。
     * 例如换成硬件编码器时它可能要 nv12。
     */
    {
        const void* cfg = nullptr;
        if (avcodec_get_supported_config(nullptr, ost->enc->codec,
                                         AV_CODEC_CONFIG_PIX_FORMAT,
                                         0, &cfg, nullptr) == 0 && cfg) {
            cc->pix_fmt = *static_cast<const enum AVPixelFormat*>(cfg);
            std::printf("  编码器 %s 首选像素格式: %s\n",
                        cc->codec->name, av_get_pix_fmt_name(cc->pix_fmt));
        } else {
            cc->pix_fmt = AV_PIX_FMT_YUV420P;
        }
    }

    /* x264 的档位（如果用的是 libx264；其它编码器会返回失败，打印警告即可） */
    if (cc->codec->id == AV_CODEC_ID_H264) {
        const int r = av_opt_set(cc->priv_data, "preset", "veryfast", 0);
        if (r < 0)
            std::fprintf(stderr, "  警告: 设置 preset 失败 (%s)\n", errStr(r).c_str());
    }

    ost->st->time_base = cc->time_base;

    int ret = avcodec_open2(cc, cc->codec, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "打开视频编码器失败: %s\n", errStr(ret).c_str());
        std::exit(1);
    }

    /* 送给编码器的帧（编码器要的格式） */
    ost->frame = av_frame_alloc();
    ost->frame->format = cc->pix_fmt;
    ost->frame->width  = cc->width;
    ost->frame->height = cc->height;
    if (av_frame_get_buffer(ost->frame, 0) < 0) {
        std::fprintf(stderr, "视频 frame buffer 分配失败\n");
        std::exit(1);
    }

    /*
     * 【知识点 14】tmpFrame + sws 什么时候才需要
     *   我们生成的测试图是 yuv420p。如果编码器也要 yuv420p（libx264 就是），
     *   就不需要中间转换，直接把图生成到 ost->frame 里。
     *   只有当编码器要别的格式（nv12/yuv444p…）时，才需要：
     *       生成 yuv420p 到 tmpFrame → sws_scale → frame
     *   这是「格式不匹配就加一层转换」的标准套路，和音频侧的 swr 完全对称。
     */
    if (cc->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmpFrame = av_frame_alloc();
        ost->tmpFrame->format = AV_PIX_FMT_YUV420P;
        ost->tmpFrame->width  = cc->width;
        ost->tmpFrame->height = cc->height;
        if (av_frame_get_buffer(ost->tmpFrame, 0) < 0) {
            std::fprintf(stderr, "视频 tmpFrame buffer 分配失败\n");
            std::exit(1);
        }
        ost->sws = sws_getContext(cc->width, cc->height, AV_PIX_FMT_YUV420P,
                                  cc->width, cc->height, cc->pix_fmt,
                                  kScaleFlags, nullptr, nullptr, nullptr);
        if (!ost->sws) {
            std::fprintf(stderr, "sws_getContext 失败\n");
            std::exit(1);
        }
        std::printf("  需要色彩空间转换: yuv420p → %s\n", av_get_pix_fmt_name(cc->pix_fmt));
    } else {
        std::printf("  编码器格式就是 yuv420p，无需 sws 转换\n");
    }

    /* 真实 YUV 输入文件（可选） */
    if (!yuvFile.empty()) {
        ost->yuvFile = std::fopen(yuvFile.c_str(), "rb");
        if (!ost->yuvFile) {
            std::fprintf(stderr, "打不开 YUV 输入 %s\n", yuvFile.c_str());
            std::exit(1);
        }
        std::printf("  视频源: 真实 YUV 文件 %s\n", yuvFile.c_str());
    } else {
        std::printf("  视频源: 合成测试图（原示例的算法）\n");
    }

    /* 把编码参数交给容器（含 SPS/PPS extradata） */
    ret = avcodec_parameters_from_context(ost->st->codecpar, cc);
    if (ret < 0) {
        std::fprintf(stderr, "avcodec_parameters_from_context 失败: %s\n", errStr(ret).c_str());
        std::exit(1);
    }
    std::printf("  视频流已配置: %dx%d %s fps=%d, extradata=%d 字节（SPS/PPS）\n",
                cc->width, cc->height, av_get_pix_fmt_name(cc->pix_fmt), fps,
                ost->st->codecpar->extradata_size);
}

/*
 * 生成一帧测试图（原示例的算法：随帧号变化的渐变图案）
 * 【知识点 15】为什么用 linesize 而不是 width 索引
 *   行与行之间的真实间距是 linesize[i]，它因为对齐要求可能大于 width。
 *   写成 data[0][y * width + x] 在 width 不是对齐倍数时会出现斜纹。
 *   这是视频代码最经典的 bug，务必养成用 linesize 的习惯。
 */
void fillTestImage(AVFrame* pict, int frameIndex)
{
    const int w = pict->width, h = pict->height;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            pict->data[0][y * pict->linesize[0] + x] =
                static_cast<uint8_t>(x + y + frameIndex * 3);

    for (int y = 0; y < h / 2; ++y) {
        for (int x = 0; x < w / 2; ++x) {
            pict->data[1][y * pict->linesize[1] + x] =
                static_cast<uint8_t>(128 + y + frameIndex * 2);
            pict->data[2][y * pict->linesize[2] + x] =
                static_cast<uint8_t>(64 + x + frameIndex * 5);
        }
    }
}

AVFrame* getVideoFrame(OutputStream* ost)
{
    AVCodecContext* cc = ost->enc;

    /* 真实 YUV 已经读到尾 → 返回 nullptr 让上层进入冲刷阶段 */
    if (ost->yuvEof)
        return nullptr;

    /* 到时见好就收（和音频同一套判据，保证两路时长一致） */
    if (av_compare_ts(ost->nextPts, cc->time_base,
                      static_cast<int64_t>(gDuration), AVRational{ 1, 1 }) >= 0)
        return nullptr;

    if (av_frame_make_writable(ost->frame) < 0)
        std::exit(1);

    if (ost->yuvFile) {
        /*
         * ---- 真实 YUV 输入 ----
         *
         * 【知识点 16.1】★★ 读进 frame 的正确做法 —— 这里踩过一个崩溃 ★★
         * -----------------------------------------------------------------
         * 错误写法（本文件第一版就是这样的，实测直接崩溃 0xC0000005）：
         *
         *     uint8_t* buf = malloc(need);
         *     fread(buf, 1, need, f);
         *     av_image_fill_arrays(frame->data, frame->linesize, buf, fmt, w, h, 1);
         *     free(buf);                       // ← 崩溃根源
         *
         * 问题在于 av_image_fill_arrays 会**重新计算 linesize** 并改写
         * frame->data[] / frame->linesize[]。而 frame 的 data 本来指向
         * av_frame_get_buffer 分配的、**引用计数管理**的缓冲区
         * （frame->buf[0]）。被改写之后：
         *   - frame->buf[0] 还指着原来那块内部缓冲区（没人释放，泄漏）
         *   - frame->data[]  却指着我们 malloc 的那块
         *   - free(buf) 之后 frame->data[] 就成了野指针
         *   - 编码器一读就崩溃
         *
         * 正确做法：**把数据拷进 frame 自己的缓冲区**，
         * 绝不改动 frame->data[] 的指向。
         *
         * 两种实现：
         *   (a) 逐行拷贝，正确处理 linesize（本程序用这个，最直观）
         *          for (y) memcpy(frame->data[p] + y*frame->linesize[p],
         *                         buf + offset + y*rowBytes, rowBytes);
         *       注意 Y/U/V 三个平面各有自己的 linesize 和行数（U/V 是 w/2 × h/2）
         *
         *   (b) 用 av_image_copy 一次性拷三个平面（等价，更省事）：
         *          av_image_copy(frame->data, frame->linesize,
         *                        (const uint8_t**)srcData, srcLinesize,
         *                        (AVPixelFormat)frame->format, w, h);
         *
         * 另外性能上也不该每帧 malloc/free：真实项目（pusher）复用一个常驻
         * 缓冲，只在第一帧分配。本程序为了代码直白就每帧分配一次，
         * 但拷贝方向是对的。
         * -----------------------------------------------------------------
         */
        const int need = av_image_get_buffer_size(
            static_cast<AVPixelFormat>(ost->frame->format), cc->width, cc->height, 1);
        auto* buf = static_cast<uint8_t*>(std::malloc(need));
        if (!buf)
            std::exit(1);
        const size_t got = std::fread(buf, 1, need, ost->yuvFile);
        if (got < static_cast<size_t>(need)) {
            std::free(buf);
            std::printf("  YUV 文件读完（或末尾不完整），视频结束\n");
            ost->yuvEof = true;
            return nullptr;
        }

        /* 按平面逐行拷贝到 frame 自己的缓冲区（linesize 可能大于行有效字节数） */
        const int planeW[3] = { cc->width, cc->width / 2, cc->width / 2 };
        const int planeH[3] = { cc->height, cc->height / 2, cc->height / 2 };
        size_t srcOff = 0;
        for (int p = 0; p < 3 && ost->frame->data[p]; ++p) {
            const int rowBytes = planeW[p];
            for (int y = 0; y < planeH[p]; ++y) {
                std::memcpy(ost->frame->data[p] + static_cast<size_t>(y) * ost->frame->linesize[p],
                            buf + srcOff + static_cast<size_t>(y) * rowBytes,
                            static_cast<size_t>(rowBytes));
            }
            srcOff += static_cast<size_t>(rowBytes) * planeH[p];
        }
        std::free(buf);
    } else if (ost->tmpFrame) {
        /* ---- 需要 sws 转换 ---- */
        fillTestImage(ost->tmpFrame, static_cast<int>(ost->nextPts));
        sws_scale(ost->sws,
                  const_cast<const uint8_t* const*>(ost->tmpFrame->data),
                  ost->tmpFrame->linesize, 0, cc->height,
                  ost->frame->data, ost->frame->linesize);
    } else {
        /* ---- 直接生成到编码器帧 ---- */
        fillTestImage(ost->frame, static_cast<int>(ost->nextPts));
    }

    /*
     * 【知识点 16】视频 pts 就是帧号
     *   因为 cc->time_base = {1, fps}，所以每帧 +1。
     *   第 n 帧 = n/fps 秒。
     *
     * ★ 这里和 08-02 一样，特意没写成 pts += 40：
     *   原 08-02 代码把 time_base 当毫秒用是个 bug。
     *   判断依据永远是「time_base 是什么」，不是「一帧多少毫秒」。
     */
    ost->frame->pts = ost->nextPts++;
    return ost->frame;
}

/* 生成并编码一帧视频。返回 false 表示这一路已结束 */
bool writeVideoFrame(AVFormatContext* oc, OutputStream* ost, AVPacket* pkt)
{
    AVFrame* frame = getVideoFrame(ost);
    const bool more = encodeAndWrite(oc, ost, frame, pkt);
    if (!more)
        ost->finished = true;
    return more;
}

void closeStream(OutputStream* ost)
{
    if (!ost)
        return;
    if (ost->yuvFile)
        std::fclose(ost->yuvFile);
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmpFrame);
    sws_freeContext(ost->sws);          /* sws_freeContext 接受 nullptr，安全 */
    swr_free(&ost->swr);
    delete ost;
}

void usage(const char* prog)
{
    std::fprintf(stderr,
        "用法: %s <out.flv|out.mp4|...> [format] [yuvFile] [w] [h] [fps] [duration] [-v]\n"
        "  format   留空则按扩展名猜；也可显式给 flv / mp4 / mkv / matroska\n"
        "  yuvFile  可选：用真实 YUV 当视频源（不给则用合成测试图）\n"
        "  w h fps  默认 352 288 25\n"
        "  duration 默认 5 秒\n"
        "  -v       打印每一个包的时间戳（默认只打印前 14 个 + 关键帧）\n"
        "例: %s out.flv\n"
        "    %s out.mp4\n"
        "    %s out.flv flv ../08-02-encode_video/yuv420p_1280x720.yuv 1280 720 25 5\n",
        prog, prog, prog, prog);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /*
     * 解析参数。
     * 注意：可选的位置参数如果要跳过，必须传一个占位符（本程序约定用 "-"），
     * 不能传空字符串 —— atoi("")/atof("") 会返回 0，导致分辨率变成 0x0。
     * 这是用「位置参数」而不是「命名参数」的固有缺点；
     * 真实工程（pusher）用 "键=值" 或命令行解析库就没这个问题。
     */
    const std::string outFile  = argv[1];
    const std::string format   = (argc > 2 && std::strcmp(argv[2], "-") != 0) ? argv[2] : "";
    const std::string yuvFile  = (argc > 3 && std::strcmp(argv[3], "-") != 0) ? argv[3] : "";
    const int         width    = (argc > 4) ? std::atoi(argv[4]) : 352;
    const int         height   = (argc > 5) ? std::atoi(argv[5]) : 288;
    const int         fps      = (argc > 6) ? std::atoi(argv[6]) : kStreamFrameRate;
    const double      duration = (argc > 7) ? std::atof(argv[7]) : kDefaultDuration;
    gDuration = duration;    /* 交给帧生成函数使用（见文件头的说明） */

    if (width <= 0 || height <= 0 || fps <= 0 || duration <= 0) {
        std::fprintf(stderr, "参数非法: %dx%d fps=%d duration=%.2f"
                             "（跳过可选参数请用 '-' 占位，不要传空字符串）\n",
                     width, height, fps, duration);
        return 1;
    }

    /* 最后一个参数可以是 -v：打开逐包时间戳打印 */
    for (int i = 8; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            gVerbosePackets = true;
    }

    std::printf("输出: %s\n", outFile.c_str());
    std::printf("  容器格式: %s\n", format.empty() ? "(按扩展名猜)" : format.c_str());
    std::printf("  视频: %dx%d @ %d fps，%s\n", width, height, fps,
                yuvFile.empty() ? "合成测试图" : yuvFile.c_str());
    std::printf("  音频: 44100 Hz 立体声 正弦扫频（合成）\n");
    std::printf("  时长: %.1f 秒\n\n", duration);

    /*
     * =======================================================================
     * 【知识点 24】为什么把主体塞进一个立即执行的 lambda（不要用 goto cleanup）
     * =======================================================================
     * 原 C 示例用 `goto cleanup` 做统一收尾，这在本文件里**编译不过**，
     * 因为 C++ 硬性禁止「goto 跳过带初始化的变量声明」
     * （错误：jump to label 'cleanup' crosses initialization of ...）。
     * 本文件主体里全是 `const int x = ...` 这类声明，所以必然撞上。
     *
     * 三种可行写法：
     *   (a) 立即执行的 lambda + return 提前退出，资源在 lambda 内统一收尾
     *       —— 本教学工程用的办法，流程最平铺直叙；
     *   (b) RAII：把每个资源包成对象（unique_ptr + 自定义 deleter），
     *       完全不需要 cleanup 段 —— pusher 里的做法
     *       （FormatMuxer 析构自动 writeTrailer + close，异常安全）；
     *   (c) 把资源声明提到 goto 之前 —— 能编过，但代码会变得零散，不推荐。
     *
     * 生产代码推荐 (b)。
     * =======================================================================
     */
    const int exitCode = [&]() -> int {
        AVFormatContext* oc   = nullptr;
        OutputStream*    vst  = nullptr;
        OutputStream*    ast  = nullptr;
        AVPacket*        pkt  = nullptr;
        bool haveVideo = false, haveAudio = false;
        bool encodeVideo = false, encodeAudio = false;

    /*
     * =======================================================================
     * 【知识点 17】第 1 步：建输出上下文 —— 容器是怎么选出来的
     * =======================================================================
     *       avformat_alloc_output_context2(&oc, nullptr, format, filename);
     *
     * 它按这个优先级决定容器：
     *   1. 参数 format（如 "flv"）—— 显式指定，最可靠
     *   2. 参数 filename 的扩展名（.flv → flv，.mp4 → mp4）
     *   3. 参数 filename 的协议（rtmp:// → flv，srt:// → mpegts）
     *
     * 所以推流时：
     *       "rtmp://host/live/key"  会自动选 flv（RTMP 只承载 flv）
     *       "out.mp4"               会选 mp4
     *   这就是 pusher 里 OutputConfig::format「留空则按 url 猜」的依据。
     *
     * 返回失败（oc 为 nullptr）通常意味着扩展名不认识，
     * 原示例的兜底是「改用 flv」，本程序沿用。
     * =======================================================================
     */
    int ret = avformat_alloc_output_context2(&oc, nullptr,
                                             format.empty() ? nullptr : format.c_str(),
                                             outFile.c_str());
    if (ret < 0 || !oc) {
        std::printf("按扩展名猜不出容器，改用 flv\n");
        ret = avformat_alloc_output_context2(&oc, nullptr, "flv", outFile.c_str());
    }
    if (ret < 0 || !oc) {
        std::fprintf(stderr, "avformat_alloc_output_context2 失败\n");
        return 1;
    }
    std::printf("已选定容器: %s (%s)\n", oc->oformat->name, oc->oformat->long_name);
    std::printf("  AVFMT_GLOBALHEADER=%d（%s）\n",
                !!(oc->oformat->flags & AVFMT_GLOBALHEADER),
                (oc->oformat->flags & AVFMT_GLOBALHEADER)
                    ? "容器要求独立头 → 编码器要设 GLOBAL_HEADER"
                    : "容器不需要独立头");

    /*
     * =======================================================================
     * 【知识点 18】选编码器：为什么不用 oc->oformat->video_codec 直接赋值
     * =======================================================================
     * 原示例（4.2.1 时代）允许这样写：
     *       fmt->video_codec = AV_CODEC_ID_H264;    // fmt 是非 const
     *       fmt->audio_codec = AV_CODEC_ID_AAC;
     *
     * FFmpeg 8.1 里：
     *   - avformat_alloc_output_context2 的第二个参数是 **const** AVOutputFormat*
     *   - oc->oformat 也是 const AVOutputFormat*
     *   → 不能再改它内部的 video_codec/audio_codec 字段（那本来就是
     *     「该容器的默认编码格式」，不是给你改的配置项）
     *
     * 正确做法：**直接指定编码器**，然后
     *       cc->codec_id = codecId;              // 在 addStream 里设
     *       avcodec_parameters_from_context(...) // 把它写进 st->codecpar
     *   容器兼容性由下面 openOutputFile() 的候选回退逻辑保证。
     *
     * 【为什么需要「候选回退」】
     *   不是所有容器都支持所有编码格式。例如：
     *       flv 只支持 H.264/VP6 视频 + AAC/MP3 音频
     *       要往 flv 里塞 h265 会被 avformat_write_header 拒绝
     *   所以本程序把「编码格式 ID」提前告诉 avformat_alloc_output_context2，
     *   让它帮你校验；不行就换下一个候选组合。
     * =======================================================================
     */
    /* 候选组合：优先 H.264 + AAC（推流/FLV 的标准组合） */
    const struct { AVCodecID video; AVCodecID audio; } kCandidates[] = {
        { AV_CODEC_ID_H264, AV_CODEC_ID_AAC },
        { AV_CODEC_ID_NONE, AV_CODEC_ID_AAC },   /* 只要音频 */
        { AV_CODEC_ID_H264, AV_CODEC_ID_NONE },  /* 只要视频 */
    };

    for (const auto& cand : kCandidates) {
        avformat_free_context(oc);
        oc = nullptr;

        ret = avformat_alloc_output_context2(&oc, nullptr,
                                             format.empty() ? nullptr : format.c_str(),
                                             outFile.c_str());
        if (ret < 0 || !oc)
            ret = avformat_alloc_output_context2(&oc, nullptr, "flv", outFile.c_str());
        if (ret < 0 || !oc)
            break;

        /* 通过第三个参数把候选编码格式告诉 avformat，让它校验容器兼容性 */
        if (cand.video != AV_CODEC_ID_NONE) {
            if (avformat_query_codec(oc->oformat, cand.video, FF_COMPLIANCE_NORMAL) != 1)
                continue;   /* 该容器不接受这个视频格式，试下一个组合 */
        }
        if (cand.audio != AV_CODEC_ID_NONE) {
            if (avformat_query_codec(oc->oformat, cand.audio, FF_COMPLIANCE_NORMAL) != 1)
                continue;
        }

        if (cand.video != AV_CODEC_ID_NONE) {
            const AVCodec* c = avcodec_find_encoder(cand.video);
            vst = addStream(oc, c, cand.video);
            if (vst) { haveVideo = true; encodeVideo = true; }
        }
        if (cand.audio != AV_CODEC_ID_NONE) {
            const AVCodec* c = avcodec_find_encoder(cand.audio);
            ast = addStream(oc, c, cand.audio);
            if (ast) { haveAudio = true; encodeAudio = true; }
        }
        if (haveVideo || haveAudio)
            break;
    }

    if (!oc || (!haveVideo && !haveAudio)) {
        std::fprintf(stderr, "无法为容器 %s 找到可用的编码器组合\n", outFile.c_str());
        if (oc) avformat_free_context(oc);
        return 1;
    }
    std::printf("  音视频流: %s%s\n",
                haveVideo ? "视频(H.264) " : "", haveAudio ? "音频(AAC)" : "");

    /*
     * =======================================================================
     * 【知识点 19】第 5~6 步：打开编码器 + 把参数交给流
     * =======================================================================
     * 顺序非常重要，一步都不能颠倒：
     *
     *   addStream()              建流 + 建编码器上下文 + 设 GLOBAL_HEADER
     *        ↓
     *   openVideo/openAudio()    设参数 → avcodec_open2 → 分配 frame
     *        ↓                          → avcodec_parameters_from_context
     *   av_dump_format()         把容器的整体结构打印出来（排查问题很有用）
     *        ↓
     *   avio_open()              真正打开输出（文件或网络连接）
     *        ↓
     *   avformat_write_header()  ★ 写容器头；之后 st->time_base 才是最终值
     *        ↓
     *   循环：编码 + av_interleaved_write_frame
     *        ↓
     *   av_write_trailer()       ★ 写容器尾（mp4 的 moov 在这里！）
     * =======================================================================
     */
    if (haveVideo)
        openVideo(oc, vst, yuvFile, width, height, fps);
    if (haveAudio)
        openAudio(oc, avcodec_find_encoder(AV_CODEC_ID_AAC), ast);

    /*
     * av_dump_format 会打印容器的完整结构：有几路流、每路的编码格式、
     * 分辨率、采样率、时间基。排查「为什么播放器认不出」时第一步就看它。
     */
    av_dump_format(oc, 0, outFile.c_str(), 1);

    /*
     * 打开输出 IO。像 "null" muxer 这类格式带 AVFMT_NOFILE，
     * 不需要（也不能）打开文件 —— 必须先判断，否则 avio_open 会失败。
     */
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->pb, outFile.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::fprintf(stderr, "avio_open 失败 (%s): %s\n",
                         outFile.c_str(), errStr(ret).c_str());
            avformat_free_context(oc);
            return 1;
        }
    }

    /*
     * =======================================================================
     * 【知识点 20】avformat_write_header —— 时间基在这里被改写
     * =======================================================================
     * 写头之前 st->time_base 是我们设的（视频 1/fps、音频 1/sample_rate）；
     * 写头之后，muxer 会按容器规矩改写它：
     *       flv  → 视频/音频都变成 1/1000（毫秒）
     *       mp4  → 常变成 1/90000（MPEG-TS 传统）或 1/12800 之类
     *
     * 所以本程序在写头前后各打印一次时间基，把这个「改写」现象直接展示出来。
     * 这也解释了为什么 writeFrame 里必须**现查** st->time_base 做换算。
     * =======================================================================
     */
    std::printf("\n写头之前的时间基：\n");
    for (unsigned i = 0; i < oc->nb_streams; ++i)
        std::printf("  流%d: %d/%d\n", i,
                    oc->streams[i]->time_base.num, oc->streams[i]->time_base.den);

    ret = avformat_write_header(oc, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "avformat_write_header 失败: %s\n", errStr(ret).c_str());
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return 1;
    }

    std::printf("写头之后的时间基（被 muxer 改写了）：\n");
    for (unsigned i = 0; i < oc->nb_streams; ++i)
        std::printf("  流%d: %d/%d%s\n", i,
                    oc->streams[i]->time_base.num, oc->streams[i]->time_base.den,
                    (oc->streams[i]->time_base.den == 1000) ? "  ← 毫秒（flv 的规矩）" : "");

    /* 编码输出用的包（整个循环复用同一个，receive 内部会 unref 复用它） */
    pkt = av_packet_alloc();
    if (!pkt) {
        std::fprintf(stderr, "av_packet_alloc 失败\n");
        av_write_trailer(oc);
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return 1;
    }

    /*
     * =======================================================================
     * 【知识点 21】交织主循环 —— 谁的时间戳小就先写谁
     * =======================================================================
     * 这是 muxing.c 最核心的循环，也是「交织」两个字的真正含义。
     *
     *   视频流: nextPts 0, 1, 2, ...      (1/fps 为一格)
     *   音频流: nextPts 0, 1024, 2048,... (1/sample_rate 为一格)
     *
     * 两者单位不同、节奏不同。要保证写进文件的包按时间递增，
     * 就得每次挑「下一个时间戳最早」的那一路来生成并写。
     *
     * av_compare_ts(tsA, tbA, tsB, tbB) 返回：
     *      < 0 : A 的时间早于 B
     *      = 0 : 同时
     *      > 0 : A 晚于 B
     * 它会自动把两个不同的时间基换算到同一标尺再比，所以可以直接用。
     *
     * 【为什么必须这么做，不能先把视频写完再写音频】
     *   因为 av_interleaved_write_frame 要求 dts 单调递增。
     *   如果先写完全部视频（dts 0..124），再写音频（dts 从 0 开始），
     *   音频的 dts 就倒退了 → 报 "non-monotonous DTS" 错误。
     *   （有些 muxer 容忍，但播放器 seek 会乱，音画也会不同步。）
     *
     * 【两路都结束时退出】
     *   writeXxxFrame 返回非 0 表示「这一路已经彻底结束（含冲刷完成）」。
     * =======================================================================
     */
    std::printf("\n开始编码并交织写入...\n");

    /*
     * 【知识点 21.1】三个容易混淆的现象，必须分清
     * -----------------------------------------------------------------
     * 现象 A：日志开头「全是音频包，视频包迟迟不出现」
     *   —— **这不是 bug，是视频编码器的 look-ahead 延迟。**
     *   libx264 默认 rc_lookahead=10，加上 B 帧（max_b_frames=1）和
     *   帧级多线程，它要吃掉若干帧才开始吐第一个包。
     *   而 AAC 几乎立刻就出包（只延迟 1 个 1024 采样的帧）。
     *   所以前 ~40 次循环里视频只送不吐，日志上就只见音频。
     *
     *   实测：本示例（352x288, lookahead=10, bframes=1, threads=9）
     *   第 1 个视频包在日志第 80 行（音频已写了约 40 个包）。
     *
     * 现象 B：**文件里的包顺序**和**日志里的调用顺序**不一样
     *   —— 因为 av_interleaved_write_frame 内部会按 dts 排序缓冲，
     *   它是「攒够一段再按 dts 吐」。用 ffprobe 看文件才是最终顺序：
     *       ffprobe -show_packets out.flv
     *   本示例实测文件里的顺序是正确的交替（视频 dts 0/40/80…，
     *   音频 dts 递增，两者交错），所以交织是成功的。
     *
     * 现象 C：平局（返回 0）时如果总选同一路，长期会不均衡
     *   —— 这是推流场景要避免的：流式播放没有「回头重排」的机会，
     *   如果一口气推 1 秒音频再推 1 秒视频，播放器就得攒一大堆数据。
     *   所以下面在平局时**交替**选择，让两路均匀推进。
     * -----------------------------------------------------------------
     */
    bool preferVideoOnTie = true;

    while (encodeVideo || encodeAudio) {
        const bool videoEarlier =
            encodeVideo &&
            (!encodeAudio ||
             av_compare_ts(vst->nextPts, vst->enc->time_base,
                           ast->nextPts, ast->enc->time_base) < 0);

        /* 平局（返回 0）时按 preferVideoOnTie 交替 */
        const bool tie = encodeVideo && encodeAudio &&
                         av_compare_ts(vst->nextPts, vst->enc->time_base,
                                       ast->nextPts, ast->enc->time_base) == 0;
        const bool doVideo = videoEarlier || (tie && preferVideoOnTie);

        if (doVideo) {
            if (!writeVideoFrame(oc, vst, pkt) && vst->finished)
                encodeVideo = false;
            if (tie)
                preferVideoOnTie = false;
        } else {
            if (!writeAudioFrame(oc, ast, pkt) && ast->finished)
                encodeAudio = false;
            if (tie)
                preferVideoOnTie = true;
        }
    }
    std::printf("交织循环结束。\n");

    /*
     * =======================================================================
     * 【知识点 22】av_write_trailer —— 忘了它就等于文件废了
     * =======================================================================
     * 它做三件事：
     *   (1) flush muxer 的内部缓冲（av_interleaved_write_frame 缓存的包在这里真正写出去）
     *   (2) 写容器的「尾」：
     *         mp4/mov → **moov 箱**（索引表：每帧的位置和大小）
     *         flv     → 什么都不用写（flv 是流式格式，没有全局索引）
     *   (3) 更新头部某些字段（如 duration）
     *
     * ★ mp4 的 moov 不写，文件基本就无法播放/无法 seek
     *   （有些播放器能靠扫描修复，但不该依赖）。
     *   这是一个非常常见的坑：程序崩了没走到这行，mp4 就成了砖头。
     *
     * 【顺序要求】必须在关闭编码器之前调 ——
     *   官方注释明确说：如果先 avcodec_close 再调 trailer，
     *   trailer 可能访问已被释放的内存。
     * =======================================================================
     */
    std::printf("\n写 trailer...\n");
    if (av_write_trailer(oc) < 0)
        std::fprintf(stderr, "av_write_trailer 失败\n");

    /* ---------- 汇总 ---------- */
    {
        std::printf("\n===== 封装完成 =====\n");
        std::printf("容器     : %s\n", oc->oformat->name);
        std::printf("文件     : %s\n", outFile.c_str());
        if (haveVideo)
            std::printf("视频流#%d: 写入 %" PRId64 " 包\n",
                        vst->st->index, vst->packetsWritten);
        if (haveAudio)
            std::printf("音频流#%d: 写入 %" PRId64 " 包\n",
                        ast->st->index, ast->packetsWritten);
        std::printf("流时间基 : 视频 %d/%d  音频 %d/%d\n",
                    haveVideo ? vst->st->time_base.num : 0,
                    haveVideo ? vst->st->time_base.den : 0,
                    haveAudio ? ast->st->time_base.num : 0,
                    haveAudio ? ast->st->time_base.den : 0);
    }

cleanup:
    /* =======================================================================
     * 【知识点 23】释放顺序（对照 pusher 的 RAII 写法）
     * =======================================================================
     * 严格的顺序：
     *   1. av_write_trailer()        ← 必须在编码器还活着的时候调
     *   2. 关闭各编码器 / frame      ← closeStream()
     *   3. avio_closep(&oc->pb)      ← 关闭输出 IO（会 flush 剩余缓冲）
     *   4. avformat_free_context()   ← 最后释放容器上下文
     *
     * ★ avio_closep 传的是 &oc->pb（二级指针），因为它要把
     *   调用方的 pb 置 nullptr。avio_close（不带 p）已废弃。
     *
     * 【pusher 里的做法】不用 goto，而是把每一步都包成 RAII：
     *   FormatMuxer 的析构函数里依次调 writeTrailer() + close()，
     *   即使中途抛异常/提前 return，栈展开也会走到析构 —— 不会漏掉 trailer。
     *   这是 C++ 相比 C 的核心优势，生产代码应该这么写。
     * ======================================================================= */
    av_packet_free(&pkt);
    if (haveVideo) closeStream(vst);
    if (haveAudio) closeStream(ast);

    if (oc) {
        if (oc->oformat && !(oc->oformat->flags & AVFMT_NOFILE) && oc->pb)
            avio_closep(&oc->pb);
        avformat_free_context(oc);
    }
    return 0;
    }();   /* lambda 立即执行 */

    if (exitCode == 0) {
        std::printf("\n验证方法:\n");
        std::printf("  ffprobe -show_format -show_streams %s\n", outFile.c_str());
        std::printf("  ffplay %s\n", outFile.c_str());
        std::printf("  看包的时间戳交织情况: ffprobe -show_packets %s | findstr pts\n",
                    outFile.c_str());
    }
    return exitCode;
}

/*
 * ===========================================================================
 * 附：从本文件到 pusher/muxer.cpp 的迁移对照
 * ===========================================================================
 *   本文件                              pusher 里
 *   ──────────────────────────         ──────────────────────────────
 *   OutputStream 结构体           →    encoder 层与 muxer 层分开：
 *                                      IVideoEncoder / IAudioEncoder / IMuxer
 *   一个 main 走完全程            →    Pusher 串起来：采集线程 → 编码线程 → 推流线程
 *   fwrite 到文件 / avio_open     →    avio_open2 + rw_timeout（网络必须设超时）
 *   writeFrame 里的换算           →    FormatMuxer::writePacket（记住输入时间基）
 *   av_write_trailer              →    FormatMuxer 析构里兜底调一次，避免 mp4 缺 moov
 *   出错 exit(1)                  →    返回 false，由 Pusher 决定重连还是退出
 *
 * 【推流相比落盘多出来的三件事（本示例没有，但 pusher 必须做）】
 *   1. rw_timeout：网络写可能永久阻塞，必须设超时（单位微秒）
 *   2. AVIOInterruptCB：关窗口时能立刻打断阻塞中的写，而不是干等超时
 *   3. 断线重连：muxer 写失败后销毁重建（同时要处理「重连后时间戳怎么接」）
 *
 * 【为什么 RTMP 一定配 flv】
 *   RTMP 协议本身只承载 FLV 的 tag 结构，所以推 RTMP 时
 *   avformat_alloc_output_context2("rtmp://...") 会自动选中 flv muxer，
 *   并且 flv 的 time_base 是毫秒 —— 这就是为什么推流端时间戳都以毫秒讨论。
 * ===========================================================================
 */
