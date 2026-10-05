#pragma once
/*
 * RingBuffer.h - fixed-capacity rolling window, the user-space counterpart to
 * struct vws_fifo in the driver. Overwrites the oldest element when full,
 * which is exactly what a rolling statistics window wants.
 */
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace vws {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : buf_(capacity) {
        if (capacity == 0) throw std::invalid_argument("RingBuffer capacity must be > 0");
    }

    void push(const T& v) {
        buf_[head_] = v;
        head_ = (head_ + 1) % buf_.size();
        if (size_ < buf_.size()) ++size_;
    }

    /// Index 0 is the oldest retained element.
    const T& operator[](std::size_t i) const {
        return buf_[(head_ + buf_.size() - size_ + i) % buf_.size()];
    }

    const T& back() const { return (*this)[size_ - 1]; }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return buf_.size(); }
    bool empty() const noexcept { return size_ == 0; }
    bool full() const noexcept { return size_ == buf_.size(); }
    void clear() noexcept { head_ = 0; size_ = 0; }

    /// Copy out in chronological order, for median/variance work.
    std::vector<T> snapshot() const {
        std::vector<T> out;
        out.reserve(size_);
        for (std::size_t i = 0; i < size_; ++i) out.push_back((*this)[i]);
        return out;
    }

private:
    std::vector<T> buf_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

}  // namespace vws
