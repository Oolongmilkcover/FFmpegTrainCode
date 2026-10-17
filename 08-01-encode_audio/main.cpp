/**
 * @file    08-01-encode_audio/main.cpp
 * @brief   音频编码（AAC）—— 从本地读取裸 PCM 数据编码为 AAC
 *
 * ===========================================================================
 * 这份代码要学什么
 * ===========================================================================
 * 一整套音频编码流程，以及每一步调用的 libavcodec API 细节：
 *
 *   ① 选编码器        avcodec_find_encoder / _by_name
 *   ② 建上下文        avcodec_alloc_context3
 *   ③ 定参数          采样率 / 声道布局 / 采样格式 / 码率 / profile
 *   ④ 查能力          avcodec_get_supported_config  ← 参数设错会在这里暴露
 *   ⑤ 打开编码器      avcodec_open2                  ← frame_size 在这里才确定
 *   ⑥ 建帧            av_frame_alloc + copy 布局 + get_buffer
 *   ⑦ 喂数据          packed ↔ planar 转换（手写 / libswresample）
 *   ⑧ 编码            avcodec_send_frame + avcodec_receive_packet
 *   ⑨ 补头写文件      手写 7 字节 ADTS 头
 *   ⑩ 冲刷            send(nullptr) 把编码器内部残留全部吐出
 *   ⑪ 释放            *_free 系列
 *
 * 每个环节都会在下面用「【知识点 N】」标出来，重点写清楚：
 *   - 这个 API 的参数**各自是什么含义**
 *   - 参数**为什么必须这么设**，设错了会怎样
 *   - 调用**顺序**为什么不能颠倒
 *   - 返回值怎么判，哪些返回值是「正常」而不是错误
 * ===========================================================================
 * 运行环境：Qt 6.11.0 (MinGW 13.1.0) + FFmpeg 8.1 (libavcodec 62)
 *          本工程是纯 C++17，不链接任何 Qt 库，只用 Qt 的工具链
 *
 *          与 08-02（视频编码）、08-03（封装）保持同一套风格：
 *          extern "C" 包头 / 匿名 namespace / std::string 错误信息 /
 *          立即执行 lambda 收尾（不用 goto）。
 *
 *          这份是 08-01 的「音频编码」专题，配套讲解见同目录
 *          《音频编码知识点详解.md》。
 * ===========================================================================
 * 用法：
 *   encode_audio <input.pcm> <out.aac> [codec_name] [sample_rate] [channels] [sample_fmt] [noadts]
 *   例：
 *     encode_audio 48000_2_f32le.pcm aac.aac
 *     encode_audio 48000_2_s16le.pcm aac.aac aac 48000 2 s16le
 *     encode_audio 48000_2_f32le.pcm aac.raw aac 48000 2 f32le noadts
 * ===========================================================================
 * @author        Liao Qingfu (原始版本) / 改写为 C++17 + FFmpeg 8.1
 * @date          2020-04-15 (原始) / 2026 适配
 */

#include <cinttypes>    /* PRId64：打印 int64_t 的可移植写法 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/error.h>            /* av_strerror：负错误码 → 可读字符串 */
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>   /* 音频重采样 / 采样格式转换 */
}

