#include "VwsDevice.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace vws {

VwsDevice::VwsDevice(const std::string& path, bool writable) : path_(path) {
    fd_ = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd_ < 0)
        throw std::system_error(errno, std::generic_category(), "open " + path);

    // Fail fast on a module/binary mismatch rather than mis-parsing samples.
    const vws_info i = info();
    if (i.abi_version != VWS_ABI_VERSION) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("vws ABI mismatch: driver reports v" +
                                 std::to_string(i.abi_version) + ", this build expects v" +
                                 std::to_string(VWS_ABI_VERSION));
    }
}

VwsDevice::~VwsDevice() {
    if (fd_ >= 0) ::close(fd_);
}

VwsDevice::VwsDevice(VwsDevice&& other) noexcept
    : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

VwsDevice& VwsDevice::operator=(VwsDevice&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

void VwsDevice::ioctlOrThrow(unsigned long req, void* arg, const char* what) const {
    if (::ioctl(fd_, req, arg) != 0)
        throw std::system_error(errno, std::generic_category(), what);
}

vws_info VwsDevice::info() const {
    vws_info i{};
    ioctlOrThrow(VWS_IOC_GET_INFO, &i, "VWS_IOC_GET_INFO");
    return i;
}

vws_stats VwsDevice::stats() const {
    vws_stats s{};
    ioctlOrThrow(VWS_IOC_GET_STATS, &s, "VWS_IOC_GET_STATS");
    return s;
}

SensorDescriptor VwsDevice::sensor(std::uint8_t id) const {
    vws_sensor_info si{};
    si.id = id;
    ioctlOrThrow(VWS_IOC_GET_SENSOR, &si, "VWS_IOC_GET_SENSOR");

    SensorDescriptor d;
    d.id         = si.id;
    d.type       = static_cast<SensorType>(si.type);
    d.name       = std::string(si.name, ::strnlen(si.name, sizeof(si.name)));
    d.enabled    = si.enabled != 0;
    d.faultMode  = si.fault_mode;
    d.faultParam = si.fault_param;
    d.rangeMin   = toEngineering(d.type, si.range_min);
    d.rangeMax   = toEngineering(d.type, si.range_max);
    d.noiseAmp   = toEngineering(d.type, si.noise_amp);
    return d;
}

std::vector<SensorDescriptor> VwsDevice::sensors() const {
    const vws_info i = info();
    std::vector<SensorDescriptor> out;
    out.reserve(i.n_sensors);
    for (std::uint32_t id = 0; id < i.n_sensors; ++id)
        out.push_back(sensor(static_cast<std::uint8_t>(id)));
    return out;
}

void VwsDevice::setSampleRate(std::uint32_t hz) {
    ioctlOrThrow(VWS_IOC_SET_RATE, &hz, "VWS_IOC_SET_RATE");
}

void VwsDevice::injectFault(std::uint8_t sensorId, std::uint8_t mode, std::int32_t param) {
    vws_fault_req r{};
    r.sensor_id = sensorId;
    r.mode = mode;
    r.param = param;
    ioctlOrThrow(VWS_IOC_INJECT_FAULT, &r, "VWS_IOC_INJECT_FAULT");
}

void VwsDevice::clearFault(std::uint8_t sensorId) {
    std::uint8_t id = sensorId;
    ioctlOrThrow(VWS_IOC_CLEAR_FAULT, &id, "VWS_IOC_CLEAR_FAULT");
}

void VwsDevice::setEnabled(std::uint8_t sensorId, bool on) {
    vws_enable_req r{};
    r.sensor_id = sensorId;
    r.enable = on ? 1 : 0;
    ioctlOrThrow(VWS_IOC_SET_ENABLE, &r, "VWS_IOC_SET_ENABLE");
}

void VwsDevice::flush() {
    if (::ioctl(fd_, VWS_IOC_FLUSH) != 0)
        throw std::system_error(errno, std::generic_category(), "VWS_IOC_FLUSH");
}

std::vector<vws_sample> VwsDevice::waitAndRead(std::chrono::milliseconds timeout,
                                               std::size_t maxSamples) {
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    const int pr = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (pr < 0) {
        if (errno == EINTR) return {};          // a signal arrived; let the caller look
        throw std::system_error(errno, std::generic_category(), "poll");
    }
    if (pr == 0 || !(pfd.revents & POLLIN))
        return {};

    std::vector<vws_sample> out(maxSamples);
    const ssize_t n = ::read(fd_, out.data(), out.size() * sizeof(vws_sample));
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return {};
        throw std::system_error(errno, std::generic_category(), "read");
    }
    out.resize(static_cast<std::size_t>(n) / sizeof(vws_sample));
    return out;
}

}  // namespace vws
