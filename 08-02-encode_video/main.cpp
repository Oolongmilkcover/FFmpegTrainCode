/**
 * @file    08-02-encode_video/main.cpp
 * @brief   视频编码：读本地 YUV 裸数据 → 用 libx264 编成 H.264
 *
 * ===========================================================================
 * 这份代码要学什么（对应简历项目 pusher 的 encoder 层）
 * ===========================================================================
 * 采集到的视频是「一帧一帧的 YUV 裸数据」，要变成能推流/存盘的 H.264，
 * 中间就是这个流程：
 *
 *   ① 选编码器        avcodec_find_encoder_by_name("libx264")
 *   ② 建上下文        avcodec_alloc_context3
 *   ③ 设视频参数      宽高 / pix_fmt / time_base / framerate /
 *                     gop_size / max_b_frames / bit_rate
 *   ④ 调私有选项      av_opt_set(priv_data, "preset"/"tune"/"profile")
 *   ⑤ 打开            avcodec_open2          ← 之后才能读回实际生效的参数
 *   ⑥ 建帧            av_frame_alloc + get_buffer
 *   ⑦ 喂 YUV          av_image_fill_arrays 把裸数据挂到 frame 上
 *   ⑧ 编码            send_frame + receive_packet
 *   ⑨ 冲刷            send(NULL)
 *
 * 每个环节用「【知识点 N】」标出来，重点写：
 *   - 参数各自什么含义、推流和落盘为什么要设成不同的值
 *   - 时间基（time_base）到底怎么影响 pts
 *   - 返回值哪些是「正常」而不是错误
 * ===========================================================================
 * 环境：Qt 6.11.0 (MinGW 13.1.0) + FFmpeg 8.1
 *       本工程是纯 C++17，不链接 Qt 库，只用 Qt 的工具链
 * ===========================================================================
 * 用法：
 *   encode_video <in.yuv> <out.h264> [codec] [w] [h] [fps] [frames] [preset] [tune]
 *   默认：codec=libx264  w=1280 h=720  fps=25  frames=0(全部)
 *         preset=medium  tune=zerolatency
 *
 *   例（跑完整个 125 帧的测试文件）：
 *     encode_video yuv420p_1280x720.yuv out.h264
 *   例（只要前 25 帧，快速验证参数改动）：
 *     encode_video yuv420p_1280x720.yuv out.h264 libx264 1280 720 25 25
 *   例（慢速高压缩，对比 preset 对耗时/码率的影响）：
 *     encode_video yuv420p_1280x720.yuv out.h264 libx264 1280 720 25 25 veryslow
 *
 *   测试文件的来源（同目录已有）：
 *     ffmpeg -i test_1280x720.flv -t 5 -r 25 -pix_fmt yuv420p yuv420p_1280x720.yuv
 *     —— 裸 YUV 没有任何头部信息，所以宽高/像素格式/帧率必须由命令行告诉程序
 * ===========================================================================
 * @author  Liao Qingfu (原始 C 版本) / 改写为 C++17 + FFmpeg 8.1
 */

#include <cinttypes>    /* PRId64：打印 int64_t 的可移植写法 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>     /* av_strerror */
#include <libavutil/imgutils.h>  /* av_image_get_buffer_size / av_image_fill_arrays */
#include <libavutil/opt.h>       /* av_opt_set：设置编码器私有选项 */
#include <libavutil/time.h>      /* av_gettime_relative：计时用 */
}

