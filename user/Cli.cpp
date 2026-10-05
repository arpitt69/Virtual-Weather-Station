/*
 * vwsctl - configure the driver and inject faults from the shell.
 *
 * Everything here is also reachable through sysfs; this exists to exercise
 * the ioctl path (and because `vwsctl inject temp_a stuck` is easier to type
 * than two echoes into /sys).
 */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ctime>
#include <string>
#include <vector>

#include "Units.h"
#include "VwsDevice.h"

using namespace vws;

namespace {

const char* kDevice = "/dev/vws";

void usage() {
    std::cout <<
        "usage: vwsctl [--device PATH] <command> [args]\n"
        "\n"
        "  info                          driver and ABI information\n"
        "  sensors                       list the sensor bank\n"
        "  stats                         FIFO and sampling counters\n"
        "  rate <hz>                     set the sampling clock (1-1000)\n"
        "  inject <sensor> <mode> [p]    inject a fault\n"
        "  clear <sensor>                clear a sensor's fault\n"
        "  enable <sensor> <0|1>         enable or disable a sensor\n"
        "  flush                         discard whatever is in the FIFO\n"
        "  watch [count]                 print raw samples (default 20)\n"
        "  stall <seconds>               hold the device open without reading,\n"
        "                                then report what the overflow cost\n"
        "\n"
        "  <sensor> is an id, a name (temp_a), or 'all'\n"
        "  <mode>   none stuck drift spike dropout noise\n"
        "\n"
        "fault parameters:\n"
        "  stuck    (none)            repeat the last value\n"
        "  drift    milli-units/sample\n"
        "  spike    percent of samples\n"
        "  dropout  percent of samples suppressed\n"
        "  noise    milli-unit amplitude\n";
}

/// Resolve an id, a sensor name, or "all" to a sensor id.
bool resolveSensor(const VwsDevice& dev, const std::string& token, std::uint8_t& out) {
    if (token == "all" || token == "ALL") {
        out = VWS_ALL_SENSORS;
        return true;
    }
    const std::vector<SensorDescriptor> all = dev.sensors();

    char* end = nullptr;
    const long n = std::strtol(token.c_str(), &end, 10);
    if (end && *end == '\0') {
        for (const auto& d : all)
            if (d.id == n) { out = d.id; return true; }
        std::cerr << "vwsctl: no sensor with id " << n << "\n";
        return false;
    }

    for (const auto& d : all)
        if (d.name == token) { out = d.id; return true; }

    std::cerr << "vwsctl: no sensor named '" << token << "'\n";
    return false;
}

bool parseMode(const std::string& token, std::uint8_t& out) {
    static const char* names[] = {"none", "stuck", "drift", "spike", "dropout", "noise"};
    for (std::uint8_t i = 0; i < VWS_FAULT_COUNT; ++i) {
        if (token == names[i]) { out = i; return true; }
    }
    std::cerr << "vwsctl: unknown fault mode '" << token << "'\n";
    return false;
}

void cmdInfo(const VwsDevice& dev) {
    const vws_info i = dev.info();
    std::printf("device:         %s\n", dev.path().c_str());
    std::printf("abi_version:    %u\n", i.abi_version);
    std::printf("sensors:        %u\n", i.n_sensors);
    std::printf("sample_rate:    %u Hz\n", i.sample_rate_hz);
    std::printf("fifo_depth:     %u samples (%zu bytes)\n", i.fifo_depth,
                static_cast<std::size_t>(i.fifo_depth) * sizeof(vws_sample));
    std::printf("simulated day:  %u s of wall time\n", i.day_seconds);
    std::printf("sample record:  %zu bytes\n", sizeof(vws_sample));
}

void cmdSensors(const VwsDevice& dev) {
    std::printf("%-3s %-10s %-12s %-8s %-9s %-7s %14s %14s\n",
                "id", "name", "type", "enabled", "fault", "param", "range_lo", "range_hi");
    for (const SensorDescriptor& d : dev.sensors()) {
        std::printf("%-3u %-10s %-12s %-8s %-9s %-7d %14.3f %14.3f  %s\n",
                    static_cast<unsigned>(d.id), d.name.c_str(), typeName(d.type),
                    d.enabled ? "yes" : "no", faultName(d.faultMode), d.faultParam,
                    d.rangeMin, d.rangeMax, unitName(d.type));
    }
}

void cmdStats(const VwsDevice& dev) {
    const vws_stats s = dev.stats();
    std::printf("timer_ticks:       %llu\n", (unsigned long long)s.timer_ticks);
    std::printf("samples_generated: %llu\n", (unsigned long long)s.samples_generated);
    std::printf("samples_read:      %llu\n", (unsigned long long)s.samples_read);
    std::printf("samples_dropped:   %llu\n", (unsigned long long)s.samples_dropped);
    std::printf("fifo_overflows:    %llu\n", (unsigned long long)s.fifo_overflows);
    std::printf("dropouts_injected: %llu\n", (unsigned long long)s.dropouts_injected);
    std::printf("fifo_used:         %u\n", s.fifo_used);
    std::printf("readers:           %u\n", s.readers);
}

void cmdWatch(VwsDevice& dev, int count) {
    std::printf("%14s %-3s %-12s %14s %-6s %s\n",
                "t_ns", "id", "type", "value", "flags", "notes");
    int printed = 0;
    while (printed < count) {
        const std::vector<vws_sample> raw = dev.waitAndRead(std::chrono::milliseconds(500));
        for (const vws_sample& s : raw) {
            if (printed++ >= count) break;
            const auto t = static_cast<SensorType>(s.type);
            std::string notes;
            if (s.flags & VWS_F_FAULT)     notes += "fault ";
            if (s.flags & VWS_F_STUCK)     notes += "stuck ";
            if (s.flags & VWS_F_SPIKE)     notes += "spike ";
            if (s.flags & VWS_F_SATURATED) notes += "saturated ";
            if (s.flags & VWS_F_RESYNC)    notes += "resync ";
            std::printf("%14llu %-3u %-12s %14.3f 0x%04x %s\n",
                        (unsigned long long)s.timestamp_ns,
                        static_cast<unsigned>(s.sensor_id), typeName(t),
                        toEngineering(t, s.value), s.flags, notes.c_str());
        }
    }
}

/**
 * Deliberately starve the reader to exercise the driver's overflow path.
 *
 * The interesting number is the age of the first sample that comes back. On
 * an overwrite ring it is bounded by the ring's own span (depth / total
 * sample rate) however long the stall lasted, because the ring always holds
 * the most recent `depth` samples. On a ring that discards the incoming
 * sample instead, it would be as old as the stall itself - the reader would
 * be handed the backlog from the moment the ring filled. So this one
 * measurement distinguishes the two policies.
 */
void cmdStall(VwsDevice& dev, double seconds) {
    const vws_info info = dev.info();
    const unsigned enabled = [&] {
        unsigned n = 0;
        for (const SensorDescriptor& d : dev.sensors()) n += d.enabled ? 1 : 0;
        return n;
    }();
    const double totalRate = static_cast<double>(info.sample_rate_hz) * enabled;
    const double ringSpan = totalRate > 0.0 ? info.fifo_depth / totalRate : 0.0;

    std::printf("stalling %.1f s: %u sensors at %u Hz = %.0f samples/s into a "
                "%u-sample ring\n",
                seconds, enabled, info.sample_rate_hz, totalRate, info.fifo_depth);
    std::printf("ring spans %.3f s of data; expect the backlog to be capped at "
                "that, not at %.1f s\n", ringSpan, seconds);

    dev.flush();

    struct timespec req{};
    req.tv_sec = static_cast<time_t>(seconds);
    req.tv_nsec = static_cast<long>((seconds - static_cast<double>(req.tv_sec)) * 1e9);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) { }

