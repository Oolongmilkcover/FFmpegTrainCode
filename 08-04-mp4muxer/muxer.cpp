#include "muxer.h"

#include <cinttypes>
#include <cstdio>

extern "C" {
#include <libavutil/error.h>    // av_strerror
#include <libavutil/mathematics.h> // av_rescale_q
}

namespace {

std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

} // namespace

Muxer::~Muxer()
{
    /*
     * 兜底：如果调用方忘了 SendTrailer / DeInit，至少把 IO 和上下文释放掉，
     * 避免内存泄漏和「文件没关闭导致 mp4 尾部没写完」。
     * 注意 DeInit 是幂等的（指针用完置 nullptr），所以重复调用安全。
     */
    DeInit();
}

// =============================================================================
//  【知识点 10】(1/3) 建输出上下文
// =============================================================================
int Muxer::Init(const std::string& url)
{
    DeInit();

    /*
     * avformat_alloc_output_context2 按这个优先级选容器：
     *   1. 第 3 个参数给的 format（本工程传 nullptr，跳过）
     *   2. 第 4 个参数 url 的扩展名（".mp4" → mp4 muxer）
     *   3. url 的协议（"rtmp://" → flv）
     */
    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, nullptr, url.c_str());
    if (ret < 0 || !fmt_ctx_) {
        std::printf("avformat_alloc_output_context2 失败: %s\n", errStr(ret).c_str());
        return -1;
    }

    url_ = url;
    std::printf("Muxer 初始化: %s (容器=%s)\n", url_.c_str(), fmt_ctx_->oformat->name);
    return 0;
}

// =============================================================================
//  【知识点 15】DeInit —— 原版这里用错了函数 ★真 bug★
// =============================================================================
/*
 * 原版写法：
 *     if (fmt_ctx_) {
 *         avformat_close_input(&fmt_ctx_);      // ✗ 这是给「输入上下文」用的
 *     }
 *
 * 为什么错：
 *   avformat_close_input() 是**解封装**用的收尾函数，它会按输入格式
 *   （AVInputFormat）去关闭 demuxer 的私有数据。而我们的 fmt_ctx_ 是
 *   avformat_alloc_output_context2 建出来的**输出上下文**，
 *   用输入的函数去关输出上下文属于未定义行为（轻则泄漏，重则崩溃）。
 *
 * 正确的输出侧收尾顺序：
 *   1. 如果打开过 IO（非 AVFMT_NOFILE）→ avio_closep(&fmt_ctx_->pb)
 *      它会 flush 缓冲并关闭文件/连接，传二级指针以便置空
 *   2. avformat_free_context(fmt_ctx_)
 *
 * 另外原版 DeInit 从来没被调用（析构函数是空的），
 * 所以 avio_open 打开的 FILE 句柄一直没关 —— 本版在析构里兜底。
 */