namespace {

/*
 * ===========================================================================
 * 【知识点 1】视频编码的数据流全景
 * ===========================================================================
 *
 *   磁盘            内存                 编码器内部              输出
 *   ──────        ──────────          ────────────          ──────
 *   裸 YUV   ─读─▶ AVFrame      ─送─▶  libx264        ─取─▶ AVPacket
 *   一连串           YUV420P            运动估计             H.264 NAL
 *   平面             三块平面            DCT+量化              (IDR/P/B)
 *
 * 三个必须先建立的观念：
 *
 *   (1) 裸 YUV 不含任何格式信息。同一堆字节，当成 1280x720 解释和当成
 *       640x1440 解释，出来的画面完全不同。所以宽高/像素格式必须由调用方提供。
 *       —— 这正是本程序要命令行传 w/h 的原因，也是采集端必须把格式
 *          一路传递下去的原因。
 *
 *   (2) 编码器对输入有要求：libx264 只吃 yuv420p（或其它 YUV 平面格式），
 *       不吃 RGB/BGR。采集拿到 RGB 时，中间必须用 libswscale 转一次。
 *
 *   (3) 编码器有内部延迟。x264 默认会缓存若干帧做前向预测；
 *       推流场景必须用 tune=zerolatency 把缓存降到最低，否则画面延迟很大。
 * ===========================================================================
 */

/* 把负错误码翻译成可读字符串（libavutil 的惯例用法） */
std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

int64_t nowMs()
{
    return av_gettime_relative() / 1000;   /* 微秒 → 毫秒 */
}

/*
 * ===========================================================================
 * 【知识点 2】send / receive 配对规则（视频和音频完全一样）
 * ===========================================================================
 *   avcodec_send_frame(ctx, frame);
 *   while (1) {
 *       ret = avcodec_receive_packet(ctx, pkt);
 *       if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;   // 正常
 *       if (ret < 0) { 真错误 }
 *       ...消费 pkt...
 *   }
 *
 * 规则（和音频侧一字不差，可以直接复用到 pusher）：
 *
 *   (1) 送 1 次、收 N 次。视频因为有 B 帧和 look-ahead，经常是
 *       「送了好几帧才吐出第一个包」，也可能「送一帧吐出好几个包」。
 *
 *   (2) AVERROR(EAGAIN) = 现在没包，请继续送输入；不是错误。
 *       AVERROR_EOF     = 冲刷完毕；也不是错误。
 *
 *   (3) 冲刷：send 一个 NULL frame。不冲刷会丢掉编码器里压着的最后几帧
 *       （x264 默认会压 look-ahead 帧，帧级多线程还会再压 thread_count 帧）。
 *
 *   (4) avcodec_receive_packet 内部第一步就是 av_packet_unref(pkt)，
 *       所以循环里不用手动 unref；但也意味着 pkt->data 下次就失效，
 *       要长期持有必须 av_packet_ref / av_packet_move_ref 拷一份。
 *
 *   (5) pkt->flags & AV_PKT_FLAG_KEY 判断是否关键帧（I 帧）。
 *       推流时关键帧很重要：新观众接入要靠它立即出画面。
 *
 * 【x264 的一个特别之处】
 *   x264 内部缓存帧时不会增加 AVFrame buffer 的引用计数
 *   （它自己拷贝了数据），所以 av_frame_make_writable 在绝大多数情况下
 *   不会真的触发重新分配。但仍然应该调 —— 换成别的编码器
 *   （或将来支持硬编）就可能会引用，不调就埋雷。
 * ===========================================================================
 */
bool encodeOne(AVCodecContext* cc, AVFrame* frame, AVPacket* pkt,
               FILE* out, int64_t* outBytes, int64_t* outPackets, int* outKeyFrames)
{
    int ret = avcodec_send_frame(cc, frame);
    if (ret < 0) {
        std::fprintf(stderr, "avcodec_send_frame 失败: %s\n", errStr(ret).c_str());
        return false;
    }

    while (true) {
        ret = avcodec_receive_packet(cc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return true;                     /* 正常结束，不是错误 */
        if (ret < 0) {
            std::fprintf(stderr, "avcodec_receive_packet 失败: %s\n", errStr(ret).c_str());
            return false;
        }

        const bool isKey = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (isKey)
            ++(*outKeyFrames);

        /* 只打印前几包和每个关键帧，避免 125 帧刷屏；想全看把条件改成 true */
        if (isKey || *outPackets < 3) {
            std::printf("  包[%3" PRId64 "] %-6s size=%-6d pts=%-5" PRId64
                        " dts=%-5" PRId64 " duration=%d\n",
                        *outPackets, isKey ? "关键帧" : "P/B帧",
                        pkt->size, pkt->pts, pkt->dts, pkt->duration);
        }

        if (std::fwrite(pkt->data, 1, pkt->size, out) != static_cast<size_t>(pkt->size)) {
            std::fprintf(stderr, "fwrite 失败\n");
            return false;
        }
        *outBytes += pkt->size;
        ++(*outPackets);
    }
}

void usage(const char* prog)
{
    std::fprintf(stderr,
        "用法: %s <in.yuv> <out.h264> [codec] [w] [h] [fps] [frames] [preset] [tune]\n"
        "  codec  默认 libx264\n"
        "  w h    默认 1280 720（必须和 YUV 文件实际尺寸一致）\n"
        "  fps    默认 25\n"
        "  frames 默认 0 = 编码到文件结束\n"
        "  preset 默认 medium（ultrafast/fast/medium/slow/veryslow）\n"
        "  tune   默认 zerolatency（推流用）；落盘可比对 film\n",
        prog);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    /* ---------- 第 1 步：解析参数 ---------- */
    const std::string inYuvFile  = argv[1];
    const std::string outH264    = argv[2];
    const std::string codecName  = (argc > 3) ? argv[3] : "libx264";
    const int         width      = (argc > 4) ? std::atoi(argv[4]) : 1280;
    const int         height     = (argc > 5) ? std::atoi(argv[5]) : 720;
    const int         fps        = (argc > 6) ? std::atoi(argv[6]) : 25;
    const int         maxFrames  = (argc > 7) ? std::atoi(argv[7]) : 0;   /* 0 = 全部 */
    const std::string preset     = (argc > 8) ? argv[8] : "medium";
    const std::string tune       = (argc > 9) ? argv[9] : "zerolatency";

    std::printf("输入 YUV : %s\n", inYuvFile.c_str());
    std::printf("  尺寸 %dx%d  fps=%d  pix_fmt=yuv420p\n", width, height, fps);
    std::printf("输出 H264: %s\n", outH264.c_str());
    std::printf("  编码器 %s  preset=%s  tune=%s\n\n",
                codecName.c_str(), preset.c_str(), tune.c_str());

    /*
     * =======================================================================
     * 【知识点 23】为什么把主体塞进一个立即执行的 lambda
     * =======================================================================
     * 原 C 版本用 `goto cleanup` 做统一收尾。这在 C 里没问题，但 C++ 有一条
     * 硬性规定：**goto 不能跳过带初始化的变量声明**
     * （错误信息形如 "jump to label crosses initialization of ..."）。
     * 本程序主体里有大量 `const int frameBytes = ...` 这类声明，
     * 所以直接照搬 goto 会编译不过。
     *
     * 两种正确改法：
     *   (a) 把主体放进一个立即执行的 lambda，内部用 return 做「提前退出」，
     *       资源在 lambda 内部统一收尾 —— 本程序用的办法，骨架最清晰；
     *   (b) 用 RAII：把每个资源包成带析构函数的类（unique_ptr + 自定义
     *       deleter），完全不需要 goto/cleanup —— 这是 pusher 里的做法
     *       （X264VideoEncoder 在析构函数里统一 close()）。
     *
     * 生产代码推荐 (b)。本教学工程用 (a)，是为了让「每一步做了什么」
     * 按顺序平铺出来，不被类的封装打断。
     * =======================================================================
     */
    const int exitCode = [&]() -> int {
        const AVCodec*  codec  = nullptr;
        AVCodecContext* cc     = nullptr;
        AVFrame*        frame  = nullptr;
        AVPacket*       pkt    = nullptr;
        uint8_t*        yuvBuf = nullptr;
        FILE*           infile  = nullptr;
        FILE*           outfile = nullptr;

        /*
         * ===================================================================
         * 【知识点 3】第 2 步：查找编码器
         * ===================================================================
         *   avcodec_find_encoder(AV_CODEC_ID_H264)
         *       按格式 ID 找 → 返回注册表里第一个匹配的实现。
         *       H.264 有十几个实现（libx264 / h264_nvenc / h264_qsv / h264_amf /
         *       h264_mf / libopenh264 ...），返回哪个**不确定**。
         *
         *   avcodec_find_encoder_by_name("libx264")
         *       按 AVCodec.name 精确找 → 结果**可控**。
         *
         * 【为什么推流端一定要用 by_name】
         *   不同实现的参数集完全不同：h264_nvenc 不认 "preset=medium" 这种
         *   x264 的档位名，它会报错或者静默忽略。想做「一份代码、多种编码器
         *   可切」就必须明确指定名字，失败时给出可读提示，而不是拿到一个
         *   神秘实现然后设参数处处失败。
         *
         * 返回值指向 libavcodec 的全局静态注册表，不需要也不能 free。
         * 返回 nullptr = 这份 FFmpeg 没编进该编码器。
         * 用 `ffmpeg -encoders | findstr 264` 可以确认。
         * =================================================================== */
        codec = avcodec_find_encoder_by_name(codecName.c_str());
        if (!codec) {
            std::fprintf(stderr,
                         "找不到编码器 \"%s\"。\n"
                         "用 `ffmpeg -encoders | findstr 264` 看这份 FFmpeg 支持哪些；\n"
                         "libx264 需要编译时带 --enable-libx264。\n",
                         codecName.c_str());
            return 1;
        }
        std::printf("找到编码器: %s (%s)  id=%d\n", codec->name, codec->long_name, codec->id);
        std::printf("  capabilities=0x%x%s%s\n", codec->capabilities,
                    (codec->capabilities & AV_CODEC_CAP_DELAY) ? " 有内部延迟" : "",
                    (codec->capabilities & AV_CODEC_CAP_DR1) ? " 支持引用式帧" : "");

        /*
         * ===================================================================
         * 【知识点 4】第 3 步：分配编码器上下文
         * ===================================================================
         * avcodec_alloc_context3(codec) 会顺带把 codec_id / codec_type 填好，
         * 不需要（也不应该）手写这两个字段。
         * 私有参数区 priv_data 也在这里按 codec->priv_data_size 分配好了 ——
         * 后面 av_opt_set(cc->priv_data, ...) 就是往那块内存写。
         * =================================================================== */
        cc = avcodec_alloc_context3(codec);
        if (!cc) {
            std::fprintf(stderr, "avcodec_alloc_context3 失败\n");
            return 1;
        }

        /* ---------- 第 4 步：设置视频编码参数 ----------
         *
         * 【知识点 5】宽高与像素格式
         *   width/height 必须和喂进来的 YUV 尺寸严格一致，差一个像素整幅画面就错位。
         *   大多数编码器还要求宽高是偶数（yuv420p 有 2x2 的色度下采样）。
         *
         *   pix_fmt 必须和 AVFrame->format 一致，否则 avcodec_send_frame 直接报错。
         *   采集端一般给 RGB，需要 sws_scale 转成 yuv420p；
         *   本程序的输入已经是 yuv420p，所以直接固定它。
         */
        cc->width   = width;
        cc->height  = height;
        cc->pix_fmt = AV_PIX_FMT_YUV420P;

        /*
         * 【知识点 6】time_base 与 pts 的关系 ★最容易错的地方
         * -----------------------------------------------------------------
         * time_base 是「时间戳的单位」，是一个分数 {num, den}，单位 = num/den 秒。
         *
         * 视频编码器最常用的约定：
         *       time_base = {1, fps}          ← 也就是「pts 用帧号表示」
         *       framerate = {fps, 1}          ← 帧率，会写进码流的 VUI
         *
         * 于是帧号 n 对应的时间是：
         *       秒 = pts × time_base = n × (1/fps) = n/fps
         *
         * 举例 fps=25：
         *       time_base = 1/25 秒 = 40 ms
         *       第 0 帧 pts=0   → 0.00 s
         *       第 1 帧 pts=1   → 0.04 s
         *       第 25 帧 pts=25 → 1.00 s
         *
         * 【原 C 版本这里有个 bug，要特别注意】
         *   原代码写的是：
         *       codec_ctx->time_base = (AVRational){1, 25};
         *       ...
         *       pts += 40;
         *       frame->pts = pts;
         *   它把 time_base 当成了「毫秒」，所以每帧加 40。
         *   但 time_base 明明是 {1,25}，pts 的单位是「帧」而不是「毫秒」！
         *   结果 pts 变成 40, 80, 120...
         *   换算成时间 = 40/25 = 1.6 秒、3.2 秒、4.8 秒……
         *   25 帧的数据被打上了 40 秒的时间轴，播放器会认为这是一段
         *   帧率极低的幻灯片。
         *
         *   正确做法：pts 就是帧序号，每帧 +1（见主循环）。
         *
         * 【framerate 为什么必须设】
         *   它会写进 H.264 码流的 VUI（Video Usability Information）里。
         *   播放器/封装层靠它推算「这一帧该显示多久」。
         *   不设的话有些播放器会按默认 25fps 显示，实际不是 25fps 就音画不同步。
         * -----------------------------------------------------------------
         */
        cc->time_base = AVRational{ 1, fps > 0 ? fps : 25 };
        cc->framerate = AVRational{ fps > 0 ? fps : 25, 1 };

        /*
         * 【知识点 7】gop_size —— 关键帧（I 帧）间隔
         *   gop_size = 25 且 fps = 25 → 每 1 秒一个 I 帧。
         *
         *   影响：
         *     - 越小：seek 越精确、新观众接入越快出画面（推流很在意这个），
         *             代价是码率变高（I 帧比 P 帧大得多）
         *     - 越大：压缩率越高，但 seek 慢、丢包后恢复慢
         *
         *   推流常用 1~2 秒；点播可以用 5~10 秒。
         *
         *   补充：如果某帧的 frame->pict_type 手动设成 AV_PICTURE_TYPE_I，
         *   编码器会忽略 gop_size 强制编成 I 帧 —— 采集端「请求关键帧」
         *   功能就是这么实现的（pusher 里新观众接入时要触发一次）。
         */
        cc->gop_size = (fps > 0 ? fps : 25);

        /*
         * 【知识点 8】max_b_frames —— B 帧数量
         *   B 帧（双向预测帧）能显著提升压缩率，但它带来两个麻烦：
         *
         *   (1) dts < pts：B 帧要等后面的帧才能解码，所以解码顺序 ≠ 显示顺序，
         *       封装时必须按 dts 排序（这就是 av_interleaved_write_frame
         *       存在的原因）。
         *   (2) 增加编码器缓冲 → 增加端到端延迟。
         *
         *   所以：
         *     推流 / 直播  → max_b_frames = 0，并配合 tune=zerolatency
         *     点播 / 存盘  → max_b_frames = 2~3，换取更高压缩率
         *
         *   ★ 注意 zerolatency 和 B 帧是互相矛盾的：zerolatency 会关掉
         *     前向预测，此时再设 max_b_frames>0 没有意义（x264 会忽略或报冲突）。
         *     本程序在这里做了自动纠正。
         */
        cc->max_b_frames = (tune == "zerolatency") ? 0 : 2;

        /*
         * 【知识点 9】bit_rate —— 目标码率
         *   单位 bit/s。3000000 = 3 Mbps。
         *
         *   本程序只设 bit_rate，不设 rc_max_rate/rc_buffer_size，
         *   所以是普通 ABR/VBR 模式：编码器尽量贴近目标平均码率。
         *
         *   如果要更接近 CBR（推流时码率稳定，避免网络抖动）：
         *       cc->rc_max_rate    = cc->bit_rate;
         *       cc->rc_min_rate    = cc->bit_rate;
         *       cc->rc_buffer_size = cc->bit_rate;      // 1 秒的缓冲
         *   这三个一起设，x264 才会走严格 CBR。
         *
         *   参考：1280x720@25 的常见取值
         *       1.5 Mbps = 勉强能看     3 Mbps = 标清观感
         *       4~6 Mbps = 比较清晰     8 Mbps+ = 接近原始
         */
        cc->bit_rate = 3000000;

        /*
         * 【知识点 10】线程
         *   thread_count = 0 表示「让 FFmpeg 自动选」= CPU 核心数。
         *
         *   ★ 但这里有个坑：x264 默认用帧级多线程（frame threading），
         *     它会缓存 thread_count 帧再输出，导致编码延迟 = thread_count 帧。
         *     推流场景要在「性能」和「延迟」之间做取舍：
         *         低延迟优先 → thread_count = 1（tune=zerolatency 已经关掉大部分）
         *         吞吐优先   → thread_count = 0（自动）
         *
         *   另一种思路是改用帧内多线程：
         *       cc->thread_type = FF_THREAD_SLICE;   // 切片级并行，不引入帧延迟
         *   代价是压缩率会略降。
         *
         *   本程序保持默认（0），需要低延迟时请优先用 tune=zerolatency。
         */
        cc->thread_count = 0;

        /*
         * 【知识点 11】AV_CODEC_FLAG_GLOBAL_HEADER —— 决定 SPS/PPS 放哪
         * -----------------------------------------------------------------
         * H.264 的 SPS（序列参数集）和 PPS（图像参数集）描述了分辨率、
         * profile、level 等解码必需信息。它们有两种放法：
         *
         *   不设这个 flag（本程序的做法）：
         *       每个 I 帧前面都重复带一遍 SPS/PPS。
         *       → 输出是「自包含」的裸 H.264 流。
         *       → 好处：直接存 .h264 文件就能播，随便从哪个 I 帧开始都能解。
         *       → 这就是原代码注释里说的「存本地文件时不要去设置」。
         *
         *   设上这个 flag：
         *       SPS/PPS 只出现一次，被挪到 cc->extradata 里，由封装层统一携带。
         *       → 这是 mp4/flv/rtmp 的要求（它们有自己的地方存 extradata）。
         *       → 不设的话，muxer 会因为拿不到 extradata 而报错或生成坏文件。
         *
         * 【怎么判断该不该设？看容器】
         *       if (oc->oformat->flags & AVFMT_GLOBALHEADER)
         *           cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
         *   这句话就是官方示例（muxing.c）里的标准写法：
         *   「容器说它需要独立头，我才设」。
         *
         * 结论：
         *       本项目（输出裸 .h264）        → 不设
         *       08-03（封装成 flv/mp4）       → 设
         *       pusher（推 RTMP，flv）        → 设
         * -----------------------------------------------------------------
         */
        /* cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; */   /* 输出裸流，故意不设 */

        /*
         * 【知识点 12】编码器私有选项 —— av_opt_set(cc->priv_data, ...)
         * -----------------------------------------------------------------
         * cc->priv_data 指向编码器自己的参数结构体（对 libx264 就是 x264 的
         * 参数表）。通用字段用 cc->xxx 设，编码器专属的档位用 av_opt_set 设。
         *
         * 【preset】压缩率 ↔ 速度的档位（x264 特有）
         *   ultrafast → superfast → veryfast → faster → fast → medium →
         *   slow → slower → veryslow → placebo
         *
         *   实测耗时（1280x720x125 帧，数据来自原代码注释）：
         *       ultrafast  2270 ms      medium  5815 ms      veryslow  19836 ms
         *   而压缩率（同码率下的画质）反过来：越慢越好。
         *
         *   推流选 veryfast/faster：CPU 要同时干采集+编码+推流，
         *   而且观众基本看不出 veryfast 和 medium 的差别。
         *
         * 【tune】针对内容类型调优
         *   zerolatency : 关掉前瞻/多帧缓冲，延迟最低 —— **直播必选**
         *   film        : 真人影片，保留胶片颗粒
         *   animation   : 动画，色块边缘更锐利
         *   stillimage  : 静态图
         *   fastdecode  : 降低解码复杂度（弱设备客户端用）
         *
         * 【profile】H.264 档次
         *   baseline : 无 B 帧、无 CABAC，兼容性最好（老设备）
         *   main     : 支持 B 帧，直播常用
         *   high     : 8x8 变换，压缩率最高（默认）—— 蓝光/点播用
         *
         * 【返回值一定要检查】
         *   av_opt_set 返回 AVERROR_OPTION_NOT_FOUND 表示这个编码器没有该选项。
         *   不同编码器（nvenc/qsv/amf）的选项名完全不同，静默失败会导致
         *   「以为设了 zerolatency，实际延迟依然很大」这种难查的问题。
         *   生产代码应该失败就打印警告，或者直接换用 av_dict 传参。
         *
         * 【另一种写法：av_dict】
         *   AVDictionary* opts = nullptr;
         *   av_dict_set(&opts, "preset", "veryfast", 0);
         *   avcodec_open2(cc, codec, &opts);
         *   av_dict_free(&opts);          ← 必须 free
         *   两种等价；av_opt_set 的好处是马上知道哪个选项名错了。
         * -----------------------------------------------------------------
         */
        if (codec->id == AV_CODEC_ID_H264) {
            const struct { const char* key; const char* val; } opts[] = {
                { "preset",  preset.c_str() },
                { "tune",    tune.c_str()   },
                { "profile", "main"         },   /* 推流常用 main；点播可用 high */
            };
            for (const auto& o : opts) {
                const int r = av_opt_set(cc->priv_data, o.key, o.val, 0);
                if (r < 0)
                    std::fprintf(stderr,
                                 "  警告: 设置 %s=%s 失败 (%s)，该编码器可能不支持此选项\n",
                                 o.key, o.val, errStr(r).c_str());
                else
                    std::printf("  私有选项 %s = %s\n", o.key, o.val);
            }
        } else {
            std::printf("  非 H.264 编码器，跳过 x264 专属选项\n");
        }

        /*
         * ===================================================================
         * 【知识点 13】第 5 步：avcodec_open2 —— 真正初始化
         * ===================================================================
         * 它做的事：
         *   - 校验参数组合（宽高是否偶数、pix_fmt 是否被支持、time_base 是否合法…）
         *   - 分配 x264 内部结构、查 CPU 特性、初始化线程池
         *   - 回填 cc->time_base（编码器可能调整它！所以要 open 之后再看）
         *   - 回填 cc->extradata / extradata_size（如果设了 GLOBAL_HEADER）
         *
         * 第三个参数 AVDictionary** 用来传私有选项（av_opt_set 的替代写法）。
         * 本程序已经用 av_opt_set 设过了，所以传 nullptr。
         *
         * 【为什么 open 之后要重新打印这些参数】
         *   某些编码器会把 time_base 规范化（比如 {1,25} 变成 {1,50}）。
         *   后面算 pts、传给 muxer 做时间基换算时，用的必须是**实际生效的值**。
         * =================================================================== */
        int ret = avcodec_open2(cc, codec, nullptr);
        if (ret < 0) {
            std::fprintf(stderr, "avcodec_open2 失败: %s\n", errStr(ret).c_str());
            return 1;
        }

        std::printf("\n打开成功。实际生效的参数：\n");
        std::printf("  %dx%d  pix_fmt=%s\n", cc->width, cc->height,
                    av_get_pix_fmt_name(cc->pix_fmt));
        std::printf("  time_base=%d/%d  framerate=%d/%d\n",
                    cc->time_base.num, cc->time_base.den,
                    cc->framerate.num, cc->framerate.den);
        std::printf("  gop_size=%d  max_b_frames=%d  bit_rate=%" PRId64 "\n",
                    cc->gop_size, cc->max_b_frames, cc->bit_rate);
        std::printf("  thread_count=%d  thread_type=%d\n", cc->thread_count, cc->thread_type);
        std::printf("  extradata_size=%d  ← 设了 GLOBAL_HEADER 时 SPS/PPS 在这里\n",
                    cc->extradata_size);
        std::printf("  时间基含义：1 个 pts 单位 = %d/%d 秒 = %.1f ms\n\n",
                    cc->time_base.num, cc->time_base.den,
                    1000.0 * cc->time_base.num / cc->time_base.den);

        /* ---------- 打开输入输出文件 ---------- */
        infile = std::fopen(inYuvFile.c_str(), "rb");
        if (!infile) {
            std::fprintf(stderr, "打不开输入文件 %s\n", inYuvFile.c_str());
            return 1;
        }
        outfile = std::fopen(outH264.c_str(), "wb");
        if (!outfile) {
            std::fprintf(stderr, "打不开输出文件 %s\n", outH264.c_str());
            return 1;
        }

        /*
         * ===================================================================
         * 【知识点 14】第 6 步：AVPacket 与 AVFrame
         * ===================================================================
         *   AVPacket  装「编码后的数据」（压缩数据），编码器的输出。
         *   AVFrame   装「一帧原始画面」（YUV 平面），编码器的输入。
         *
         * 都用专门的 alloc 函数分配，用对应的 free 释放（传二级指针以便置空）。
         * 不要用 av_init_packet（已废弃），也不要自己 malloc 这两个结构体。
         * =================================================================== */
        pkt = av_packet_alloc();
        if (!pkt) {
            std::fprintf(stderr, "av_packet_alloc 失败\n");
            return 1;
        }

        frame = av_frame_alloc();
        if (!frame) {
            std::fprintf(stderr, "av_frame_alloc 失败\n");
            return 1;
        }

        /*
         * 【知识点 15】AVFrame 三要素 → 分配内存
         *   视频帧由三要素确定：
         *       format  像素格式（yuv420p）
         *       width   宽
         *       height  高
         *   三个都设好才能 av_frame_get_buffer。
         *
         * yuv420p 的内存布局（为什么总字节数是 w*h*3/2 而不是 w*h*3）：
         *       平面 0 (Y)  : w × h           每个像素 1 字节，存亮度
         *       平面 1 (U)  : (w/2) × (h/2)   色度，2x2 下采样
         *       平面 2 (V)  : (w/2) × (h/2)   色度
         *   合计 = w*h + w*h/4 + w*h/4 = w*h*1.5
         *
         *   举例 1280x720：921600 + 230400 + 230400 = 1382400 字节
         *   （原代码注释里那个 1382400 就是这个数）
         *
         * av_frame_get_buffer 的第二个参数是 align：
         *   传 0 = 用默认对齐（通常 32/64 字节），这是最优选择。
         *   对齐是为了 SIMD 指令能一次读整块；libx264 内部大量用 SSE/AVX，
         *   不对齐会明显掉性能。通常只有从设备直接映射内存时才手动指定。
         */
        frame->format = cc->pix_fmt;
        frame->width  = cc->width;
        frame->height = cc->height;

        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            std::fprintf(stderr, "av_frame_get_buffer 失败: %s\n", errStr(ret).c_str());
            return 1;
        }

        /* 一帧 YUV 的字节数（用于 fread 一次读一帧）
         * 注意 frame->format 是 int，而这两个函数要 enum AVPixelFormat，
         * C++ 不允许 int 隐式转枚举，必须显式转换
         * （原 C 版本能编过是因为 GCC 对 C 更宽松） */
        const int frameBytes = av_image_get_buffer_size(
            static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, 1);
        std::printf("每帧 YUV 字节数 = %d\n", frameBytes);
        std::printf("  linesize[0]=%d (Y)  linesize[1]=%d (U)  linesize[2]=%d (V)\n",
                    frame->linesize[0], frame->linesize[1], frame->linesize[2]);
        /*
         * 【知识点 16】linesize 的含义与坑
         *   视频里 linesize[i] = 第 i 个平面「一行」的字节数。
         *   因为对齐要求，它常常**大于**「width」：
         *       1280 宽的 yuv420p，Y 平面一行 1280 字节，
         *       对齐到 32/64 字节后 linesize[0] 可能是 1280 或更大。
         *
         *   所以访问像素必须用：
         *       frame->data[0][y * frame->linesize[0] + x]
         *   而**不能**用：
         *       frame->data[0][y * width + x]          ← 错！行间会错位
         *
         *   这是视频代码最经典的 bug 来源（画面出现斜纹/撕裂就是这个原因）。
         *
         *   本程序里 linesize 恰好等于 width（1280 已是 32 的倍数），
         *   所以看不出差别；一旦宽度不是 32 的倍数（比如 1918），差别就出来了。
         */

        /* 读入缓冲区：一帧 YUV 的紧凑排布（无 padding），fread 需要连续内存 */
        yuvBuf = static_cast<uint8_t*>(std::malloc(frameBytes));
        if (!yuvBuf) {
            std::fprintf(stderr, "malloc yuvBuf 失败\n");
            return 1;
        }

        /*
         * ===================================================================
         * 【知识点 17】第 7 步：主循环 —— pts 就是帧序号
         * ===================================================================
         * 因为 time_base = {1, fps}，所以 pts 的「单位」就是一帧的时间。
         *   第 n 帧的 pts = n
         *   对应时间 = n × (1/fps) 秒
         *
         * 千万**不要**像原代码那样 pts += 40（那是把 time_base 当毫秒了）。
         * 也**不要** pts += 1/fps 的毫秒值（整数除法会变成 0）。
         * 正确写法就是最简单的：pts = 帧序号。
         *
         * 如果采集端给的时间戳是微秒（{1,1000000}），那就要换算：
         *   frame->pts = av_rescale_q(usPts, AVRational{1,1000000}, cc->time_base);
         * 这正是 pusher 里 X264VideoEncoder 做的事 —— 因为采集源统一产出微秒。
         * =================================================================== */
        int64_t pts        = 0;
        int64_t frameIdx   = 0;
        int64_t outBytes   = 0;
        int64_t outPackets = 0;
        int     keyFrames  = 0;
        int64_t encodeMs   = 0;
        const int64_t t0   = nowMs();

        std::printf("\n开始编码...\n");
        for (;;) {
            if (maxFrames > 0 && frameIdx >= maxFrames) {
                std::printf("已达到指定帧数 %d，停止\n", maxFrames);
                break;
            }

            const size_t readBytes = std::fread(yuvBuf, 1, frameBytes, infile);
            if (readBytes < static_cast<size_t>(frameBytes)) {
                /* 短读：文件结束，或最后有个不完整的帧（裸 YUV 没法判断，直接丢弃） */
                if (readBytes > 0)
                    std::printf("末尾有 %zu 字节不完整数据，丢弃\n", readBytes);
                std::printf("文件读取完毕\n");
                break;
            }

            /*
             * 【知识点 18】av_frame_make_writable —— 保证能安全写入
             *   编码器可能持有上一帧的引用（引用计数共享同一块 buffer）。
             *   若直接覆写，会破坏编码器正在用的数据 → 画面出现撕裂/花屏。
             *
             *   这个函数检查引用计数：
             *     只有自己用 → 直接返回（零开销）
             *     还有别人用 → 重新分配一块 buffer 并拷贝
             *
             *   返回值必须检查：为负说明重分配失败，此时 frame 不可写。
             *
             *   （x264 内部是拷贝数据的，所以这里一般不会真的重分配。
             *     但换成硬编/别的编码器就可能会引用，所以这个调用不能省。）
             */
            ret = av_frame_make_writable(frame);
            if (ret < 0) {
                std::fprintf(stderr, "av_frame_make_writable 失败: %s\n", errStr(ret).c_str());
                return 1;
            }

            /*
             * 【知识点 19】av_image_fill_arrays —— 把裸 YUV「挂」到 frame 上
             *   签名：
             *     int av_image_fill_arrays(uint8_t *dst_data[4], int dst_linesize[4],
             *                              const uint8_t *src, enum AVPixelFormat pix_fmt,
             *                              int width, int height, int align);
             *
             *   它**不拷贝数据**，只是把 src 按平面切分后填进 dst_data[]：
             *       dst_data[0] = src                        (Y)
             *       dst_data[1] = src + w*h                  (U)
             *       dst_data[2] = src + w*h + w*h/4          (V)
             *   并按 pix_fmt 算出各平面的 linesize。
             *
             *   ⚠ 后果：yuvBuf 的生命周期必须覆盖到 avcodec_send_frame() 返回之后！
             *     不要在 send 之前就 free/覆写它。本程序在下一轮循环开头才覆写，
             *     而那时 send 早已返回，所以是安全的。
             *
             *   返回值 = 需要的总字节数，应该等于 frameBytes；
             *   不相等说明传入的 src 缓冲区太小，是严重 bug，必须检查。
             */
            const int needSize = av_image_fill_arrays(
                frame->data, frame->linesize, yuvBuf,
                static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, 1);
            if (needSize != frameBytes) {
                std::fprintf(stderr, "av_image_fill_arrays 尺寸不符: need=%d, expect=%d\n",
                             needSize, frameBytes);
                return 1;
            }

            /*
             * 【知识点 20】设置 pts
             *   pts 的语义：「本帧应该在什么时刻显示」，单位是 cc->time_base。
             *   因为 time_base = {1,fps}，所以 pts = 帧序号（见知识点 17）。
             *
             *   如果想强制某帧为 I 帧（比如采集端收到「请求关键帧」）：
             *       frame->pict_type = AV_PICTURE_TYPE_I;   // 会忽略 gop_size
             *       frame->flags |= AV_FRAME_FLAG_KEY;      // 新版也建议置这个
             *   编完这一帧记得把 pict_type 复位成 AV_PICTURE_TYPE_NONE，
             *   否则后面每一帧都会变成 I 帧（码率会暴涨）。
             */
            frame->pts = pts;
            ++pts;

            /* ---- 送进编码器并取回所有包 ---- */
            const int64_t tEnc0 = nowMs();
            if (!encodeOne(cc, frame, pkt, outfile, &outBytes, &outPackets, &keyFrames))
                return 1;
            encodeMs += nowMs() - tEnc0;

            ++frameIdx;

            /* 每处理若干帧打印一次进度（125 帧时不会刷屏） */
            if (frameIdx <= 3 || frameIdx % 25 == 0) {
                std::printf("  已送 %3" PRId64 " 帧  pts=%-4" PRId64 "  已出 %2" PRId64
                            " 包  累计编码耗时 %" PRId64 " ms\n",
                            frameIdx, pts - 1, outPackets, encodeMs);
            }
        }

        /*
         * ===================================================================
         * 【知识点 21】冲刷 —— 不冲刷就会丢尾部若干帧
         * ===================================================================
         * 循环结束时编码器内部还压着一些帧：
         *   - x264 的 look-ahead（前向预测）要缓存若干帧才能决定怎么编
         *   - 线程池（frame threading）也会让输出滞后 thread_count 帧
         *   - B 帧需要等后面的帧才能确定
         *
         * 冲刷方式：send(nullptr)，然后一直 receive 到 AVERROR_EOF。
         *
         * 【后果】不冲刷的话，最后几帧会永久丢失。视频表现为「最后 0.2 秒卡住」；
         * 音频表现为「最后一个音被切断」。这是推流端最容易漏的一步。
         * =================================================================== */
        std::printf("\n冲刷编码器（send NULL）...\n");
        if (!encodeOne(cc, nullptr, pkt, outfile, &outBytes, &outPackets, &keyFrames)) {
            std::fprintf(stderr, "冲刷失败\n");
            return 1;
        }

        /* ---------- 汇总 ---------- */
        const int64_t totalMs = nowMs() - t0;
        const double  durSec  = pts * av_q2d(cc->time_base);   /* pts × time_base = 秒 */
        std::printf("\n===== 编码完成 =====\n");
        std::printf("输入帧数   : %" PRId64 " 帧\n", frameIdx);
        std::printf("输出包数   : %" PRId64 " 包（其中关键帧 %d 个）\n", outPackets, keyFrames);
        std::printf("输出大小   : %" PRId64 " 字节 (%.2f KB)\n", outBytes, outBytes / 1024.0);
        std::printf("时间轴长度 : %" PRId64 " × %d/%d = %.3f 秒  ← pts 是帧号，不是毫秒\n",
                    pts, cc->time_base.num, cc->time_base.den, durSec);
        std::printf("实际帧率   : %.2f fps\n", durSec > 0 ? frameIdx / durSec : 0.0);
        std::printf("平均码率   : %.0f kbps\n",
                    durSec > 0 ? outBytes * 8.0 / durSec / 1024.0 : 0.0);
        std::printf("总耗时     : %" PRId64 " ms（其中编码 %" PRId64 " ms）\n", totalMs, encodeMs);
        std::printf("编码速度   : %.1f fps\n", totalMs > 0 ? frameIdx * 1000.0 / totalMs : 0.0);

        /* ---- 统一收尾（lambda 内部，等价于原 C 版本的 goto cleanup） ---- */
        if (infile)  std::fclose(infile);
        if (outfile) std::fclose(outfile);
        if (yuvBuf)  std::free(yuvBuf);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        return 0;
    }();   /* lambda 立即执行 */

    if (exitCode != 0)
        return exitCode;

    std::printf("\n产物: %s\n", outH264.c_str());
    std::printf("验证方法:\n");
    std::printf("  ffprobe -show_streams %s\n", outH264.c_str());
    std::printf("  播放（含 SPS/PPS 的裸流可直接播）: ffplay %s\n", outH264.c_str());
    std::printf("  转成 mp4 看效果: ffmpeg -i %s -c copy out.mp4\n", outH264.c_str());
    return 0;
}

/*
 * ===========================================================================
 * 附：从本文件到 pusher/encoder.cpp 的迁移对照
 * ===========================================================================
 * 本文件是「一个 main 函数走完全程」的教学形态。放进 pusher 时要拆成类：
 *
 *   本文件                         pusher 里
 *   ──────────────────────        ────────────────────────────────
 *   codec/cc 等裸指针        →    X264VideoEncoder 的成员，
 *                                 构造/析构里成对管理（RAII）
 *   av_opt_set 设 preset     →    EncoderConfig::preset，由上层传入
 *   frame->pts = pts++       →    frame->pts = av_rescale_q(采集的微秒,
 *                                     {1,1000000}, cc->time_base)
 *   fwrite 到文件            →    PacketSink 回调把 AVPacket 交给 muxer
 *   encodeOne 的收包循环     →    drainToSink()，视频音频共用
 *
 * 【推流端最容易踩的三个坑（pusher 里都已处理）】
 *   1. 时间基换算漏了   → 文件时长离谱、播放器 seek 乱跳
 *   2. 忘了 flush       → 尾部丢帧
 *   3. 忘了按期出关键帧 → 新观众接入后长时间黑屏
 *      （靠 gop_size 控制间隔，必要时手动 frame->pict_type = AV_PICTURE_TYPE_I）
 * ===========================================================================
 */
