#pragma once
/*
 * SensorReader.h - the producer thread.
 *
 * Its only job is to get back into poll() quickly: anything slower than the
 * sampling clock and the kernel FIFO starts dropping samples. Conversion to
 * engineering units happens here (it is cheap), everything else is the fusion
 * thread's problem.
 */
#include <atomic>
#include <cstdint>
#include <thread>

#include "SampleQueue.h"
#include "Units.h"
#include "VwsDevice.h"

namespace vws {

class SensorReader {
public:
    SensorReader(VwsDevice& dev, SampleQueue<Reading>& out);
    ~SensorReader();

    SensorReader(const SensorReader&) = delete;
    SensorReader& operator=(const SensorReader&) = delete;

    void start();
    void stop();   ///< idempotent; safe to call from a signal-driven shutdown

    std::uint64_t samplesRead() const noexcept { return samples_.load(); }
    std::uint64_t resyncs() const noexcept { return resyncs_.load(); }
    std::uint64_t readCalls() const noexcept { return reads_.load(); }
    bool failed() const noexcept { return failed_.load(); }
    const std::string& error() const noexcept { return error_; }

private:
    void run();

    VwsDevice& dev_;
    SampleQueue<Reading>& out_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> samples_{0};
    std::atomic<std::uint64_t> resyncs_{0};
    std::atomic<std::uint64_t> reads_{0};
    std::string error_;
};

}  // namespace vws
