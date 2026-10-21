#include "pusher.h"

#include <chrono>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>   // av_packet_clone 的配套：时间戳工具
}

namespace {

/* 输出是"网络推流"还是"本地文件"？
 * 这决定包队列满了怎么办：网络流必须丢包（不能堆延迟），文件必须阻塞等待（不能缺数据）。 */
bool isNetworkOutput(const OutputConfig& out)
{
    if (out.format == "flv" || out.format == "rtmp") return true;
    return out.url.rfind("rtmp", 0) == 0
        || out.url.rfind("rtsp", 0) == 0
        || out.url.rfind("srt",  0) == 0;
}

} // namespace

// ============================================================================================
// 生命周期
// ============================================================================================

Pusher::Pusher(PusherConfig cfg)
    : m_cfg(std::move(cfg))
{
    /* 【知识点 24】两个队列的策略为什么不一样
     * ---------------------------------------------------------------------------
     * 帧队列：采集是实时源 → 必须丢最旧，绝不能让采集线程等。
     *         容量故意小（默认 8）：攒太多只会让"采集 → 编码"的延迟变大。
     * 包队列：落盘时阻塞（等一等无所谓，文件必须完整）；
     *         推流时丢包（网络卡住时宁可丢，也不能让延迟越堆越大）。
     * --------------------------------------------------------------------------- */
    m_frameQueue = std::make_unique<PusherFrameQueue>(
        static_cast<size_t>(m_cfg.frameQueueCapacity), AvFrameDeleter{},
        /*dropOldestWhenFull=*/true);

    m_packetQueue = std::make_unique<PusherPacketQueue>(
        static_cast<size_t>(m_cfg.packetQueueCapacity), AvPacketDeleter{},
        /*dropOldestWhenFull=*/isNetworkOutput(m_cfg.output));

    m_muxer = createMuxer();
}

Pusher::~Pusher()
{
    stop();
}

// ============================================================================================
// start —— 打开所有资源，起四条线程
// ============================================================================================

