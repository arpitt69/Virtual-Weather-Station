#include "SensorReader.h"

#include <chrono>
#include <vector>

namespace vws {

using namespace std::chrono_literals;

SensorReader::SensorReader(VwsDevice& dev, SampleQueue<Reading>& out)
    : dev_(dev), out_(out) {}

SensorReader::~SensorReader() { stop(); }

void SensorReader::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread(&SensorReader::run, this);
}

void SensorReader::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

void SensorReader::run() {
    std::vector<Reading> batch;
    batch.reserve(256);

    while (!stop_.load()) {
        try {
            // A bounded poll() timeout is what makes shutdown prompt: without
            // it the thread would sit in the kernel until the next sample.
            const std::vector<vws_sample> raw = dev_.waitAndRead(200ms);
            if (raw.empty()) continue;

            ++reads_;
            batch.clear();
            for (const vws_sample& s : raw) {
                Reading r;
                if (!r.fromSample(s)) continue;   // unknown sensor type
                if (s.flags & VWS_F_RESYNC) ++resyncs_;
                batch.push_back(r);
            }
            samples_ += batch.size();
            out_.pushAll(batch.begin(), batch.end());
        } catch (const std::exception& e) {
            error_ = e.what();
            failed_.store(true);
            break;
        }
    }
    out_.close();
}

}  // namespace vws
