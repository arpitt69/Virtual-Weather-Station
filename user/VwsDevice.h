#pragma once
/*
 * VwsDevice.h - RAII handle for /dev/vws.
 *
 * Owns the file descriptor, wraps the ioctl ABI in typed calls, and exposes
 * poll()/read() as a single waitAndRead(). Non-copyable, movable; the
 * destructor is the only place the fd is closed.
 */
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "../include/vws_ioctl.h"
#include "Units.h"

namespace vws {

struct SensorDescriptor {
    std::uint8_t id = 0;
    SensorType   type = SensorType::Temperature;
    std::string  name;
    bool         enabled = true;
    std::uint8_t faultMode = VWS_FAULT_NONE;
    std::int32_t faultParam = 0;
    double       rangeMin = 0.0;
    double       rangeMax = 0.0;
    double       noiseAmp = 0.0;   // engineering units
};

class VwsDevice {
public:
    /// @throws std::system_error if the device cannot be opened.
    explicit VwsDevice(const std::string& path = "/dev/vws", bool writable = true);
    ~VwsDevice();

    VwsDevice(const VwsDevice&) = delete;
    VwsDevice& operator=(const VwsDevice&) = delete;
    VwsDevice(VwsDevice&& other) noexcept;
    VwsDevice& operator=(VwsDevice&& other) noexcept;

    int fd() const noexcept { return fd_; }
    const std::string& path() const noexcept { return path_; }

    vws_info info() const;
    std::vector<SensorDescriptor> sensors() const;
    SensorDescriptor sensor(std::uint8_t id) const;
    vws_stats stats() const;

    void setSampleRate(std::uint32_t hz);
    void injectFault(std::uint8_t sensorId, std::uint8_t mode, std::int32_t param);
    void clearFault(std::uint8_t sensorId);
    void setEnabled(std::uint8_t sensorId, bool on);
    void flush();

    /**
     * Wait for data with poll(), then drain one read() worth of samples.
     * Returns an empty vector when the timeout expires with nothing ready,
     * which is how the caller gets a chance to notice a shutdown request.
     */
    std::vector<vws_sample> waitAndRead(std::chrono::milliseconds timeout,
                                        std::size_t maxSamples = 256);

private:
    void ioctlOrThrow(unsigned long req, void* arg, const char* what) const;

    int fd_ = -1;
    std::string path_;
};

}  // namespace vws