namespace {

/*
 * ===========================================================================
 * 【知识点 1】音频编码的数据流全景
 * ===========================================================================
 *
 *   磁盘/麦克风               内存中              编码器内部           输出
 *   ────────────          ─────────────         ──────────         ──────
 *   裸 PCM 文件   ──读──▶  AVFrame        ──▶  AAC 编码器   ──▶   AVPacket
 *   (交错 L R L R)        (分平面 L|R)          (有内部缓存)       (裸 AAC 帧)
 *                              ▲                    │
 *                              │                    └─ 每帧 1024 个采样点
 *                              └── 必须满足编码器要求的三要素：
 *                                    采样格式 sample_fmt
 *                                    声道布局 ch_layout
 *                                    采样点数 nb_samples (= frame_size)
 *
 * 三个最容易被忽略的事实：
 *
 *  (1) PCM 是「未压缩的采样点序列」，它本身不含任何格式信息。
 *      同一堆字节，当成 s16 解释和当成 f32 解释，出来的声音完全不同。
 *      所以「读裸 PCM」必须由调用方告诉程序格式（采样率/声道数/采样格式），
 *      这正是本程序要 4 个额外命令行参数的原因。
 *
 *  (2) 编码器对输入是「有要求」的，不是给你什么都能编。
 *      AAC 编码器（native）只吃 AV_SAMPLE_FMT_FLTP —— float 且分平面。
 *      而 ffmpeg 命令行导出的 PCM 只能是 packed（交错）。
 *      所以中间必须有一步「格式转换」，这是本程序的核心工作量。
 *
 *  (3) 编码器有「内部延迟」。AAC 需要攒够 1024 个采样点才能开始做
 *      MDCT 变换，所以第一次 send 之后一个包都拿不到，这是正常的。
 * ===========================================================================
 */

/*
 * 本程序自己的「输入 PCM 描述 + 输出配置」结构体。
 * 相当于把裸 PCM 缺失的那部分格式信息补齐。
 *
 * 【C++ 相比 C 的改动】用 std::string 保存路径/编码器名，
 * 不再用裸 const char*，避免到处传 argv 的下标；
 * 成员带默认值，也就不需要 memset 清零了。
 */
struct EncodeConfig {
    std::string         inPcmFile;                    /* 输入裸 PCM 路径 */
    std::string         outFile;                      /* 输出 AAC 路径 */
    std::string         codecName;                    /* 空 = 按 ID 找默认 aac */
    int                 sampleRate   = 48000;         /* 采样率（Hz） */
    int                 channels     = 2;             /* 声道数 */
    enum AVSampleFormat inSampleFmt  = AV_SAMPLE_FMT_FLT; /* 输入 PCM 格式，默认 f32le */
    bool                writeAdts    = true;          /* 是否自己补 ADTS 头 */
};

/* 把负错误码翻译成可读字符串（libavutil 的惯例用法） */
std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

/*
 * ===========================================================================
 * 【知识点 2】编码器的能力查询 —— 参数设错要在这里就被拦住
 * ===========================================================================
 * 为什么不能「直接设了参数就 avcodec_open2」？
 *   因为编码器的限制是分散的：
 *     - 有的编码器只支持特定采样格式（AAC 只有 fltp）
 *     - 有的只支持特定采样率（AAC 只支持 7350~96000 那 13 个值）
 *     - 有的只支持特定声道布局
 *   avcodec_open2 失败时只给你一个 AVERROR(EINVAL)，
 *   你根本不知道是三个参数里的哪一个出了问题。
 *   所以「先查能力、再设参数」是 libavcodec 的推荐姿势。
 *
 * 查询统一走一个函数（FFmpeg 8.1）：
 *
 *   int avcodec_get_supported_config(const AVCodecContext *avctx,  // 可为 NULL
 *                                    const AVCodec        *codec,  // 为 NULL 则用 avctx->codec
 *                                    enum AVCodecConfig    config, // 查哪一项
 *                                    unsigned              flags,  // 保留参数，填 0
 *                                    const void          **out_configs,     // 出参：数组首地址
 *                                    int                  *out_num_configs);// 出参：元素个数，可为 NULL
 *
 * 关键细节（这几条决定了代码 robustness）：
 *
 *   a) 返回数组用「哨兵值」表示结束，不是靠 out_num_configs：
 *        AV_CODEC_CONFIG_SAMPLE_FORMAT     → enum AVSampleFormat，哨兵 AV_SAMPLE_FMT_NONE
 *        AV_CODEC_CONFIG_SAMPLE_RATE       → int，            哨兵 0
 *        AV_CODEC_CONFIG_CHANNEL_LAYOUT    → AVChannelLayout，哨兵 {0}（即 nb_channels==0）
 *      这是 libavcodec 的一贯风格（AVCodec.sample_fmts 时代就是这样），
 *      必须按哨兵遍历，官方示例 encode_audio.c 至今也是这么写的。
 *
 *   b) *out_configs == NULL 不是「失败」，而是「不做限制、全都能用」。
 *      这时应该直接判定为支持，而不是当成错误。
 *
 *   c) 出参类型是 const void *，因为一个函数要返回 7 种不同的数组类型。
 *      取出来必须显式转换成具体类型才敢解引用。C++ 里用 static_cast /
 *      reinterpret_cast 更醒目（C 的强制转换语法会把「转错了」藏起来）。
 * ===========================================================================
 */

/* 检测该编码器是否支持指定采样格式 */
int check_sample_fmt(const AVCodec* codec, enum AVSampleFormat sample_fmt)
{
    const void* cfgs = nullptr;
    int nb_cfgs = 0;
    const enum AVSampleFormat* p = nullptr;
    bool found = false;

    if (avcodec_get_supported_config(nullptr, codec,
                                     AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     &cfgs, &nb_cfgs) < 0) {
        std::fprintf(stderr, "avcodec_get_supported_config(SAMPLE_FORMAT) failed\n");
        return 0;   /* 查不到就当不支持，交给上层报错，避免用错参数硬编 */
    }

    if (!cfgs) {
        /* NULL = 这个编码器对采样格式没有限制 */
        std::printf("  %s: 对采样格式无限制\n", codec->name);
        return 1;
    }

    /* 一边遍历一边打印，同时记下有没有匹配项（哨兵 AV_SAMPLE_FMT_NONE 结束） */
    std::printf("  %s 支持的采样格式（%d 个）: ", codec->name, nb_cfgs);
    for (p = static_cast<const enum AVSampleFormat*>(cfgs);
         *p != AV_SAMPLE_FMT_NONE; ++p) {
        std::printf("%s ", av_get_sample_fmt_name(*p));
        if (*p == sample_fmt)
            found = true;
    }
    std::printf("\n");
    return found ? 1 : 0;
}

/*
 * 检测该编码器是否支持指定采样率。
 * 「采样率」= 每秒钟采多少个点，单位 Hz。
 * 它决定了音频的频率上限（奈奎斯特定理）：能表示的频率 < 采样率/2。
 * 48000 Hz 能表示到 24000 Hz，人耳上限约 20000 Hz，所以 CD(44100)/视频(48000) 够用。
 */
int check_sample_rate(const AVCodec* codec, int sample_rate)
{
    const void* cfgs = nullptr;
    int nb_cfgs = 0;
    const int* p = nullptr;
    bool found = false;

    if (avcodec_get_supported_config(nullptr, codec,
                                     AV_CODEC_CONFIG_SAMPLE_RATE, 0,
                                     &cfgs, &nb_cfgs) < 0) {
        std::fprintf(stderr, "avcodec_get_supported_config(SAMPLE_RATE) failed\n");
        return 0;
    }

    if (!cfgs) {
        std::printf("  %s: 对采样率无限制\n", codec->name);
        return 1;
    }

    std::printf("  %s 支持的采样率（%d 个）: ", codec->name, nb_cfgs);
    for (p = static_cast<const int*>(cfgs); *p != 0; ++p) {
        std::printf("%d ", *p);
        if (*p == sample_rate)
            found = true;
    }
    std::printf("\n");
    return found ? 1 : 0;
}

/*
 * 检测该编码器是否支持指定声道布局。
 *
 * 【知识点 2.1】AVChannelLayout —— 声道布局到底是什么
 * ---------------------------------------------------------------------------
 * 声道布局描述的不是「几个声道」，而是「这几个声道分别是什么、按什么顺序排」。
 * 例如 2 声道的 stereo 明确规定了：
 *     第 0 个采样点是左声道（AV_CHAN_FRONT_LEFT）
 *     第 1 个采样点是右声道（AV_CHAN_FRONT_RIGHT）
 * 而 2 声道的 downmix 则是 STEREO_LEFT / STEREO_RIGHT，含义不同。
 * 6 声道的 5.1 还规定了 L/R/C/LFE/SL/SR 的顺序。
 *
 * 结构体定义（FFmpeg 8.1）：
 *     typedef struct AVChannelLayout {
 *         enum AVChannelOrder order;       // 顺序语义
 *         int                 nb_channels; // 声道数
 *         union {
 *             uint64_t         mask;       // order==NATIVE 时的位掩码
 *             AVChannelCustom *map;        // order==CUSTOM 时的逐声道表
 *         } u;
 *         void *opaque;
 *     } AVChannelLayout;
 *
 * order 的取值：
 *     AV_CHANNEL_ORDER_NATIVE    —— 用标准掩码描述（绝大多数情况）
 *     AV_CHANNEL_ORDER_UNSPEC    —— 只知道声道数，不知道每路是什么
 *     AV_CHANNEL_ORDER_CUSTOM    —— 逐声道自定义（map 数组，**持有堆内存**）
 *     AV_CHANNEL_ORDER_AMBISONIC —— 全景声（Ambisonic）
 *
 * 【三条必须记住的规矩】
 *   (1) 比较两个布局只能用 av_channel_layout_compare()，返回 0 表示相等。
 *       不能写 a == b（结构体不能比较），
 *       更不能 memcmp（里面有 opaque/map 指针，同布局不同指针会误判不等）。
 *   (2) 复制只能用 av_channel_layout_copy(dst, src)，
 *       不能写 dst = src。因为 CUSTOM 布局的 u.map 指向堆内存，
 *       浅拷贝会让两个结构体指向同一块内存 → double free。
 *   (3) 自己声明的栈上 AVChannelLayout，若调用过 copy/from_string，
 *       用完必须 av_channel_layout_uninit() 释放 map。
 *       而嵌在 AVCodecContext / AVFrame 里的 ch_layout 由对应的 *_free 负责。
 *
 * 常用辅助函数：
 *   av_channel_layout_default(&l, nb_ch)           按声道数取标准布局（2→stereo）
 *   av_channel_layout_from_mask(&l, AV_CH_LAYOUT_STEREO)   从位掩码构造
 *   av_channel_layout_from_string(&l, "5.1")       从字符串解析
 *   av_channel_layout_describe(&l, buf, size)      转成可读字符串（打日志用）
 *   av_channel_layout_check(&l)                    校验合法性
 * ---------------------------------------------------------------------------
 */
int check_channel_layout(const AVCodec* codec, const AVChannelLayout* layout)
{
    const void* cfgs = nullptr;
    int nb_cfgs = 0;
    const AVChannelLayout* p = nullptr;
    char want[128] = { 0 };

    if (avcodec_get_supported_config(nullptr, codec,
                                     AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0,
                                     &cfgs, &nb_cfgs) < 0) {
        std::fprintf(stderr, "avcodec_get_supported_config(CHANNEL_LAYOUT) failed\n");
        return 0;
    }

    av_channel_layout_describe(layout, want, sizeof(want));

    if (!cfgs) {
        std::printf("  %s: 对声道布局无限制（当前 %s）\n", codec->name, want);
        return 1;
    }

    std::printf("  %s 支持的声道布局（%d 个）: ", codec->name, nb_cfgs);
    for (p = static_cast<const AVChannelLayout*>(cfgs); p->nb_channels != 0; ++p) {
        char tmp[128] = { 0 };
        av_channel_layout_describe(p, tmp, sizeof(tmp));
        std::printf("%s ", tmp);
        /* 注意：这里用 compare，不是 == */
        if (av_channel_layout_compare(p, layout) == 0) {
            std::printf("\n");
            return 1;
        }
    }
    std::printf("\n");
    return 0;
}

/*
 * ===========================================================================
 * 【知识点 3】ADTS 头 —— 手写 7 字节，理解 AAC 的封装
 * ===========================================================================
 * 为什么需要它？
 *   AAC 编码器输出的每个 packet 是「裸 raw_data_block」，里面没有任何
 *   帧同步信息和格式信息。播放器拿到一串字节，既不知道从哪开始是一帧，
 *   也不知道采样率是多少、几个声道。ADTS 就是给每个裸帧前面加一个头，
 *   把这些信息重复携带一遍，让播放器能随机定位、随到随播。
 *
 * 完整 56 bit（=7 字节，无 CRC 时）的位域分布：
 *
 *   ┌───────────────────────────────────────────────────────────────┐
 *   │ syncword            12 bit  = 0xFFF  （帧同步字，用于定位）    │
 *   │ ID                   1 bit  = 0 (MPEG-4) / 1 (MPEG-2)          │
 *   │ layer                2 bit  = 00     （AAC 恒为 0）             │
 *   │ protection_absent    1 bit  = 1      （1=无CRC→头长7字节）     │
 *   ├───────────────────────────────────────────────────────────────┤
 *   │ profile              2 bit  = audioObjectType-1（LC = 1）      │
 *   │ sampling_frequency_index 4 bit（见下面的表）                   │
 *   │ private_bit          1 bit  = 0                                │
 *   │ channel_configuration 3 bit（声道数；>7 时填 0，改用 PCE）     │
 *   ├───────────────────────────────────────────────────────────────┤
 *   │ original/copy        1 bit  = 0                                │
 *   │ home                 1 bit  = 0                                │
 *   │ copyright_id_bit     1 bit  = 0                                │
 *   │ copyright_id_start   1 bit  = 0                                │
 *   │ aac_frame_length    13 bit  = AAC裸数据字节数 + 7（加上头本身）│
 *   │ adts_buffer_fullness 11 bit = 0x7FF（VBR 的约定值）            │
 *   │ number_of_raw_data_blocks 2 bit = 0（表示本头后面只有 1 个块） │
 *   └───────────────────────────────────────────────────────────────┘
 *
 * 采样率索引表（AAC 规范固定，不能自己编号）：
 *   0:96000  1:88200  2:64000  3:48000  4:44100  5:32000  6:24000
 *   7:22050  8:16000  9:12000 10:11025 11:8000 12:7350
 *
 * 【为什么这里必须靠「帧大小」才知道要写几个字节的 AAC 数据】
 *   aac_frame_length 只有 13 bit，最大 8191。
 *   正常 AAC 帧（1024 采样点 @128kbps）约 200~600 字节，远不会溢出；
 *   但「外部输入必须校验」是编码器代码的基本素养，所以下面做了截断保护。
 * ===========================================================================
 */
void get_adts_header(AVCodecContext* ctx, uint8_t* adts_header, int aac_length)
{
    uint8_t freq_idx = 0;

    switch (ctx->sample_rate) {
        case 96000: freq_idx = 0;  break;
        case 88200: freq_idx = 1;  break;
        case 64000: freq_idx = 2;  break;
        case 48000: freq_idx = 3;  break;
        case 44100: freq_idx = 4;  break;
        case 32000: freq_idx = 5;  break;
        case 24000: freq_idx = 6;  break;
        case 22050: freq_idx = 7;  break;
        case 16000: freq_idx = 8;  break;
        case 12000: freq_idx = 9;  break;
        case 11025: freq_idx = 10; break;
        case 8000:  freq_idx = 11; break;
        case 7350:  freq_idx = 12; break;
        default:
            std::fprintf(stderr,
                         "warning: sample_rate %d has no ADTS index, fallback to 44100\n",
                         ctx->sample_rate);
            freq_idx = 4;
            break;
    }

    /*
     * channel_configuration 只有 3 bit，能表示 0~7。
     * AAC 约定：0 表示「声道配置由 PCE（Program Config Element）描述」。
     * 所以超过 7 声道时不能硬塞，要填 0 并在码流里带 PCE。
     */
    uint8_t chan_cfg = static_cast<uint8_t>(ctx->ch_layout.nb_channels);
    if (chan_cfg > 7) {
        std::fprintf(stderr,
                     "warning: %d channels > 7, set channel_configuration=0 (PCE needed)\n",
                     chan_cfg);
        chan_cfg = 0;
    }

    /* aac_frame_length = 本帧 AAC 裸数据 + 7 字节头（13 bit 上限 0x1FFF） */
    uint32_t frame_length = static_cast<uint32_t>(aac_length) + 7u;
    if (frame_length > 0x1FFFu) {
        std::fprintf(stderr,
                     "warning: aac frame %u bytes exceeds ADTS 13-bit limit, truncated\n",
                     frame_length);
        frame_length &= 0x1FFFu;
    }

    adts_header[0] = 0xFF;
    /* [1] = 1111 0001: syncword低4位(1111) + ID=0 + layer=00 + protection_absent=1 */
    adts_header[1] = 0xF1;
    /*
     * [2] 拼三部分：
     *   高 2 bit：profile（注意 AAC 里 profile = audioObjectType - 1，
     *             所以 LC(2) 在这里写 1，正好等于 AV_PROFILE_AAC_LOW）
     *   中 4 bit：采样率索引
     *   低 2 bit：channel_configuration 的高 2 位
     */
    adts_header[2] = static_cast<uint8_t>((ctx->profile << 6) + (freq_idx << 2) + (chan_cfg >> 2));
    /* [3] = channel_configuration 低 2 位 + aac_frame_length 高 3 位 */
    adts_header[3] = static_cast<uint8_t>(((chan_cfg & 3) << 6) + (frame_length >> 11));
    /* [4] = aac_frame_length 的中间 8 位 */
    adts_header[4] = static_cast<uint8_t>((frame_length & 0x7FF) >> 3);
    /* [5] = aac_frame_length 低 3 位 + adts_buffer_fullness 高 5 位(全1) */
    adts_header[5] = static_cast<uint8_t>(((frame_length & 7) << 5) + 0x1F);
    /* [6] = adts_buffer_fullness 低 6 位(全1) + number_of_raw_data_blocks(00) */
    adts_header[6] = 0xFC;
}

/*
 * 全局帧序号，只用于控制日志输出量（前 3 帧打印包详情，之后静默）。
 */
int64_t g_frame_index = 0;

/*
 * ===========================================================================
 * 【知识点 4】编码主循环 —— send / receive 的配对规则
 * ===========================================================================
 * 这是 libavcodec 唯一正确的编码 API 用法：
 *
 *     avcodec_send_frame(ctx, frame);         送 1 次
 *     while (1) {
 *         ret = avcodec_receive_packet(ctx, pkt);   收 N 次
 *         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;  // 正常结束
 *         if (ret < 0) { ...真错误... }
 *         ...消费 pkt...
 *     }
 *
 * 【六条必须理解的规则】
 *
 *  (1) 为什么是 1 送 N 收，而不是「送一帧得一帧」？
 *      因为编码器内部有缓存和 look-ahead。以 AAC 为例，它要攒够 1024 个
 *      采样点才能做一次 MDCT，所以：
 *        第 1 次 send(1024点) → receive 返回 EAGAIN，一个包都没有
 *        第 2 次 send(1024点) → receive 返回 pts=-1024 的 priming 帧
 *      从「packet 数」和「frame 数」对不上，是完全正常的。
 *
 *  (2) AVERROR(EAGAIN) 是「现在没有，请再送输入」，不是错误。
 *      AVERROR_EOF 是「冲刷完毕，以后也不会再有了」。
 *      这两个都必须当正常流程处理，只 break 不报错。
 *
 *  (3) 冲刷（flush）：send 一个 NULL frame（C++ 里写 nullptr）。
 *      编码器会把内部残留的采样点全部编码出来（AAC 会吐 1~2 个包，
 *      最后一个往往只有几个字节的 padding）。
 *      不冲刷的话，最后 <1024 个采样点会永久丢失。
 *
 *  (4) avcodec_receive_packet() 内部第一步是 av_packet_unref(pkt)，
 *      意思是「把你给我的这个 pkt 清空，我要往里填新数据」。
 *      带来两个后果：
 *        - 循环里**不需要**手动 av_packet_unref(pkt)；
 *        - pkt 的 buffer 会被复用，所以**不能**把 pkt 直接推进队列，
 *          跨线程传递前必须先 av_packet_move_ref() / av_packet_ref()
 *          自己拷一份引用出来。
 *
 *  (5) 返回值 < 0 才是错误，ret >= 0 表示「本次成功拿到一个包」，
 *      拿到包之后必须 continue 继续收，不能 break。
 *
 *  (6) frame == nullptr 只用于冲刷，不要拿它当「送一个空帧」用。
 *
 *  【补充】本函数带一个 flushed 标志：
 *      一旦送过 nullptr（冲刷），编码器就进入 EOF 状态，
 *      之后再 send 任何非空帧都会返回 AVERROR_EOF。
 *      所以记住「已经冲刷过」这个状态，避免调用方重复冲刷时刷出
 *      "avcodec_send_frame failed: End of file" 这种看着像错误的日志。
 *      （这个坑在 08-03 封装示例里实测踩到过。）
 * ===========================================================================
 */
struct EncodeStats {
    int64_t packets   = 0;   /* 累计输出包数 */
    int64_t bytes     = 0;   /* 累计输出字节数 */
    int64_t keyFrames = 0;   /* 关键帧数（ADTS 下每包都是独立帧，这里只统计首包） */
};

int encode(AVCodecContext* ctx, AVFrame* frame, AVPacket* pkt,
           FILE* output, bool write_adts, bool* flushed, EncodeStats* stats)
{
    int ret;
    int nb_out = 0;                      /* 本次 send 之后收到几个包 */
    int64_t first_pts = AV_NOPTS_VALUE;  /* 其中第一个包的 pts */

    /* 已经冲刷过就只收包，不再送（见上面第 6 条的补充说明） */
    if (*flushed)
        frame = nullptr;
    else if (!frame)
        *flushed = true;

    /* ---- 送一帧（或 nullptr 表示冲刷） ---- */
    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        std::fprintf(stderr, "avcodec_send_frame failed: %s\n", errStr(ret).c_str());
        return -1;
    }