bool Pusher::start()
{
    if (m_running.load()) return true;

    m_abortIo.store(false);
    m_running.store(true);
    m_startFailure = StartFailure::None;

    /* ---------------------------------------------------------------- 视频源
     * 【知识点 25】为什么"打开采集源"要排在"配编码器"前面
     * ---------------------------------------------------------------------------
     * 采集参数是**协商**出来的：我们提议 1280x720@30，设备可能给 640x480@25。
     * 编码器的宽高/帧率必须和喂进来的帧完全一致，否则 send_frame 直接报错或花屏。
     * 所以顺序必须是：先开源（拿到实际值）→ 再回填 encoder 配置 → 再开编码器。
     * --------------------------------------------------------------------------- */
    if (m_cfg.withVideo) {
        m_video = createVideoSource(m_cfg.video);
        if (!m_video || !m_video->open(m_cfg.video)) {
            m_startFailure = StartFailure::VideoSource;
            m_running.store(false);
            return false;
        }
        m_cfg.encoder.width  = m_video->width();
        m_cfg.encoder.height = m_video->height();
        m_cfg.encoder.fps    = m_video->fps();
    }

    if (m_cfg.withAudio) {
        m_audio = createAudioSource(m_cfg.audio);
        if (!m_audio || !m_audio->open(m_cfg.audio)) {
            // 音频是可选能力：失败就退化成"只推视频"，比整体失败更好排查
            m_audio.reset();
            m_cfg.withAudio = false;
        } else {
            m_cfg.encoder.audioInSampleRate = m_audio->sampleRate();
            m_cfg.encoder.audioInChannels   = m_audio->channels();
            m_cfg.encoder.audioInSampleFmt  = m_audio->sampleFormat();
        }
    }

    /* ---------------------------------------------------------------- 视频编码器 */
    if (m_cfg.withVideo) {
        m_videoEnc = createVideoEncoder(m_cfg.encoder);
        if (!m_videoEnc || !m_videoEnc->open(m_cfg.encoder)) {
            m_startFailure = StartFailure::VideoEncoder;
            m_running.store(false);
            return false;
        }

        /* 【知识点 26】sink 为什么必须 clone
         * ---------------------------------------------------------------------------
         * 回调给的 AVPacket 指针只在回调期间有效（编码器随后会 unref 复用它）。
         * 我们要把它放进队列留给别的线程用，所以必须 av_packet_clone：
         * 它只给码流数据加一次引用（不拷码流），最后由队列负责 av_packet_free。
         *
         * 顺便在这里写上 stream_index：编码器不该知道自己在第几路流（见知识点 12.1），
         * 这个下标是编排层在 openOutput() 里按"实际建了几路"算出来的。
         * 它可能晚于这里赋值（sink 是 lambda，运行时才读），所以捕获 this 就够了。
         * --------------------------------------------------------------------------- */
        m_videoEnc->setSink([this](AVPacket* pkt) {
            AVPacket* copy = av_packet_clone(pkt);
            if (!copy) return;
            copy->stream_index = m_videoStreamIndex;
            m_packetQueue->push(copy);
        });
    }

    /* ---------------------------------------------------------------- 音频编码器 */
    if (m_cfg.withAudio) {
        m_audioEnc = createAudioEncoder(m_cfg.encoder);
        if (m_audioEnc && m_audioEnc->open(m_cfg.encoder)) {
            // 音频也是同一个包队列，index 同样由编排层填
            m_audioEnc->setSink([this](AVPacket* pkt) {
                AVPacket* copy = av_packet_clone(pkt);
                if (!copy) return;
                copy->stream_index = m_audioStreamIndex;
                m_packetQueue->push(copy);
            });
        } else {
            m_audioEnc.reset();
            m_cfg.withAudio = false;
        }
    }

    /* ---------------------------------------------------------------- 输出
     * 这里先打开一次；失败就直接返回（重连只在"运行中掉线"时才有意义）。 */
    if (!openOutput()) {
        m_startFailure = StartFailure::Output;
        m_running.store(false);
        if (m_video) m_video->close();
        if (m_audio) m_audio->close();
        return false;
    }

    /* ---------------------------------------------------------------- 起线程
     * 【知识点 27】为什么编码线程和写出线程要分开
     * ---------------------------------------------------------------------------
     * x264 编码是 CPU 密集的，可能偶发慢一拍；写出可能卡在网络。
     * 如果放在同一条线程：网络卡 200ms → 编码停 200ms → 帧队列堆满 → 丢帧。
     * 分开之后，网络慢只会让包队列涨（推流模式下丢包），编码照常跑，
     * 采集也照常跑 —— 这就是"解耦"的实际价值。
     * --------------------------------------------------------------------------- */
    if (m_cfg.withVideo) m_videoThread = std::thread(&Pusher::videoCaptureLoop, this);
    if (m_cfg.withAudio) m_audioThread = std::thread(&Pusher::audioCaptureLoop, this);
    if (m_cfg.withVideo) m_encodeThread = std::thread(&Pusher::encodeLoop, this);
    m_pushThread = std::thread(&Pusher::pushLoop, this);

    return true;
}

// ============================================================================================
// stop —— 有序停止
// ============================================================================================

/*
 * 【知识点 28】停止顺序为什么必须是这个顺序
 * ---------------------------------------------------------------------------
 * 每一步都在解决"上一步还在产生数据"的问题：
 *
 *   1) 叫停采集 + requestStop()
 *        让卡在 av_read_frame 里的采集线程立刻返回（中断回调）。
 *        不做这步，采集线程可能卡几十秒，join 会一直等。
 *
 *   2) join 视频采集线程
 *        它退出前会 close() 帧队列 —— 告诉编码线程"不会再有新帧了"。
 *
 *   3) join 音频采集线程
 *        它退出前会 flush 音频编码器，把最后几包塞进包队列。
 *
 *   4) join 编码线程
 *        它把帧队列里剩下的帧编完、flush 视频编码器，然后退出。
 *        —— 到这里，包队列的两个生产者都结束了。
 *
 *   5) close() 包队列
 *        现在关才安全：推流线程会把存货写完才退出（close 的语义见 dropqueue.h）。
 *
 *   6) join 推流线程
 *        它写完剩余包、调用 writeTrailer()（mp4 的 moov 在这一步）。
 *        如果它还卡在网络写上，就靠 m_abortIo 打断。
 *
 * 顺序稍微换一下就会出问题，比如先关包队列再 flush 编码器 →
 * 编码器最后吐出的包会被队列拒绝，尾部就丢了。
 * ---------------------------------------------------------------------------
 */