void Muxer::DeInit()
{
    if (fmt_ctx_) {
        /*
         * 注意顺序：先关 IO，再释放上下文。
         * AVFMT_NOFILE 的格式（比如 "null"）根本没有 pb，要跳过。
         */
        if (fmt_ctx_->pb && !(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt_ctx_->pb);
        }
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    url_.clear();
    aud_codec_ctx_ = nullptr;
    aud_stream_    = nullptr;
    audio_index_   = -1;
    vid_codec_ctx_ = nullptr;
    vid_stream_    = nullptr;
    video_index_   = -1;
}

// =============================================================================
//  【知识点 10】AddStream —— 把编码器参数交给容器
// =============================================================================
int Muxer::AddStream(AVCodecContext* codec_ctx)
{
    if (!fmt_ctx_ || !codec_ctx) {
        std::printf("AddStream: 上下文为空\n");
        return -1;
    }

    AVStream* st = avformat_new_stream(fmt_ctx_, nullptr);
    if (!st) {
        std::printf("avformat_new_stream 失败\n");
        return -1;
    }

    /*
     * 关键一步：把编码器的参数**拷贝**进流。
     *
     * 拷什么：
     *   视频：codec_id / 宽高 / pix_fmt / framerate / **extradata(SPS+PPS)**
     *   音频：codec_id / 采样率 / 声道布局 / sample_fmt / **extradata(ASC)**
     *
     * 为什么必须做：容器头里要写这些信息，否则播放器不知道怎么解。
     * 特别是 extradata —— 因为编码器设了 GLOBAL_HEADER，
     * SPS/PPS 只存在于 codec_ctx_->extradata，muxer 就靠
     * st->codecpar->extradata 把它写进 mp4 的 esds/avcC 盒。
     * 漏了这步会生成「无法解码」或直接写头失败的文件。
     *
     * ★ 是 copy 不是接管：st->codecpar 有自己的 extradata 副本，
     *   之后释放编码器不会影响流。
     */
    int ret = avcodec_parameters_from_context(st->codecpar, codec_ctx);
    if (ret < 0) {
        std::printf("avcodec_parameters_from_context 失败: %s\n", errStr(ret).c_str());
        return -1;
    }

    /*
     * st->codecpar->codec_tag = 0 表示「让 muxer 自己选 tag」。
     * mp4 里 h264 的 tag 应该是 'avc1'，让 muxer 决定最省事。
     * 个别容器（如某些裸流）需要手动指定，本工程不需要。
     *
     * 【知识点 11】时间基的「建议值」
     *   这里先把 stream 的 time_base 设成编码器的时间基，
     *   但这只是**建议** —— avformat_write_header 之后 muxer 会按容器
     *   的规矩改写它（mp4 常改成 1/90000 或 1/15360 之类）。
     *   所以写包时一定不能硬编码，要现查 st->time_base。
     */
    st->codecpar->codec_tag = 0;
    st->time_base = codec_ctx->time_base;

    if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        aud_codec_ctx_ = codec_ctx;
        aud_stream_    = st;
        audio_index_   = st->index;
        std::printf("  添加音频流 #%d (%s)\n", st->index,
                    avcodec_get_name(codec_ctx->codec_id));
    } else if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        vid_codec_ctx_ = codec_ctx;
        vid_stream_    = st;
        video_index_   = st->index;
        std::printf("  添加视频流 #%d (%s)\n", st->index,
                    avcodec_get_name(codec_ctx->codec_id));
    } else {
        std::printf("  未知的流类型 %d\n", codec_ctx->codec_type);
        return -1;
    }

    return 0;
}

// =============================================================================
//  【知识点 11】SendHeader —— 在此之后 st->time_base 才是最终值
// =============================================================================
int Muxer::SendHeader()
{
    if (!fmt_ctx_) {
        return -1;
    }

    int ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) {
        std::printf("avformat_write_header 失败: %s\n", errStr(ret).c_str());
        return -1;
    }

    /* 把「被改写后」的时间基打出来 —— 排查时间戳问题第一步就看这里 */
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        const AVStream* st = fmt_ctx_->streams[i];
        std::printf("  流%d 最终 time_base = %d/%d\n",
                    static_cast<int>(i), st->time_base.num, st->time_base.den);
    }
    std::printf("已写 mp4 头\n");
    return 0;
}