    /* ---- 收所有能收到的包 ---- */
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            /* 正常结束。若一个包都没收到，说明编码器还在攒数据 */
            if (nb_out == 0)
                std::printf("  (编码器缓冲中：本次没有输出包，需要更多输入)\n");
            else if (frame == nullptr)
                std::printf("  冲刷完成：本次 %d 个包，首包 pts=%" PRId64 "\n",
                            nb_out, first_pts);
            return 0;
        } else if (ret < 0) {
            std::fprintf(stderr, "avcodec_receive_packet failed: %s\n", errStr(ret).c_str());
            return -1;
        }

        if (first_pts == AV_NOPTS_VALUE)
            first_pts = pkt->pts;

        /*
         * pkt 字段含义：
         *   data/size : 编码后的裸 AAC 数据及其字节数
         *   pts       : 这个包对应第一个采样点在时间轴上的位置（单位 = 采样点）
         *               第一包是 -1024（priming），第二包才是 0
         *   dts       : 解码时间戳；音频无 B 帧，通常和 pts 相同
         *   flags     : AV_PKT_FLAG_KEY 等；AAC 每帧都是关键帧
         *   duration  : 本包覆盖多少个采样点（= frame_size）
         *
         * 注意：下面用到的是**本次循环新拿到的** pkt，字段是有效的；
         *       一旦调用 av_interleaved_write_frame 之类的函数，
         *       pkt 会被 unref 清空（size=0/pts=NOPTS），那之后再读就晚了。
         *       本程序自己 fwrite，不经过 muxer，所以没这个问题。
         */
        if (!frame || g_frame_index <= 3)
            std::printf("  pkt[%2d] size=%4d pts=%-8" PRId64 " dts=%-8" PRId64
                        " duration=%d flags=0x%x\n",
                        nb_out + 1, pkt->size, pkt->pts, pkt->dts,
                        pkt->duration, pkt->flags);

        size_t len = 0;

        if (write_adts) {
            /*
             * 【知识点 5】AV_CODEC_FLAG_GLOBAL_HEADER 的准确语义
             * -----------------------------------------------------------------
             * 含义：告诉编码器「把全局头（对 AAC 是 AudioSpecificConfig）
             *       从码流里挪出去，由封装层统一写一次」。
             * 对 AAC 的效果：每个 packet 里只有裸 raw_data_block，不带 ADTS。
             *
             * 为什么本程序要主动设这个 flag？
             *   native aac 编码器默认就不带 ADTS；libfdk_aac 默认带 ADTS。
             *   实测两者 ctx->flags 都是 0，没法靠 flags 区分谁带谁不带。
             *   所以主动置上这个 flag，让两边行为统一（都不带），
             *   再由本程序自己补 ADTS —— 输出就是一个 ffplay 能直接播的文件。
             *
             * 注意：必须在 avcodec_open2() 之前设置，之后再改无效。
             * -----------------------------------------------------------------
             */
            uint8_t aac_header[7];
            get_adts_header(ctx, aac_header, pkt->size);
            len = std::fwrite(aac_header, 1, 7, output);
            if (len != 7) {
                std::fprintf(stderr, "fwrite aac_header failed\n");
                return -1;
            }
            stats->bytes += 7;
        }

        /* 写真正的编码数据 */
        len = std::fwrite(pkt->data, 1, pkt->size, output);
        if (len != static_cast<size_t>(pkt->size)) {
            std::fprintf(stderr, "fwrite aac data failed\n");
            return -1;
        }

        stats->bytes += pkt->size;
        ++stats->packets;
        ++nb_out;

        /*
         * 这里不需要 av_packet_unref(pkt)：
         * 下一次 avcodec_receive_packet() 内部会自己 unref。
         * 但正因为如此，pkt->data 在下一次 receive 时就失效了 ——
         * 要长期持有必须 av_packet_ref() 或 av_packet_move_ref() 拷一份。
         */
    }
    return -1;
}