    const vws_stats st = dev.stats();
    const std::vector<vws_sample> raw = dev.waitAndRead(std::chrono::milliseconds(500));

    struct timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const std::uint64_t nowNs = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
                                static_cast<std::uint64_t>(now.tv_nsec);

    std::printf("\noverflow_episodes: %llu\n", (unsigned long long)st.fifo_overflows);
    std::printf("samples_dropped:   %llu\n", (unsigned long long)st.samples_dropped);
    std::printf("samples_generated: %llu\n", (unsigned long long)st.samples_generated);
    std::printf("fifo_used:         %u/%u\n", st.fifo_used, info.fifo_depth);

    if (raw.empty()) {
        std::printf("first_sample_age:  no samples returned\n");
        return;
    }

    const double age = static_cast<double>(nowNs - raw.front().timestamp_ns) * 1e-9;
    unsigned resync = 0;
    for (const vws_sample& smp : raw)
        if (smp.flags & VWS_F_RESYNC) ++resync;

    std::printf("first_sample_age:  %.3f s\n", age);
    std::printf("resync_flagged:    %u of %zu samples in the first read\n",
                resync, raw.size());
}

}  // namespace

int main(int argc, char** argv) {
    std::string device = kDevice;
    int argi = 1;

    if (argi < argc && !std::strcmp(argv[argi], "--device")) {
        if (argi + 1 >= argc) { std::cerr << "vwsctl: --device needs a value\n"; return 1; }
        device = argv[argi + 1];
        argi += 2;
    }

    if (argi >= argc) { usage(); return 1; }
    const std::string cmd = argv[argi++];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") { usage(); return 0; }

    try {
        VwsDevice dev(device, /*writable=*/true);

        if (cmd == "info")    { cmdInfo(dev); return 0; }
        if (cmd == "sensors") { cmdSensors(dev); return 0; }
        if (cmd == "stats")   { cmdStats(dev); return 0; }
        if (cmd == "flush")   { dev.flush(); std::cout << "FIFO flushed\n"; return 0; }

        if (cmd == "stall") {
            const double sec = (argi < argc) ? std::atof(argv[argi]) : 3.0;
            cmdStall(dev, sec > 0.0 ? sec : 3.0);
            return 0;
        }

        if (cmd == "watch") {
            const int n = (argi < argc) ? std::atoi(argv[argi]) : 20;
            cmdWatch(dev, n > 0 ? n : 20);
            return 0;
        }

        if (cmd == "rate") {
            if (argi >= argc) { std::cerr << "vwsctl: rate needs a frequency\n"; return 1; }
            const auto hz = static_cast<std::uint32_t>(std::strtoul(argv[argi], nullptr, 10));
            dev.setSampleRate(hz);
            std::cout << "sampling clock set to " << hz << " Hz\n";
            return 0;
        }

        if (cmd == "clear") {
            if (argi >= argc) { std::cerr << "vwsctl: clear needs a sensor\n"; return 1; }
            std::uint8_t id = 0;
            if (!resolveSensor(dev, argv[argi], id)) return 1;
            dev.clearFault(id);
            std::cout << "fault cleared\n";
            return 0;
        }

        if (cmd == "enable") {
            if (argi + 1 >= argc) { std::cerr << "vwsctl: enable needs a sensor and 0|1\n"; return 1; }
            std::uint8_t id = 0;
            if (!resolveSensor(dev, argv[argi], id)) return 1;
            if (id == VWS_ALL_SENSORS) { std::cerr << "vwsctl: enable needs one sensor\n"; return 1; }
            const bool on = std::atoi(argv[argi + 1]) != 0;
            dev.setEnabled(id, on);
            std::cout << "sensor " << static_cast<unsigned>(id)
                      << (on ? " enabled\n" : " disabled\n");
            return 0;
        }

        if (cmd == "inject") {
            if (argi + 1 >= argc) { std::cerr << "vwsctl: inject needs a sensor and a mode\n"; return 1; }
            std::uint8_t id = 0, mode = 0;
            if (!resolveSensor(dev, argv[argi], id)) return 1;
            if (!parseMode(argv[argi + 1], mode)) return 1;
            const std::int32_t param =
                (argi + 2 < argc) ? static_cast<std::int32_t>(std::strtol(argv[argi + 2], nullptr, 10)) : 0;
            dev.injectFault(id, mode, param);
            std::printf("injected %s (param %d) into sensor %s\n",
                        faultName(mode), param,
                        id == VWS_ALL_SENSORS ? "all" : std::to_string(id).c_str());
            return 0;
        }

        std::cerr << "vwsctl: unknown command '" << cmd << "'\n";
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "vwsctl: " << e.what() << "\n";
        return 1;
    }
}
