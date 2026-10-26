// =============================================================================
//  dropqueue.h —— 有界队列（推流链路里的"背压"就靠它）
//
//  【知识点 1】为什么推流链路必须有队列，而且必须"有界"
//  ---------------------------------------------------------------------------
//  采集、编码、写出是三件速度不同的事：
//      摄像头 30fps 到点就出帧，你处理不过来它也不会等你
//      编码可能偶尔慢一拍（场景复杂时）
//      网络写出最不可控（对端一卡就是几百毫秒）
//
//  如果中间没有队列：三者必须严格同步，一处慢就全链卡住，采集设备会丢帧。
//  如果队列**无界**：网络卡的时候包越堆越多，延迟越来越大，
//  半小时后观众看到的是"半小时前的画面"——直播里这是致命伤。
//
//  所以：队列要有界，满了之后必须有个明确的策略。本文件实现两种：
//
//      dropOldestWhenFull = true   → 丢最旧的，收下新的（实时链路）
//      dropOldestWhenFull = false  → 阻塞等待，直到消费者腾出位置（落盘）
//
//  这个开关不是随便加的：视频帧队列必须丢，包队列落盘时必须阻塞（文件要完整）、
//  推流时才改成丢（不能堆延迟）。见 pusher.cpp 里构造队列的两处注释。
// ---------------------------------------------------------------------------
//
//  【知识点 2】close() 的语义 —— 决定"优雅退出"能不能写对
//  ---------------------------------------------------------------------------
//      close() 之后：push() 直接拒绝（并释放元素，不能泄漏）
//      close() 之后：pop() 仍然能把队列里剩下的元素取完，取空后才返回 false
//
//  只有这样，"先停生产 → 把存货消费干净 → 再写 trailer"才能实现。
//  如果把 close() 写成"立刻丢弃所有元素"，最后一段画面和音频就没了，
//  mp4 的结尾也会不完整。
// =============================================================================
#ifndef PUSH_DROPQUEUE_H
#define PUSH_DROPQUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
}

/*
 * 队列持有元素的所有权。被丢弃 / 被 close 拒绝 / 队列析构时，都用 Deleter 释放，
 * 这样上层就不用到处写 "别忘了 free"。
 */
struct AvFrameDeleter {
    void operator()(AVFrame* f) const { if (f) av_frame_free(&f); }
};

struct AvPacketDeleter {
    void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};

template <typename T, typename Deleter>
class DropQueue
{
public:
    DropQueue(size_t capacity, Deleter deleter, bool dropOldestWhenFull)
        : m_capacity(capacity ? capacity : 1)
        , m_deleter(std::move(deleter))
        , m_dropOldest(dropOldestWhenFull)
    {
    }

    ~DropQueue()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_items.empty()) {
            m_deleter(m_items.front());
            m_items.pop_front();
        }
    }

    DropQueue(const DropQueue&) = delete;
    DropQueue& operator=(const DropQueue&) = delete;

    /*
     * 放一个元素进去（所有权交给队列）。
     *
     * 满的时候分两种：
     *   丢最旧 → 队列里留的永远是"最近"的数据，这正是实时链路要的
     *   阻塞   → 等消费者腾位置；等待期间如果队列被 close 了，就放弃并释放元素
     */
    void push(T item)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_closed) {                       // 已经收工：拒绝，并且一定要释放，否则泄漏
            lock.unlock();
            m_deleter(item);
            return;
        }

        if (m_items.size() >= m_capacity) {
            if (m_dropOldest) {
                /* 丢掉最旧的，收下新的。
                 * 想统计"丢了多少"，在这里加一行计数/打印即可 ——
                 * 本工程不输出日志，所以没有内置计数器（见文档 §8.2）。 */
                m_deleter(m_items.front());
                m_items.pop_front();
            } else {
                // wait 的谓词必须同时检查 m_closed：否则 close 之后这里会永远等下去
                m_cv.wait(lock, [this] { return m_closed || m_items.size() < m_capacity; });
                if (m_closed) {
                    lock.unlock();
                    m_deleter(item);
                    return;
                }
            }
        }

        m_items.push_back(item);
        lock.unlock();
        m_cv.notify_one();
    }

    /*
     * 取一个元素出来（所有权交回调用方）。
     *
     * 带 timeout 的原因：消费者要周期性返回上层，检查"是不是该退出了"。
     * 如果死等，stop() 时就会卡在 pop 里出不来。
     * 返回 false 表示"超时"或"队列已关闭且取空"，两种情况都该让调用方回头看退出标志。
     */
    bool pop(T& out, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return !m_items.empty() || m_closed; })) {
            return false;                 // 超时，队列还是空的
        }
        if (m_items.empty()) return false; // 已 close 且取空

        out = m_items.front();
        m_items.pop_front();
        lock.unlock();
        m_cv.notify_all();                // 唤醒可能在等位置的阻塞 push
        return true;
    }

    /* 停止生产：之后 push 被拒，但 pop 仍能把存货取完 */
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        m_cv.notify_all();
    }

    /* 生产者是否已收工。消费者用它判断"取空就可以退出"——
     * 不能拿外部的 running 标志判断：生产者的尾巴数据可能还没写完 */
    bool closed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size();
    }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::deque<T>           m_items;
    size_t                  m_capacity;
    Deleter                 m_deleter;
    bool                    m_dropOldest;
    bool                    m_closed = false;
};

#endif // PUSH_DROPQUEUE_H