/*
 * ===========================================================================
 * 【知识点 6】packed ↔ planar —— 采样点在内存里怎么排
 * ===========================================================================
 * 同一个立体声瞬间的两个采样点，有两种排布方式：
 *
 *   packed（交错 / interleaved）：所有声道的数据交替存放
 *       s16le / f32le 就是这种
 *       内存:  L0 R0 L1 R1 L2 R2 L3 R3 ...
 *       特点:  单块内存，data[0] 指向全部，linesize = 声道数 × 每点字节数 × 点数
 *
 *   planar（分平面 / planar）：每个声道各占一块连续内存
 *       fltp / s16p 就是这种（名字末尾带 p = planar）
 *       内存:  data[0] → L0 L1 L2 L3 ...
 *              data[1] → R0 R1 R2 R3 ...
 *       特点:  每声道一个 buffer，data[i] 各指一块，
 *              linesize[0] = 单声道每行的字节数（= 每点字节数 × 点数）
 *
 * 判断方式：av_sample_fmt_is_planar(fmt) 返回 1 表示 planar。
 *
 * 【为什么必须转换】
 *   ffmpeg 命令行只能导出 packed PCM：
 *       ffmpeg -i x.aac -ar 48000 -ac 2 -f f32le out.pcm     # L R L R ...
 *   而 native AAC 编码器只吃 AV_SAMPLE_FMT_FLTP（planar）：
 *       只接受一个采样格式，见 avcodec_get_supported_config 的输出
 *   所以「packed → planar」这一步是所有 FFmpeg 音频编码入门代码都要做的。
 *
 * 【做法 A：手写转换（本函数）】
 *   优点：直观，能看清 planar 的排布，零依赖
 *   缺点：只能处理「2 声道 + f32le + 采样率不变」这一种情况
 *
 * 【做法 B：libswresample（生产代码唯一正确做法，见 create_swr）】
 *   一次解决三件事：采样格式（含 packed/planar）、声道布局、采样率
 * ===========================================================================
 */
void f32le_convert_to_fltp(const float* f32le, float* fltp, int nb_samples)
{
    /* planar 的左声道从 fltp 头开始，右声道从第 nb_samples 个 float 开始 */
    float* fltp_l = fltp;
    float* fltp_r = fltp + nb_samples;

    for (int i = 0; i < nb_samples; ++i) {
        fltp_l[i] = f32le[i * 2];        /* packed 下标 2i   → planar 左 */
        fltp_r[i] = f32le[i * 2 + 1];    /* packed 下标 2i+1 → planar 右 */
        /* 试试把其中一行注释掉，可以听出「只有一边有声音」的效果 */
    }
}

/*
 * ===========================================================================
 * 【知识点 7】用 libswresample 做格式转换（s16le → fltp 等）
 * ===========================================================================
 * libswresample（swr）是 FFmpeg 官方的音频重采样/格式转换库。
 * 它一次能同时完成三种转换，这也是它比手写强的地方：
 *
 *     采样格式  s16 ↔ flt ↔ fltp      （含 packed/planar 布局变化）
 *     声道布局  mono ↔ stereo ↔ 5.1   （含上混/下混矩阵）
 *     采样率    44100 ↔ 48000          （含抗混叠滤波）
 *
 * 标准三步：
 *     swr_alloc_set_opts2(&swr, &out_layout, out_fmt, out_rate,
 *                               &in_layout,  in_fmt,  in_rate, 0, NULL);
 *     swr_init(swr);
 *     每次：swr_convert(swr, out_data, out_count, in_data, in_count);
 *
 * 【三个必须注意的细节】
 *
 *  (1) swr_alloc_set_opts2 的第一个参数是二级指针，必须传 &swr。
 *      它内部会自己分配 SwrContext。
 *
 *  (2) 输入/输出的 AVChannelLayout 必须分开传。旧 API（swr_alloc_set_opts）
 *      用的是 uint64_t 掩码，新 API 换成了结构体。
 *      注意栈上的 in_layout 用完要 av_channel_layout_uninit()。
 *
 *  (3) swr_convert 的**返回值是实际输出的采样点数，不等于输入采样点数**。
 *      因为 swr 内部带 FIFO：当输入/输出采样率不同，或者内部滤波器需要
 *      历史数据时，它会把尾巴缓存起来下次再吐。
 *      所以绝不能写 frame->nb_samples = nb_in_samples，
 *      必须 frame->nb_samples = swr_convert(...) 的返回值。
 *      想知道「最多可能吐出多少」，用 swr_get_out_samples(swr, in_samples)。
 * ===========================================================================
 */

/* 创建重采样上下文：把 (in_fmt, in_channels, in_rate) 的 PCM 转成编码器要求的格式 */
SwrContext* create_swr(enum AVSampleFormat in_fmt, int in_channels, int in_rate,
                       const AVCodecContext* enc_ctx)
{
    SwrContext* swr = nullptr;
    AVChannelLayout in_layout;
    int ret;

    /* 用「声道数」推出标准布局（2 → stereo）。这是本程序唯一能做的假设。 */
    av_channel_layout_default(&in_layout, in_channels);

    ret = swr_alloc_set_opts2(&swr,
                              /* 目标：编码器要求的 */
                              &enc_ctx->ch_layout, enc_ctx->sample_fmt, enc_ctx->sample_rate,
                              /* 源：PCM 文件实际的 */
                              &in_layout,          in_fmt,             in_rate,
                              0, nullptr);
    av_channel_layout_uninit(&in_layout);   /* 栈上布局用完必须释放 */

    if (ret < 0 || !swr) {
        std::fprintf(stderr, "swr_alloc_set_opts2 failed\n");
        return nullptr;
    }

    /* swr_init 会真正计算重采样滤波器系数，比较耗时，不要放在循环里 */
    if ((ret = swr_init(swr)) < 0) {
        std::fprintf(stderr, "swr_init failed: %s\n", errStr(ret).c_str());
        swr_free(&swr);
        return nullptr;
    }

    std::printf("swr 已建立：%s/%dch/%dHz  ->  %s/%dch/%dHz\n",
                av_get_sample_fmt_name(in_fmt), in_channels, in_rate,
                av_get_sample_fmt_name(enc_ctx->sample_fmt),
                enc_ctx->ch_layout.nb_channels, enc_ctx->sample_rate);
    return swr;
}

/*
 * 把命令行里 `ffmpeg -f` 后面的格式名翻译成 AVSampleFormat。
 *
 * 【知识点：名字对照关系】
 *   ffmpeg 命令行        AVSampleFormat          内存布局
 *   ─────────────       ─────────────────       ──────────
 *   s16le               AV_SAMPLE_FMT_S16       16位有符号整数，packed
 *   s32le               AV_SAMPLE_FMT_S32       32位有符号整数，packed
 *   f32le / flt         AV_SAMPLE_FMT_FLT       32位浮点，packed
 *   s16p                AV_SAMPLE_FMT_S16P      16位整数，planar
 *   fltp                AV_SAMPLE_FMT_FLTP      32位浮点，planar
 *
 * 为什么没有 le/be 后缀？
 *   因为文件里的字节序就是本机字节序（x86 是小端），FFmpeg 内部不做字节序
 *   转换，所以名字里不需要区分。跨平台传文件时才需要考虑。
 *
 * 【为什么 AAC 用 float 而不是 int16？】
 *   MDCT 变换和量化都在浮点域做，精度更高。编码器内部即使收 s16 也要先转
 *   成浮点，所以直接给 fltp 可以少一次转换。
 */
enum AVSampleFormat parse_in_sample_fmt(const std::string& name)
{
    if (name.empty())            return AV_SAMPLE_FMT_FLT;
    if (name == "s16")           return AV_SAMPLE_FMT_S16;
    if (name == "s16le")         return AV_SAMPLE_FMT_S16;
    if (name == "s32")           return AV_SAMPLE_FMT_S32;
    if (name == "flt")           return AV_SAMPLE_FMT_FLT;
    if (name == "f32")           return AV_SAMPLE_FMT_FLT;
    if (name == "f32le")         return AV_SAMPLE_FMT_FLT;
    if (name == "fltp")          return AV_SAMPLE_FMT_FLTP;
    if (name == "s16p")          return AV_SAMPLE_FMT_S16P;
    /* 兜底：交给 FFmpeg 按名字解析（av_get_sample_fmt 需要精确名字） */
    return av_get_sample_fmt(name.c_str());
}

void usage(const char* prog)
{
    std::fprintf(stderr,
        "用法: %s <input_pcm> <output_file> [codec_name] [sample_rate] [channels] [sample_fmt] [noadts]\n"
        "  codec_name  留空或 '-' = 按 ID 找默认 aac；也可给 libfdk_aac（需 FFmpeg 编进该库）\n"
        "  sample_rate 默认 48000\n"
        "  channels    默认 2\n"
        "  sample_fmt  默认 f32le（可用 s16le / s32 / f32le / flt / fltp / s16p）\n"
        "  noadts      输出纯裸 AAC（不加 7 字节 ADTS 头）\n"
        "例: %s 48000_2_f32le.pcm aac.aac\n"
        "    %s 48000_2_s16le.pcm aac.aac aac 48000 2 s16le\n"
        "    %s 48000_2_f32le.pcm aac.raw aac 48000 2 f32le noadts\n",
        prog, prog, prog, prog);
}

} // namespace

/*
 * 提取测试文件的方法（用 ffmpeg 命令行把参考 aac 转成裸 PCM）：
 *   s16 格式：ffmpeg -i buweishui.aac -ar 48000 -ac 2 -f s16le 48000_2_s16le.pcm
 *   flt 格式：ffmpeg -i buweishui.aac -ar 48000 -ac 2 -f f32le 48000_2_f32le.pcm
 *   注意：ffmpeg 只能导出 packed 的 PCM，所以要编码成 fltp 必须自己转。
 */
