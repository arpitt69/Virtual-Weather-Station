#pragma once
/*
 * SampleQueue.h - bounded blocking queue between the reader thread
 * (producer) and the fusion thread (consumer).
 *
 * The queue drops the oldest entry rather than blocking the producer: the
 * reader must get back into poll() promptly or the kernel FIFO overflows
 * instead, and losing the oldest reading is the cheaper of the two.
 */
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace vws {

template <typename T>
class SampleQueue {
public:
    explicit SampleQueue(std::size_t maxSize = 4096) : maxSize_(maxSize) {}

    void push(const T& v) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (q_.size() >= maxSize_) {
                q_.pop_front();
                ++dropped_;
            }
            q_.push_back(v);
        }
        cv_.notify_one();
    }

    template <typename It>
    void pushAll(It first, It last) {
        {
            std::lock_guard<std::mutex> lk(m_);
            for (It it = first; it != last; ++it) {
                if (q_.size() >= maxSize_) { q_.pop_front(); ++dropped_; }
                q_.push_back(*it);
            }
        }
        cv_.notify_one();
    }

    /// Blocks until an item is available, the timeout expires, or close().
    std::optional<T> popFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, timeout, [this] { return !q_.empty() || closed_; }))
            return std::nullopt;
        if (q_.empty()) return std::nullopt;
        T v = q_.front();
        q_.pop_front();
        return v;
    }

    /// Drain everything currently queued in one lock acquisition.
    std::vector<T> drainFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, timeout, [this] { return !q_.empty() || closed_; });
        std::vector<T> out(q_.begin(), q_.end());
        q_.clear();
        return out;
    }

    void close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        cv_.notify_all();
    }

    std::size_t size() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    std::uint64_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> q_;
    std::size_t maxSize_;
    std::uint64_t dropped_ = 0;
    bool closed_ = false;
};

}  // namespace vws