// =============================================================================
//  【知识点 12 / 13】SendPacket —— 时间基换算 + 交织写入
// =============================================================================
int Muxer::SendPacket(AVPacket* packet)
{
    /*
     * ★ 参数检查必须放在最前面。
     *   原版第一行就是 `int stream_index = packet->stream_index;`，
     *   packet 为 nullptr 时直接空指针崩溃。
     */
    if (!packet) {
        std::printf("SendPacket: packet 为空\n");
        return -1;
    }

    const int stream_index = packet->stream_index;

    /* 空包（size<=0 或 data 为空）说明上游有问题，直接丢掉并释放 */
    if (packet->size <= 0 || !packet->data) {
        std::printf("SendPacket: 空包，丢弃\n");
        av_packet_free(&packet);
        return -1;
    }

    /*
     * 【知识点 12】时间基换算：编码器时间基 → 输出流时间基
     * -----------------------------------------------------------------
     *   src = 编码器 time_base（视频 {1,1000000}，音频 {1,44100}）
     *   dst = stream time_base（写头后被 muxer 改成 mp4 的规矩）
     *
     * 这一步漏了会怎样？
     *   包里的 pts 还是「微秒」的数，而容器按 1/90000 解释，
     *   结果时长会被缩小到 1/11 左右，播放器里音视频飞快播完。
     *
     * 注意 st->time_base 必须**现查**，不能在 Init 时缓存 ——
     * avformat_write_header 会改写它。
     *
     * pts/dts/duration 三个都要换算，不能只换 pts：
     *   音视频无 B 帧时 dts == pts，但一旦开 B 帧就不等。
     *   也可以直接用 av_packet_rescale_ts(packet, src, dst) 一行搞定。
     * -----------------------------------------------------------------
     */
    AVRational src_tb{ 1, 1 };
    AVRational dst_tb{ 1, 1 };
    if (vid_stream_ && vid_codec_ctx_ && stream_index == video_index_) {
        src_tb = vid_codec_ctx_->time_base;
        dst_tb = vid_stream_->time_base;
    } else if (aud_stream_ && aud_codec_ctx_ && stream_index == audio_index_) {
        src_tb = aud_codec_ctx_->time_base;
        dst_tb = aud_stream_->time_base;
    } else {
        std::printf("SendPacket: stream_index=%d 不属于任何已知流\n", stream_index);
        av_packet_free(&packet);
        return -1;
    }

    const int64_t old_pts = packet->pts;
    packet->pts      = av_rescale_q(packet->pts,      src_tb, dst_tb);
    packet->dts      = av_rescale_q(packet->dts,      src_tb, dst_tb);
    packet->duration = av_rescale_q(packet->duration, src_tb, dst_tb);
    packet->pos      = -1;   // 位置信息对输出没意义，置 -1 避免无谓 seek

    /*
     * 日志限流：只打印前 6 个包的换算过程。
     * 5 秒的片段有 300 多个包，全打出来反而看不清「时间基是怎么变的」。
     */
    if (packets_written_ < 6) {
        std::printf("index:%d  pts:%" PRId64 " -> %" PRId64 "  (tb %d/%d -> %d/%d)\n",
                    stream_index, old_pts, packet->pts,
                    src_tb.num, src_tb.den, dst_tb.num, dst_tb.den);
    } else if (packets_written_ == 6) {
        std::printf("...（后续包的换算不再逐个打印）\n");
    }
    ++packets_written_;

    /*
     * 【知识点 13】av_interleaved_write_frame
     * -----------------------------------------------------------------
     * 它按 **dts** 排序并内部缓冲，保证写进文件的包时间递增。
     * 本工程的 while 循环已经按「谁的时间戳更早」挑着送，
     * 但 muxer 内部还会再排一次 —— 两重保险，也让音视频真正交织。
     *
     * 两个行为要记住：
     *   ① 成功后它会把这个 packet **清零**（data=NULL, size=0, pts=NOPTS），
     *      因为引用被 move 进 muxer 缓冲了。所以想打印原始值必须提前存下来。
     *   ② 它已经 unref 了包，这里再 av_packet_free 只是释放 AVPacket 结构体本身
     *      （不是 double free，是必须的，因为这个 packet 是我们 alloc 的）。
     * -----------------------------------------------------------------
     */
    int ret = av_interleaved_write_frame(fmt_ctx_, packet);

    /* 结构体本身仍由我们负责释放（内容已被 muxer 取走） */
    av_packet_free(&packet);

    if (ret < 0) {
        std::printf("av_interleaved_write_frame 失败: %s\n", errStr(ret).c_str());
        return -1;
    }
    return 0;
}

// =============================================================================
//  【知识点 14】SendTrailer
// =============================================================================
/*
 * av_write_trailer 做三件事：
 *   1. flush muxer 内部缓冲（interleaved 缓存的包在这里真正写出）
 *   2. 写容器的「尾」：**mp4 的 moov 盒**（每帧位置和大小的索引表）
 *   3. 回填头部某些字段（如 duration）
 *
 * 不写 trailer 的话，mp4 里没有 moov，文件基本无法播放/无法 seek ——
 * 这是最典型也最容易犯的错（程序崩了没走到这行，文件就成了砖头）。
 *
 * 顺序要求：必须在关闭编码器**之前**调。
 */
int Muxer::SendTrailer()
{
    if (!fmt_ctx_) {
        return -1;
    }

    int ret = av_write_trailer(fmt_ctx_);
    if (ret < 0) {
        std::printf("av_write_trailer 失败: %s\n", errStr(ret).c_str());
        return -1;
    }
    std::printf("已写 mp4 trailer (moov)\n");
    return 0;
}

// =============================================================================
//  Open —— 打开输出 IO
// =============================================================================
int Muxer::Open()
{
    if (!fmt_ctx_) {
        return -1;
    }

    /* AVFMT_NOFILE 的格式（如 "null"）不需要打开文件，要跳过 */
    if (fmt_ctx_->oformat->flags & AVFMT_NOFILE) {
        return 0;
    }

    int ret = avio_open(&fmt_ctx_->pb, url_.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        std::printf("avio_open %s 失败: %s\n", url_.c_str(), errStr(ret).c_str());
        return -1;
    }
    return 0;
}