int main(int argc, char** argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    /* ---------- 第 1 步：解析参数 ---------- */
    EncodeConfig cfg;
    cfg.inPcmFile = argv[1];
    cfg.outFile   = argv[2];

    if (argc >= 4 && std::strcmp(argv[3], "-") != 0)
        cfg.codecName = argv[3];
    if (argc >= 5)
        cfg.sampleRate = std::atoi(argv[4]);
    if (argc >= 6)
        cfg.channels = std::atoi(argv[5]);
    if (argc >= 7)
        cfg.inSampleFmt = parse_in_sample_fmt(argv[6]);
    if (argc >= 8 && std::strcmp(argv[7], "noadts") == 0)
        cfg.writeAdts = false;

    if (cfg.sampleRate <= 0 || cfg.channels <= 0) {
        std::fprintf(stderr, "参数非法: sample_rate=%d channels=%d\n",
                     cfg.sampleRate, cfg.channels);
        return 1;
    }

    std::printf("输入 PCM : %s\n", cfg.inPcmFile.c_str());
    std::printf("  %d Hz, %d 声道, %s (%s)\n",
                cfg.sampleRate, cfg.channels,
                av_get_sample_fmt_name(cfg.inSampleFmt),
                av_sample_fmt_is_planar(cfg.inSampleFmt) ? "planar 分平面" : "packed 交错");
    std::printf("输出    : %s (%s)\n\n", cfg.outFile.c_str(),
                cfg.writeAdts ? "带 ADTS 头，可直接播放" : "纯裸 AAC，无 ADTS 头");

    /*
     * =======================================================================
     * 【知识点 28】为什么把主体塞进一个立即执行的 lambda
     * =======================================================================
     * 原 C 版本用 `goto cleanup` 做统一收尾。这在 C 里没问题，但 C++ 有一条
     * 硬性规定：**goto 不能跳过带初始化的变量声明**
     * （错误信息形如 "jump to label crosses initialization of ..."）。
     * 本程序主体里有大量 `int frame_bytes = ...` 这类声明，
     * 所以直接照搬 goto 会编译不过（08-02/08-03 都实测撞过）。
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
        FILE*           infile    = nullptr;
        FILE*           outfile   = nullptr;
        const AVCodec*  codec     = nullptr;
        AVCodecContext* codec_ctx = nullptr;
        AVFrame*        frame     = nullptr;
        AVPacket*       pkt       = nullptr;
        SwrContext*     swr       = nullptr;
        int             ret       = 0;
        bool            flushed   = false;
        EncodeStats     stats;

        /*
         * ===================================================================
         * 【知识点 8】第 2 步：查找编码器
         * ===================================================================
         * 两种查找方式，区别很重要：
         *
         *   avcodec_find_encoder(AV_CODEC_ID_AAC)
         *       按「编码格式 ID」找，返回注册表里第一个匹配的编码器。
         *       同一个 ID 可能有多个实现（aac / libfdk_aac / aac_mf / aac_at），
         *       返回哪个取决于编译顺序，**不可控**。
         *
         *   avcodec_find_encoder_by_name("aac")
         *       按 AVCodec.name 精确找，结果**可控**。
         *       生产代码推荐用这个，因为你清楚自己要用哪个实现。
         *
         * 返回值是 const AVCodec*，指向 libavcodec 的**全局静态注册表**，
         * 不需要也不允许 free。返回 nullptr 表示「没编进这个编码器」。
         *
         * 用 `ffmpeg -encoders | findstr aac` 可以看本机有哪些。
         * 本机 FFmpeg 8.1 是 --disable-libfdk-aac 编译的，所以只有 aac / aac_mf。
         * =================================================================== */
        if (cfg.codecName.empty()) {
            codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
            std::printf("按 ID 查找编码器 (AV_CODEC_ID_AAC)\n");
        } else {
            codec = avcodec_find_encoder_by_name(cfg.codecName.c_str());
            std::printf("按名字查找编码器 (\"%s\")\n", cfg.codecName.c_str());
        }
        if (!codec) {
            std::fprintf(stderr,
                         "找不到编码器。请用 `ffmpeg -encoders | findstr aac` 确认名字；\n"
                         "libfdk_aac 需要 --enable-libfdk-aac 编译的 FFmpeg。\n");
            return 1;
        }
        std::printf("  → 找到 %s：%s\n", codec->name, codec->long_name);
        std::printf("  → capabilities=0x%x (%s%s)\n", codec->capabilities,
                    (codec->capabilities & AV_CODEC_CAP_DELAY) ? "有内部延迟 " : "",
                    (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) ? "实验性 " : "");

        /*
         * ===================================================================
         * 【知识点 9】第 3 步：分配编码器上下文
         * ===================================================================
         * avcodec_alloc_context3(codec) 做的事：
         *   1. 分配 AVCodecContext
         *   2. 调用 avcodec_get_context_defaults3()，用 codec 的信息填入默认值
         *      —— 注意这一步已经把 codec_id / codec_type 填好了，
         *         所以**不需要**再手写 `ctx->codec_id = ...; ctx->codec_type = ...`
         *   3. 按 codec 的 priv_data_size 分配私有数据区（编码器自己的参数存在这里）
         *
         * 对比旧的 avcodec_alloc_context()：那个不带 codec 参数，
         * 所有东西都要手动填，容易漏，已废弃。**永远用 alloc_context3**。
         * =================================================================== */
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::fprintf(stderr, "avcodec_alloc_context3 failed\n");
            return 1;
        }

        /* ---------- 第 4 步：设置编码参数 ----------
         *
         * 【知识点 10】码率 bit_rate
         *   单位是 bit/s，不是 Byte/s。128*1024 = 131072 bit/s = 128 kbps。
         *   对 AAC，这是「目标平均码率」；编码器用 ABR 策略尽量贴近它。
         *   码率越高音质越好、文件越大，但收益递减：
         *       64 kbps  ≈ 勉强可用（立体声会有明显损失）
         *       128 kbps ≈ 透明（大部分人听不出与原始的区别）← 本程序选的
         *       256 kbps ≈ 对绝大多数内容已无意义
         *   注意：实际码率会围绕目标值波动，ffprobe 看到的 130 kbps 是正常的。
         *
         * 【知识点 11】profile —— AAC 的「档次」
         *   AV_PROFILE_AAC_LOW   (1)  = AAC-LC，最常用，兼容性最好
         *   AV_PROFILE_AAC_MAIN  (0)  = AAC-Main，多了反向预测，几乎没人用
         *   AV_PROFILE_AAC_SSR   (2)  = AAC-SSR，低延迟场景
         *   AV_PROFILE_AAC_LTP   (3)  = AAC-LTP，长时预测
         *   AV_PROFILE_AAC_HE    (4)  = HE-AAC (SBR)，低码率下补高频
         *   AV_PROFILE_AAC_HE_V2 (28) = HE-AACv2 (SBR+PS)，更低码率
         *   记住：profile 的值就是 AAC 规范里的 audioObjectType - 1，
         *         这也正是 ADTS 头里 profile 字段要填的值。
         */
        codec_ctx->bit_rate    = 128 * 1024;
        codec_ctx->sample_rate = cfg.sampleRate;
        codec_ctx->profile     = AV_PROFILE_AAC_LOW;

        /*
         * 【知识点 12】声道布局怎么设 —— 服务端只要声道数就能推
         *
         * av_channel_layout_default(&ctx->ch_layout, nb_channels)
         *   按「声道数」给一个标准布局：
         *       1 → mono          2 → stereo
         *       3 → 2.1           4 → quadro / 4.0
         *       6 → 5.1           8 → 7.1
         *   这是最省事的做法，也是本程序用的。
         *
         * 如果来源明确（比如设备报的就是 stereo），更准确的做法是：
         *   av_channel_layout_from_mask(&ctx->ch_layout, AV_CH_LAYOUT_STEREO);
         *   av_channel_layout_from_string(&ctx->ch_layout, "5.1(side)");
         *
         * 【为什么不能两个声道都叫「2 声道」就完事】
         *   2 声道的 stereo（L/R）和 2 声道的 downmix（SL/SR）含义不同，
         *   6 声道是 5.1 还是 6.0 顺序也不同。布局信息会影响编码器的
         *   声道耦合决策，也影响解码端的下混，所以必须给对。
         */
        av_channel_layout_default(&codec_ctx->ch_layout, cfg.channels);

        /*
         * 【知识点 13】sample_fmt 必须匹配编码器的要求
         *   每个编码器只接受特定的一组采样格式。AAC 只有 fltp。
         *   这里按编码器名字决定：
         *       native aac   → AV_SAMPLE_FMT_FLTP  (float planar)
         *       libfdk_aac   → AV_SAMPLE_FMT_S16
         *   更严谨的做法是从 check_sample_fmt() 的结果里挑，
         *   但按名字分支更直观，也符合「人知道自己在用什么编码器」的场景。
         */
        if (std::strcmp(codec->name, "aac") == 0)
            codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        else if (std::strcmp(codec->name, "libfdk_aac") == 0)
            codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        else
            codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

        /*
         * ===================================================================
         * 【知识点 14】第 5 步：先查能力，再打开
         * ===================================================================
         * 顺序不能颠倒：avcodec_open2 之前问编码器「你支持什么」，
         * 把参数调成它支持的子集，再去 open，才能一次成功。
         * 如果直接 open，失败时只会得到一个笼统的 EINVAL，很难定位。
         * =================================================================== */
        std::printf("\n查询编码器能力：\n");
        if (!check_sample_fmt(codec, codec_ctx->sample_fmt)) {
            std::fprintf(stderr, "编码器不支持采样格式 %s\n",
                         av_get_sample_fmt_name(codec_ctx->sample_fmt));
            return 1;
        }
        if (!check_sample_rate(codec, codec_ctx->sample_rate)) {
            std::fprintf(stderr, "编码器不支持采样率 %d\n", codec_ctx->sample_rate);
            return 1;
        }
        if (!check_channel_layout(codec, &codec_ctx->ch_layout)) {
            char buf[128] = { 0 };
            av_channel_layout_describe(&codec_ctx->ch_layout, buf, sizeof(buf));
            std::fprintf(stderr, "编码器不支持声道布局 %s\n", buf);
            return 1;
        }

        /*
         * 【知识点 15】AV_CODEC_FLAG_GLOBAL_HEADER 在此设置（必须在 open 之前）
         * 语义与理由见 encode() 里的详细说明。
         */
        codec_ctx->flags = AV_CODEC_FLAG_GLOBAL_HEADER;

        std::printf("\n编码参数（open 之前）：\n");
        std::printf("  bit_rate    : %" PRId64 " bps = %" PRId64 " kbps\n",
                    codec_ctx->bit_rate, codec_ctx->bit_rate / 1024);
        std::printf("  sample_rate : %d Hz\n", codec_ctx->sample_rate);
        std::printf("  sample_fmt  : %s\n", av_get_sample_fmt_name(codec_ctx->sample_fmt));
        {
            char buf[128] = { 0 };
            av_channel_layout_describe(&codec_ctx->ch_layout, buf, sizeof(buf));
            std::printf("  ch_layout   : %s (%d 声道)\n", buf, codec_ctx->ch_layout.nb_channels);
        }
        std::printf("  profile     : %d (AAC-LC)\n", codec_ctx->profile);
        std::printf("  frame_size  : %d  ← open 之前是 0，还没有意义\n\n",
                    codec_ctx->frame_size);

        /*
         * ===================================================================
         * 【知识点 16】第 6 步：avcodec_open2 —— 真正初始化编码器
         * ===================================================================
         * 它做的事：
         *   - 校验所有参数组合是否合法（不合法就返回负错误码）
         *   - 打开编码器私有的 AVCodecContext（分配 FFT/MDCT 表、量化表等）
         *   - **回填 frame_size**：告诉你「每次要喂多少个采样点」
         *   - 回填 initial_padding：编码器开头插入多少 priming 采样点
         *
         * 第三个参数是 AVDictionary **，用来传编码器私有选项，例如：
         *     AVDictionary *opts = nullptr;
         *     av_dict_set(&opts, "aac_coder", "fast", 0);
         *     avcodec_open2(ctx, codec, &opts);
         *     av_dict_free(&opts);      ← 用完必须 free
         * 本程序不传，给 nullptr。想看某个编码器有哪些选项：
         *     ffmpeg -h encoder=aac
         *
         * 【frame_size 是本知识点最重要的产出】
         *   AAC 的 frame_size 恒为 1024。它的物理含义是：
         *   AAC 每帧在时域上覆盖 1024 个采样点（约 21.3 ms @48kHz），
         *   做一次 MDCT 得到 1024 个频域系数，量化编码后就是「一个 AAC 帧」。
         *   所以你送 1024 个点进去，它吐出一个包；送 4096 个点，它吐四个包。
         *   这就是下面「每次读 frame_size 个采样点」的由来。
         * =================================================================== */
        if ((ret = avcodec_open2(codec_ctx, codec, nullptr)) < 0) {
            std::fprintf(stderr, "avcodec_open2 failed: %s\n", errStr(ret).c_str());
            return 1;
        }

        std::printf("编码参数（open 之后）：\n");
        std::printf("  frame_size      : %d  ← 每帧要喂的采样点数（AAC 恒为 1024）\n",
                    codec_ctx->frame_size);
        std::printf("  delay           : %d  ← 编码器算法性前瞻延迟（look-ahead）\n",
                    codec_ctx->delay);
        std::printf("  initial_padding : %d  ← 编码器开头插入的 priming 采样点数\n\n",
                    codec_ctx->initial_padding);
        /*
         * 【知识点 16.1】delay 和 initial_padding 是两回事，别混淆
         *   delay           : 编码器为了做变换需要「往前看」多少采样点，
         *                     这部分数据会被缓存，导致输出滞后。
         *   initial_padding : 为了让解码端能正确重建开头，编码器需要在
         *                     最前面插入多少采样点的 priming 数据，
         *                     相应的 pts 会从 -initial_padding 开始。
         *
         *   native AAC 编码器报的是 delay=0 / initial_padding=1024，
         *   所以你会看到第一个包的 pts = -1024 —— 这正好对上。
         *   有些编码器（如 MP3/lame）报 delay，有些报 initial_padding，
         *   写通用封装时两个都要考虑。
         */

        /* ---------- 打开输入输出文件 ---------- */
        /* 用标准 C 的 FILE 而不是 libavformat，是为了把注意力集中在编码本身。
         * 真实项目应该用 avformat（见文末「进一步学习」）。 */
        infile = std::fopen(cfg.inPcmFile.c_str(), "rb");
        if (!infile) {
            std::fprintf(stderr, "打不开输入文件 %s\n", cfg.inPcmFile.c_str());
            return 1;
        }
        outfile = std::fopen(cfg.outFile.c_str(), "wb");
        if (!outfile) {
            std::fprintf(stderr, "打不开输出文件 %s\n", cfg.outFile.c_str());
            return 1;
        }

        /*
         * ===================================================================
         * 【知识点 17】第 7 步：AVPacket —— 装「编码后的数据」
         * ===================================================================
         * AVPacket 是压缩数据的容器（编码器的输出 / 解码器的输入）。
         * 它的 data 指向一块引用计数（AVBufferRef）的内存，
         * 可以零拷贝地在多个 packet 之间共享。
         *
         * 用 av_packet_alloc() 分配，av_packet_free(&pkt) 释放（传二级指针）。
         * 不要用 av_init_packet()（已废弃），也不要自己 malloc。
         * =================================================================== */
        pkt = av_packet_alloc();
        if (!pkt) {
            std::fprintf(stderr, "av_packet_alloc failed\n");
            return 1;
        }

        /*
         * ===================================================================
         * 【知识点 18】第 8 步：AVFrame —— 装「一帧原始采样」
         * ===================================================================
         * AVFrame 是未压缩数据的容器（编码器的输入 / 解码的输出）。
         *
         * 一帧音频由三要素完全确定：
         *     nb_samples 每声道多少个采样点   （AAC = 1024）
         *     format     采样格式              （AAC = fltp）
         *     ch_layout  声道布局              （stereo）
         * 换算出内存占用：
         *     总字节 = av_get_bytes_per_sample(format)
         *              × nb_channels × nb_samples
         *   对 fltp/2ch/1024：4 × 2 × 1024 = 8192 字节
         *   但这 8192 字节分在两块内存里（每块 4096），即 data[0] 和 data[1]。
         * =================================================================== */
        frame = av_frame_alloc();
        if (!frame) {
            std::fprintf(stderr, "av_frame_alloc failed\n");
            return 1;
        }

        frame->nb_samples = codec_ctx->frame_size;   /* 1024 */
        frame->format     = codec_ctx->sample_fmt;   /* FLTP */

        /*
         * 【知识点 19】frame 的声道布局必须用 copy，不能按值赋值
         *   原因见知识点 2.1 的「三条铁律」：
         *   AVChannelLayout 在 CUSTOM 顺序时持有堆内存，浅拷贝会导致 double free。
         *   av_channel_layout_copy() 会正确处理（NATIVE 布局直接拷掩码，
         *   CUSTOM 布局则深拷贝 map 数组）。
         *   返回值必须检查，失败时 frame->ch_layout 是无效的。
         */
        if ((ret = av_channel_layout_copy(&frame->ch_layout, &codec_ctx->ch_layout)) < 0) {
            std::fprintf(stderr, "av_channel_layout_copy failed: %s\n", errStr(ret).c_str());
            return 1;
        }

        std::printf("分配 AudioFrame：\n");
        std::printf("  nb_samples : %d\n", frame->nb_samples);
        std::printf("  format     : %s\n",
                    av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)));
        {
            char buf[128] = { 0 };
            av_channel_layout_describe(&frame->ch_layout, buf, sizeof(buf));
            std::printf("  ch_layout  : %s (%d 声道)\n", buf, frame->ch_layout.nb_channels);
        }

        /*
         * 【知识点 20】av_frame_get_buffer —— 分配真正的数据内存
         *
         * 参数 2 是 align（对齐字节数），传 0 表示用默认值：
         *   - 0 通常会被解析成 32 或 64 字节对齐
         *   - 对齐是为了让 SIMD 指令（SSE/AVX）能一次读一整块内存，
         *     不对齐会大幅降低性能，甚至在某些平台直接崩
         *   - 只有在特殊场景（比如从设备读取必须紧凑排列的数据）才手动传对齐值
         *
         * 这个函数会根据 frame->format / nb_samples / ch_layout 自动算出
         * 需要多少内存，并且：
         *   - 为 planar 格式分配 nb_channels 个独立 buffer
         *   - 填好 frame->data[i] 指针和 frame->linesize[i]
         *   - 内存是引用计数的（AVBufferRef），av_frame_free 时会自动释放
         *
         * 调用前必须先把 nb_samples / format / ch_layout 三个字段设好，
         * 否则算出来的大小是错的。
         */
        if ((ret = av_frame_get_buffer(frame, 0)) < 0) {
            std::fprintf(stderr, "av_frame_get_buffer failed: %s\n", errStr(ret).c_str());
            return 1;
        }

        /*
         * 【知识点 21】linesize 到底是什么
         *   packed 格式：linesize[0] = 一行（所有声道）的字节数
         *   planar 格式：linesize[0] = 一行（单个声道）的字节数
         *                各平面大小相同，都等于 linesize[0]
         *
         * 为什么会有「行」的概念？因为对齐要求可能带来 padding：
         *   本帧每声道需要 4 × 1024 = 4096 字节，正好对齐，所以 linesize[0] == 4096。
         *   但如果 nb_samples 不是 8 的倍数、或格式是 s16，对齐后
         *   linesize 就会**大于**实际需要的字节数，多出来的是 padding。
         *
         * ⚠ 容易踩的坑：对音频，**不要**用 linesize[1] 当作「第 2 声道的大小」。
         *   av_frame_get_buffer 对音频只把 data[i] 填好，linesize 只在
         *   linesize[0] 里有意义（各平面等长）。用 linesize[1] 算偏移会读到垃圾值。
         *   正确做法：平面字节数 = nb_samples × av_get_bytes_per_sample(format)。
         */
        const int plane_bytes = frame->nb_samples
                    * av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));
        std::printf("  data[0]=%p  linesize[0]=%d\n",
                    static_cast<void*>(frame->data[0]), frame->linesize[0]);
        if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format)))
            std::printf("  data[1]=%p  每个平面 %d 字节  (planar：每声道一块内存)\n",
                        static_cast<void*>(frame->data[1]), plane_bytes);

        /* 每帧总字节数（所有平面加起来） */
        const int frame_bytes = av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format))
                              * frame->ch_layout.nb_channels
                              * frame->nb_samples;
        const int nb_in_samples = frame->nb_samples;   /* 每次从文件读多少个采样点 */
        /* 输入侧一行 PCM 的字节数：按「输入格式」算，不能用 frame->format 算 */
        const int in_frame_bytes = av_get_bytes_per_sample(cfg.inSampleFmt)
                                 * cfg.channels
                                 * nb_in_samples;
        std::printf("  总字节=%d（frame，fltp）；输入行字节=%d（%s）\n\n",
                    frame_bytes, in_frame_bytes,
                    av_get_sample_fmt_name(cfg.inSampleFmt));

        /* 读入缓冲：按 packed 交错格式读，大小按输入格式算
         * 【C++ 改动】用 new[]/delete[] 或 std::vector 都行；
         * 这里保留 malloc/free 是为了和 FFmpeg 的 C 风格一致，
         * 但用 static_cast<uint8_t*> 显式转换（C++ 不允许 void* 隐式转换）。 */
        uint8_t* pcm_buf = static_cast<uint8_t*>(std::malloc(in_frame_bytes));
        if (!pcm_buf) {
            std::fprintf(stderr, "malloc pcm_buf failed\n");
            return 1;   /* 由 lambda 内的统一收尾负责释放已分配的资源 */
        }
        /* 转换缓冲：仅手写 f32le → fltp 路径使用 */
        uint8_t* pcm_temp_buf = static_cast<uint8_t*>(std::malloc(frame_bytes));
        if (!pcm_temp_buf) {
            std::fprintf(stderr, "malloc pcm_temp_buf failed\n");
            return 1;
        }

        /*
         * 输入格式和编码器要求不一致时，决定用哪种转换方式：
         *   - f32le → fltp 且 2 声道：用手写转换（便于理解 planar 排布）
         *   - 其它组合：交给 libswresample
         */
        if (cfg.inSampleFmt != static_cast<AVSampleFormat>(frame->format) &&
            !(cfg.inSampleFmt == AV_SAMPLE_FMT_FLT &&
              frame->format == AV_SAMPLE_FMT_FLTP &&
              frame->ch_layout.nb_channels == 2)) {
            swr = create_swr(cfg.inSampleFmt, cfg.channels, cfg.sampleRate, codec_ctx);
            if (!swr) {
                std::fprintf(stderr, "create_swr failed\n");
                return 1;
            }
        }

        /*
         * ===================================================================
         * 【知识点 22】PTS —— 音频的时间戳
         * ===================================================================
         * PTS (Presentation Time Stamp) 回答一个问题：
         *   「这一帧的数据，是从整段音频的第几个采样点开始的？」
         *
         * 单位：采样点，不是秒。所以要换算成秒：
         *     秒 = pts / sample_rate
         *        = 1024 / 48000 = 0.02133 s  （第 2 帧的开始时间）
         *
         * 规则：第 n 帧的 pts = n × frame_size。第一帧必须是 0。
         *
         * 【为什么单位是采样点而不是秒？】
         *   整数运算无误差。用秒就只能存浮点，长时间播放会累积误差。
         *   播放器知道 sample_rate，自己除一下就行。
         *
         * 【AAC 的 priming 帧】
         *   编码器吐出来的第一个包 pts = -1024（负数，合法）。
         *   它不对应真实音频，是让解码端能正确重建开头 1024 个采样点的
         *   「预卷」数据（因为 MDCT 需要前后文）。解码器会把它丢掉。
         *   所以看到负 pts 不要「修」它，这是规范行为。
         * =================================================================== */
        int64_t pts = 0;
        std::printf("开始编码（每帧 %d 个采样点，即 %.2f ms）...\n\n",
                    nb_in_samples, 1000.0 * nb_in_samples / cfg.sampleRate);

        for (;;) {
            /* ---- 读一块 PCM ---- */
            const size_t read_bytes = std::fread(pcm_buf, 1, in_frame_bytes, infile);
            if (read_bytes == 0) {
                std::printf("文件读取完毕\n");
                break;
            }

            /*
             * 【知识点 23】尾帧处理 —— 最后一块往往不足 frame_size
             *
             * 文件总采样点数一般不是 1024 的整数倍，最后一块会短。
             * 有两种处理方式：
             *
             *   (a) 补零到 1024 再送（简单，但末尾会多一小段静音）
             *   (b) 按实际长度送（本程序的做法，需要改 frame->nb_samples）
             *
             * 本程序用 (b)：把读到的字节数换算成采样点数，让 frame 只描述
             * 真实存在的数据。这样编码出来的总时长和原始 PCM 严格一致。
             *
             * 换算公式（输入是 packed，所以乘声道数）：
             *     采样点数 = 字节数 / (每采样点字节数 × 声道数)
             *
             * ⚠ 注意 frame->nb_samples 改了之后，frame 的「逻辑长度」变了，
             *   但 av_frame_get_buffer 分配的物理内存没变（还是 1024 点的容量）。
             *   只要 nb_samples ≤ 容量就是安全的，这是「缩小使用、不超用」。
             *
             * 【更通用的做法】固定送 1024 个点，但用 av_audio_fifo 管理输入：
             *   把读到的数据先写进 FIFO，再每次从 FIFO 取满 1024 个点送编码器。
             *   实时采集（麦克风数据块大小不固定）时必须这么做。
             */
            const int nb_samples_read = static_cast<int>(read_bytes /
                    (static_cast<size_t>(av_get_bytes_per_sample(cfg.inSampleFmt))
                     * static_cast<size_t>(cfg.channels)));
            if (nb_samples_read <= 0)
                break;

            /*
             * 【知识点 24】av_frame_make_writable —— 为什么每帧写之前都要调
             *
             * 编码器可能持有上一帧的引用（因为它是引用计数的）。
             * 如果 frame 的 buffer 还被编码器引用着，你直接往里写就会：
             *   1. 破坏编码器正在用的数据 → 编出来的声音是错的
             *   2. 数据竞争（多线程编码时）
             *
             * av_frame_make_writable 检查引用计数：
             *   - 只有你一个人用 → 什么都不做，直接返回（零开销）
             *   - 还有别人引用   → 偷偷重新分配一块新 buffer，把旧数据拷过来，
             *                      然后 frame->data 指向新 buffer
             *
             * 所以它的返回值必须检查：为负说明重新分配失败了，
             * 这时 frame 里的数据是**不可写**的，继续写就会出问题。
             */
            if ((ret = av_frame_make_writable(frame)) < 0) {
                std::fprintf(stderr, "av_frame_make_writable failed: %s\n", errStr(ret).c_str());
                break;
            }

            /*
             * ===============================================================
             * 【知识点 25】把 PCM 数据「挂」到 frame 上
             * ===============================================================
             * 核心函数：
             *   int av_samples_fill_arrays(uint8_t **audio_data,  // 出参：frame->data
             *                              int *linesize,         // 出参：frame->linesize
             *                              const uint8_t *buf,    // 你的 PCM 内存
             *                              int nb_channels,
             *                              int nb_samples,
             *                              enum AVSampleFormat sample_fmt,
             *                              int align);
             *
             * 【最重要的一点】它**不拷贝数据**，只是把 buf 的地址
             * 按格式拆成 data[0]、data[1]... 并计算 linesize。
             *   packed(S16/FLT)  →  buf 整体挂到 data[0]，data[1]=NULL
             *   planar(FLTP/S16P)→  buf 按 nb_samples 切分，
             *                       data[0]=buf, data[1]=buf+nb_samples*每点字节, ...
             *
             * 后果：pcm_buf / pcm_temp_buf 的生命周期必须覆盖到
             *      avcodec_send_frame() **返回之后**。一旦 send 完，编码器
             *      要么已经拷走数据，要么自己做了引用，你就可以复用这块内存了。
             *      （本程序的 pcm_buf 在下一轮循环开头就被覆盖，是安全的。）
             *
             * 【C++ 提醒】av_samples_fill_arrays 的第二个参数是 int*，
             *   而 frame->linesize 就是 int[8]，直接传即可。
             *   第一个参数 uint8_t** 对应 frame->data（uint8_t*[8]）。
             *   这两个数组会「退化」成指针，所以类型是匹配的。
             * ===============================================================
             */
            if (cfg.inSampleFmt == static_cast<AVSampleFormat>(frame->format)) {
                /* 情况 A：输入格式已经等于编码器要求，零拷贝直接挂 */
                frame->nb_samples = nb_samples_read;
                ret = av_samples_fill_arrays(frame->data, frame->linesize,
                                             pcm_buf,
                                             frame->ch_layout.nb_channels,
                                             frame->nb_samples,
                                             static_cast<AVSampleFormat>(frame->format), 0);
            } else if (swr == nullptr) {
                /* 情况 B：手写 f32le → fltp（仅 2 声道） */
                std::memset(pcm_temp_buf, 0, frame_bytes);
                f32le_convert_to_fltp(reinterpret_cast<const float*>(pcm_buf),
                                      reinterpret_cast<float*>(pcm_temp_buf),
                                      nb_samples_read);
                frame->nb_samples = nb_samples_read;
                ret = av_samples_fill_arrays(frame->data, frame->linesize,
                                             pcm_temp_buf,
                                             frame->ch_layout.nb_channels,
                                             frame->nb_samples,
                                             static_cast<AVSampleFormat>(frame->format), 0);
            } else {
                /*
                 * 情况 C：libswresample
                 *   out_count    传 nb_in_samples（= frame 的物理容量 1024），
                 *                而不是 nb_samples_read —— 一是因为容量约束是
                 *                真实约束，二是因为 swr 可能吐出比输入更多的点
                 *                （内部 FIFO 里以前缓存的数据这次一起吐出来）。
                 *   返回值       实际写出的采样点数，必须用它更新 frame->nb_samples。
                 *
                 * 【C++ 类型坑】swr_convert 的第 4 个参数是 `const uint8_t**`，
                 *   而我们手上是 `uint8_t*`。不能写
                 *       const_cast<const uint8_t**>(pcm_buf)     // ✗ 编译报错
                 *   因为 const_cast 只能增删「指针本身」的限定符，
                 *   不能把 `T*` 变成 `const T**`（这是两个不同类型的指针）。
                 *   正确做法是先用一个数组把指针装起来，再把这个数组传进去 ——
                 *   `const uint8_t* inData[1] = { pcm_buf };` 就是数组退化成
                 *   `const uint8_t**`，类型天然匹配，也不需要任何 cast。
                 *   单平面（packed）输入时数组只有 1 个元素。
                 */
                const uint8_t* inData[1] = { pcm_buf };
                ret = swr_convert(swr, frame->data, nb_in_samples,
                                  inData, nb_samples_read);
                if (ret < 0) {
                    std::fprintf(stderr, "swr_convert failed: %s\n", errStr(ret).c_str());
                    break;
                }
                frame->nb_samples = ret;
                ret = 0;
            }

            if (ret < 0) {
                std::fprintf(stderr, "填充 frame 数据失败\n");
                break;
            }
            if (frame->nb_samples <= 0)
                continue;   /* swr 可能一个点都没吐出（全缓存了），跳过这一轮 */

            /*
             * 设置本帧的 pts = 本帧第一个采样点的序号，然后累加。
             * 顺序很重要：先赋值（用的是「本帧起点」），再累加（准备下一帧起点）。
             */
            frame->pts = pts;
            pts += frame->nb_samples;
            ++g_frame_index;

            if (g_frame_index <= 3)
                std::printf("送出 frame[%" PRId64 "]: nb_samples=%d pts=%" PRId64 "\n",
                            g_frame_index, frame->nb_samples, frame->pts);

            /* ---- 送进编码器，取回所有可用的包 ---- */
            if (encode(codec_ctx, frame, pkt, outfile, cfg.writeAdts,
                       &flushed, &stats) < 0) {
                std::fprintf(stderr, "encode failed\n");
                break;
            }
        }

        /*
         * ===================================================================
         * 【知识点 26】冲刷（flush）—— 不冲刷就会丢数据
         * ===================================================================
         * 循环结束时，编码器内部还压着一些采样点没吐出来：
         *   - AAC 的 MDCT 需要 look-ahead，总有 <1024 个点留在内部
         *   - 再加上 priming 需要补齐
         * 对于本程序，最后还有 209 个采样点在编码器里（481489 % 1024 = 209）。
         *
         * 冲刷方式：send(nullptr)，然后一直 receive 到 AVERROR_EOF。
         * AAC 会吐出 2 个包：一个是真正包含尾部 209 个点的，
         * 另一个只有几字节（纯 padding，补齐到完整帧）。
         *
         * 【不冲刷的后果】音频末尾被截掉约 21 ms，听感上是「最后一个音被切断」。
         * =================================================================== */
        std::printf("\n冲刷编码器（send nullptr）...\n");
        if (encode(codec_ctx, nullptr, pkt, outfile, cfg.writeAdts,
                   &flushed, &stats) < 0)
            std::fprintf(stderr, "冲刷失败\n");

        std::printf("\n===== 编码完成 =====\n");
        std::printf("输入采样点 : %" PRId64 " 个（%.3f 秒 @%d Hz）\n",
                    pts, static_cast<double>(pts) / cfg.sampleRate, cfg.sampleRate);
        std::printf("输出包数   : %" PRId64 " 包\n", stats.packets);
        std::printf("输出大小   : %" PRId64 " 字节 (%.2f KB)\n",
                    stats.bytes, stats.bytes / 1024.0);
        std::printf("平均码率   : %.0f kbps\n",
                    pts > 0 ? stats.bytes * 8.0
                              / (static_cast<double>(pts) / cfg.sampleRate) / 1024.0
                            : 0.0);
        std::printf("每包均值   : %.1f 字节（AAC 一帧 1024 采样点）\n",
                    stats.packets > 0 ? static_cast<double>(stats.bytes) / stats.packets : 0.0);

        /*
         * ===================================================================
         * 【知识点 27】释放顺序与「二级指针」惯例
         * ===================================================================
         * libav* 的释放函数统一传**二级指针**，例如 av_frame_free(&frame)。
         * 原因是它需要在 free 之后把调用方的指针置成 nullptr，
         * 防止你之后误用野指针。C/C++ 里改调用方的指针只能靠传地址。
         *
         * 释放顺序的原则：先释放「引用了别人」的对象，
         *   顺序本身不严格（都是引用计数），但推荐：
         *     swr（被 frame 数据源用过）→ frame → packet → codec_ctx
         *
         * 【谁负责释放 ch_layout 里的内存？】
         *   - codec_ctx->ch_layout : avcodec_free_context() 负责 uninit
         *   - frame->ch_layout     : av_frame_free() 负责 uninit
         *   - 自己声明的栈上布局    : 必须自己 av_channel_layout_uninit()
         *                            （本程序在 create_swr 里已经这么做了）
         *   所以这里**不需要**手动 uninit。
         *
         * 【C++ 里为什么这里仍然写一遍清理，而不是靠 RAII？】
         *   因为 FFmpeg 的对象是 C 结构体，没有析构函数。
         *   想真正 RAII，要自己包一层（unique_ptr + 自定义 deleter），
         *   例如：
         *       struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
         *       std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
         *   pusher 里就是这么做的（各组件在析构函数里统一 close）。
         *   教学工程为了流程直白，用的是 lambda + 显式释放。
         * ===================================================================
         */
        /* ---- 统一收尾（lambda 内部，等价于原 C 版本的 goto cleanup） ---- */
        if (infile)       std::fclose(infile);
        if (outfile)      std::fclose(outfile);
        if (pcm_buf)      std::free(pcm_buf);
        if (pcm_temp_buf) std::free(pcm_temp_buf);
        swr_free(&swr);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&codec_ctx);
        return 0;
    }();   /* lambda 立即执行 */

    if (exitCode != 0)
        return exitCode;

    std::printf("\n产物: %s\n", cfg.outFile.c_str());
    std::printf("验证方法:\n");
    std::printf("  ffprobe -show_format -show_streams %s\n", cfg.outFile.c_str());
    std::printf("  ffplay %s\n", cfg.outFile.c_str());
    if (!cfg.writeAdts)
        std::printf("  注意：noadts 输出是裸 raw_data_block，ffmpeg 解不开"
                    "（要靠封装层提供 AudioSpecificConfig），这是预期行为\n");
    return 0;
}