void Pusher::stop()
{
    // 幂等：duration 到点和析构都会调一次
    if (!m_running.exchange(false)) {
        // 即使没在跑，也要保证线程已经 join 过（防止 start 半途失败的情况）
        if (m_videoThread.joinable())  m_videoThread.join();
        if (m_audioThread.joinable())  m_audioThread.join();
        if (m_encodeThread.joinable()) m_encodeThread.join();
        if (m_pushThread.joinable())   m_pushThread.join();
        return;
    }

    // 1) 叫停采集：让阻塞中的 av_read_frame / avformat_open_input 返回
    if (m_video) m_video->requestStop();
    if (m_audio) m_audio->requestStop();

    // 2) 等视频采集线程（它退出时会关帧队列）
    if (m_videoThread.joinable()) m_videoThread.join();

    // 3) 等音频采集线程（它退出前会 flush 音频编码器）
    if (m_audioThread.joinable()) m_audioThread.join();

    // 4) 等编码线程把帧队列吃完并 flush 视频编码器
    if (m_encodeThread.joinable()) m_encodeThread.join();

    /* 5) 此时包队列已无生产者，可以关了。
     *    网络输出先把 I/O 叫停标志立起来，避免推流线程卡在一个已经死掉的连接上。 */
    if (isNetworkOutput(m_cfg.output)) {
        m_abortIo.store(true);
    }
    m_packetQueue->close();

    // 6) 等推流线程写完存货 + trailer
    if (m_pushThread.joinable()) m_pushThread.join();

    // 收尾：释放采集/编码资源（muxer 在推流线程里已经 close）
    if (m_video) m_video->close();
    if (m_audio) m_audio->close();
    m_videoEnc.reset();
    m_audioEnc.reset();
}

// ============================================================================================
// 输出：首次打开 + 重连
// ============================================================================================

bool Pusher::openOutput()
{
    m_muxer->close();

    if (!m_muxer->open(m_cfg.output, &m_abortIo)) return false;

    /* 【知识点 29】建流的顺序决定了 stream_index
     * ---------------------------------------------------------------------------
     * 先加视频再加音频 → 视频下标 0、音频下标 1；只有音频时它就是 0。
     * 所以下标必须**按实际建了几路算**，不能写死。
     * 算好之后由上面的 sink 写进包里（见知识点 12.1）。
     * --------------------------------------------------------------------------- */
    int nextIndex = 0;
    m_videoStreamIndex = -1;
    m_audioStreamIndex = -1;

    if (m_videoEnc) {
        if (!m_muxer->addStream(m_videoEnc->codecpar(), m_videoEnc->timeBase())) return false;
        m_videoStreamIndex = nextIndex++;
    }
    if (m_audioEnc) {
        if (!m_muxer->addStream(m_audioEnc->codecpar(), m_audioEnc->timeBase())) return false;
        m_audioStreamIndex = nextIndex++;
    }

    if (!m_muxer->writeHeader()) return false;

    m_outputReady = true;
    return true;
}

bool Pusher::reconnect()
{
    if (m_outputGivenUp) return false;

    /* 【知识点 30.1】退避重连
     * ---------------------------------------------------------------------------
     * 断网时立刻疯狂重试只会浪费 CPU，也容易被服务端判成恶意连接。
     * 所以按指数退避等：delay * 2^(n-1)，上限 15 秒。
     *   n=1 → 1s   n=2 → 2s   n=3 → 4s   n=4 → 8s   n>=5 → 15s
     * --------------------------------------------------------------------------- */
    int shift = m_reconnects > 4 ? 4 : m_reconnects;
    int delayMs = m_cfg.reconnectDelayMs << shift;
    if (delayMs <= 0 || delayMs > 15000) delayMs = 15000;

    /* 分片睡：这样 stop() 能很快打断等待，而不是傻等 15 秒 */
    for (int waited = 0; waited < delayMs && m_running.load(); waited += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!m_running.load()) return false;

    ++m_reconnects;
    if (m_cfg.reconnectMaxAttempts > 0 && m_reconnects > m_cfg.reconnectMaxAttempts) {
        m_outputGivenUp = true;      // 次数用尽：之后只丢包不重试
        return false;
    }

    if (!openOutput()) return false;

    /* 【知识点 30】重连之后为什么要"等一个关键帧"
     * ---------------------------------------------------------------------------
     * 断线时我们丢掉的可能是 P 帧，接上之后如果直接写 P 帧，
     * 解码器没有参考帧可用，画面会花掉甚至解不出来。
     * 所以重连后要等下一个**关键帧（I 帧）**才开始写 ——
     * 关键帧自带完整画面，从它开始解码才安全。
     *
     * 这也解释了为什么 gop_size 不能太大：gop 越长，重连后要等越久才恢复画面。
     * --------------------------------------------------------------------------- */
    if (m_videoEnc) m_waitingKeyframe.store(true);
    return true;
}

// ============================================================================================
// 四条线程
// ============================================================================================

/* 视频采集：读一帧 → 塞进帧队列。队列满了由队列决定丢最旧，这里绝不阻塞。 */
void Pusher::videoCaptureLoop()
{
    while (m_running.load()) {
        AVFrame* frame = m_video->readFrame();
        if (!frame) break;                  // 设备结束 / 被 requestStop / 出错

        m_frameQueue->push(frame);          // 所有权交给队列
    }

    /* 告诉编码线程"不会再有新帧了"。
     * 注意不能靠 m_running 判断：采集结束（比如设备拔了）时 running 可能还是 true，
     * 编码线程必须靠这个 close 才知道该收尾。 */
    m_frameQueue->close();
}

/* 音频采集：读一帧 → 就地编码。不走帧队列，因为音频不能丢（见 pusher.h 的说明） */
void Pusher::audioCaptureLoop()
{
    while (m_running.load()) {
        AVFrame* frame = m_audio->readFrame();
        if (!frame) break;

        if (m_audioEnc) {
            /* 这里直接调 encode：它内部会重采样 + 凑够 1024 个采样点再送编码器，
             * 产出的包通过 sink 进包队列。 */
            m_audioEnc->encode(frame);
        }
        av_frame_free(&frame);
    }

    // 把编码器内部压着的采样点吐出来（否则最后几十毫秒音频会丢）
    if (m_audioEnc) m_audioEnc->flush();
}

/* 视频编码：帧队列 → 编码器 → 包队列 */
void Pusher::encodeLoop()
{
    for (;;) {
        AVFrame* frame = nullptr;

        /* pop 带超时：这样才能周期性回到这里检查"生产者是不是已经收工"。
         * 判据是 closed() && size()==0，不能只看 closed()：
         * close 之后队列里可能还有存货，必须消费干净（见 dropqueue.h）。 */
        if (!m_frameQueue->pop(frame, 50)) {
            if (m_frameQueue->closed() && m_frameQueue->size() == 0) break;
            continue;
        }

        if (m_videoEnc) m_videoEnc->encode(frame);
        av_frame_free(&frame);
    }

    // 冲刷：把编码器内部压着的若干帧全吐出来，否则视频尾部会丢
    if (m_videoEnc) m_videoEnc->flush();
}

/* 写出：包队列 → muxer（必要时重连） */
void Pusher::pushLoop()
{
    for (;;) {
        AVPacket* pkt = nullptr;

        if (!m_packetQueue->pop(pkt, 50)) {
            if (m_packetQueue->closed() && m_packetQueue->size() == 0) break;
            continue;
        }

        // 输出没准备好（首次失败 / 掉线）就先尝试恢复
        if (!m_outputReady) {
            if (!m_outputGivenUp && m_running.load()) {
                reconnect();
            }
            if (!m_outputReady) {
                /* 还是没恢复：把这个包丢掉。
                 * 不能在这里阻塞等，否则包队列会被塞满，反过来卡住编码线程。 */
                av_packet_free(&pkt);
                continue;
            }
        }

        /* 重连之后要等关键帧（见【知识点 30】）。
         * 只对视频判断：音频没有"关键帧"的概念。 */
        if (m_waitingKeyframe.load() && (pkt->flags & AV_PKT_FLAG_KEY) == 0) {
            av_packet_free(&pkt);
            continue;
        }
        if (m_waitingKeyframe.load()) m_waitingKeyframe.store(false);

        if (!m_muxer->writePacket(pkt)) {
            // 写失败一般是连接坏了：标记需要重连，把这个包也已经由 muxer 释放
            m_outputReady = false;
            if (!m_running.load()) break;    // 是"停止"导致的失败，不是掉线
        }
    }

    /* 【知识点 31】写 trailer —— 漏了 mp4 就是砖头
     * ---------------------------------------------------------------------------
     * av_write_trailer 会 flush muxer 缓冲，并写 mp4 的 moov（每帧位置/大小的索引表）。
     * 不写的话文件基本无法播放、无法 seek。
     * muxer 内部会先清掉"叫停标志"再写，保证这最后一步不会被打断。
     * --------------------------------------------------------------------------- */
    if (m_outputReady) {
        m_muxer->writeTrailer();
    }
    m_muxer->close();
    m_outputReady = false;
}