/*
 * ===========================================================================
 * 附：进一步学习的路线
 * ===========================================================================
 * 1. 【用 libavformat 做封装】本程序手写 ADTS 是为了讲清编码本身。
 *    真实项目应该让 libavformat 负责容器（详见 08-03-muxing_flv）：
 *        avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, "out.m4a");
 *        AVStream *st = avformat_new_stream(fmt_ctx, nullptr);
 *        avcodec_parameters_from_context(st->codecpar, codec_ctx);
 *        avio_open(&fmt_ctx->pb, "out.m4a", AVIO_FLAG_WRITE);
 *        avformat_write_header(fmt_ctx, nullptr);
 *        av_interleaved_write_frame(fmt_ctx, pkt);   ← 替代 fwrite
 *        av_write_trailer(fmt_ctx);
 *    它会自动生成 AudioSpecificConfig，也就不需要 ADTS 了。
 *
 * 2. 【实时采集】输入不再是文件而是设备（麦克风），
 *    每次拿到的采样点数不固定，必须用 av_audio_fifo 做缓冲：
 *        AVAudioFifo *fifo = av_audio_fifo_alloc(sample_fmt, nb_channels, 1);
 *        av_audio_fifo_write(fifo, (void**)frame->data, nb_samples);
 *        while (av_audio_fifo_size(fifo) >= frame_size) {
 *            av_audio_fifo_read(fifo, (void**)out_frame->data, frame_size);
 *            encode(...);
 *        }
 *
 * 3. 【变采样率】若源是 44100 而编码器要求 48000，
 *    直接在 create_swr 里把 in_rate 传 44100、out_rate 传 48000 即可，
 *    swr 会自动做带抗混叠滤波的重采样。注意此时
 *    swr_convert 的输入输出采样点数不成比例，必须用返回值。
 *
 * 4. 【看官方示例】
 *    doc/examples/encode_audio.c    —— 生成正弦波并编码（本程序的原型）
 *    doc/examples/decode_audio.c    —— 对应的解码侧
 *    doc/examples/resampling_audio.c—— libswresample 的完整用法
 *    doc/examples/transcode_aac.c   —— 端到端的转码例子（最接近实战）
 * ===========================================================================
 */
