/*
 * vwsd - the Virtual Weather Station daemon.
 *
 * Threads:
 *   reader  blocks in poll()/read() on /dev/vws, converts to engineering
 *           units, pushes onto the queue
 *   fusion  drains the queue, runs the filter/health/fusion pipeline
 *   main    renders (ncurses or plain text), handles keys, logs CSV
 *
 * The main thread owns everything that must be torn down in order, which is
 * why shutdown is: stop reader -> close queue -> join fusion -> endwin().
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "CsvLogger.h"
#include "Dashboard.h"
#include "FusionEngine.h"
#include "SampleQueue.h"
#include "SensorReader.h"
#include "VwsDevice.h"

using namespace vws;
using namespace std::chrono_literals;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

std::uint64_t monotonicNs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

struct Options {
    std::string device = "/dev/vws";
    std::string csvPrefix;
    std::uint32_t rateHz = 0;        // 0 = leave the driver's setting alone
    FusionMode mode = FusionMode::Kalman;
    bool headless = false;
    int intervalMs = 1000;
    int durationSec = 0;             // 0 = until interrupted
};

void usage(const char* argv0) {
    std::cout <<
        "usage: " << argv0 << " [options]\n"
        "  --device PATH     character device to read (default /dev/vws)\n"
        "  --rate HZ         set the driver's sampling clock before streaming\n"
        "  --fusion MODE     kalman (default) or weighted\n"
        "  --csv PREFIX      log to PREFIX_fused.csv and PREFIX_sensors.csv\n"
        "  --headless        plain stdout instead of the ncurses dashboard\n"
        "  --interval MS     headless print interval (default 1000)\n"
        "  --duration SEC    exit after SEC seconds (default: run until Ctrl-C)\n"
        "  -h, --help        this text\n";
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "vwsd: " << what << " needs a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") { usage(argv[0]); return false; }
        else if (a == "--device")   { const char* v = next("--device");   if (!v) return false; o.device = v; }
        else if (a == "--csv")      { const char* v = next("--csv");      if (!v) return false; o.csvPrefix = v; }
        else if (a == "--rate")     { const char* v = next("--rate");     if (!v) return false; o.rateHz = std::strtoul(v, nullptr, 10); }
        else if (a == "--interval") { const char* v = next("--interval"); if (!v) return false; o.intervalMs = std::atoi(v); }
        else if (a == "--duration") { const char* v = next("--duration"); if (!v) return false; o.durationSec = std::atoi(v); }
        else if (a == "--headless") { o.headless = true; }
        else if (a == "--fusion") {
            const char* v = next("--fusion");
            if (!v) return false;
            if (!std::strcmp(v, "kalman")) o.mode = FusionMode::Kalman;
            else if (!std::strcmp(v, "weighted")) o.mode = FusionMode::InverseVariance;
            else { std::cerr << "vwsd: unknown fusion mode '" << v << "'\n"; return false; }
        } else {
            std::cerr << "vwsd: unexpected argument '" << a << "'\n";
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

void printHeadless(const FusedSnapshot& snap, const std::vector<SensorView>& views,
                   const vws_stats& ks, FusionMode mode) {
    std::printf("---- t=%.3fs  fusion=%s  kernel: gen=%llu read=%llu drop=%llu fifo=%u\n",
                static_cast<double>(snap.tNs) * 1e-9, fusionModeName(mode),
                static_cast<unsigned long long>(ks.samples_generated),
                static_cast<unsigned long long>(ks.samples_read),
                static_cast<unsigned long long>(ks.samples_dropped),
                ks.fifo_used);

    for (const SensorView& v : views) {
        const std::string why = v.reason.empty() ? std::string{} : "  [" + v.reason + "]";
        std::printf("  %-10s %-12s raw=%12.3f acc=%12.3f %-8s w=%.2f sigma=%.4f "
                    "acc/rej=%llu/%llu fault=%s%s\n",
                    v.name.c_str(), typeName(v.type),
                    v.haveRaw ? v.lastRaw : 0.0,
                    v.haveAccepted ? v.lastAccepted : 0.0,
                    v.enabled ? healthName(v.health) : "OFF",
                    v.weight, v.noiseSigma,
                    static_cast<unsigned long long>(v.accepted),
                    static_cast<unsigned long long>(v.rejected),
                    faultName(v.faultMode), why.c_str());
    }

    if (snap.tempContributors)
        std::printf("  FUSED temp=%.3f +/- %.3f C from %s\n",
                    snap.temperatureC, snap.temperatureSigma, snap.tempSources.c_str());
    else
        std::printf("  FUSED temp=UNAVAILABLE (%s)\n", snap.tempSources.c_str());

    // An absent channel prints as "--" rather than a zero, so a sensor that
    // dropped out cannot be mistaken for one reading zero.
    const auto opt = [](const std::optional<double>& v, int prec) {
        if (!v) return std::string("--");
        char b[32];
        std::snprintf(b, sizeof(b), "%.*f", prec, *v);
        return std::string(b);
    };

    std::printf("        humidity=%s%%RH pressure=%shPa wind=%sm/s rain=%smm/h light=%slux\n",
                opt(snap.humidityPct, 2).c_str(), opt(snap.pressureHpa, 2).c_str(),
                opt(snap.windMs, 2).c_str(), opt(snap.rainMmH, 2).c_str(),
                opt(snap.lightLux, 0).c_str());

    if (snap.dewPointC)
        std::printf("        dewpoint=%.2f C heatindex=%.2f C tendency=%s (%+.3f hPa/sim-h)\n",
                    *snap.dewPointC, snap.heatIndexC ? *snap.heatIndexC : 0.0,
                    tendencyName(snap.tendency), snap.pressureSlopeHpaPerHour);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    // deliberately not SA_RESTART: poll() should return EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    std::unique_ptr<VwsDevice> dev;
    try {
        dev = std::make_unique<VwsDevice>(opt.device, /*writable=*/true);
        if (opt.rateHz) dev->setSampleRate(opt.rateHz);
    } catch (const std::exception& e) {
        std::cerr << "vwsd: " << e.what() << "\n"
                  << "hint: is the module loaded?  sudo ./scripts/load.sh\n";
        return 1;
    }

    const vws_info info = dev->info();
    const double dayScale = info.day_seconds ? 86400.0 / info.day_seconds : 1.0;
    auto descriptors = dev->sensors();

    SampleQueue<Reading> queue(8192);
    FusionEngine engine(descriptors, info.sample_rate_hz, dayScale, opt.mode);
    SensorReader reader(*dev, queue);

    std::unique_ptr<CsvLogger> logger;
    if (!opt.csvPrefix.empty()) {
        try {
            logger = std::make_unique<CsvLogger>(opt.csvPrefix);
        } catch (const std::exception& e) {
            std::cerr << "vwsd: " << e.what() << "\n";
            return 1;
        }
    }

    // Fusion thread: drain whatever the reader has queued, then advance the
    // engine's clock so staleness is noticed even when nothing arrives.
    std::atomic<bool> fusionStop{false};
    std::thread fusionThread([&] {
        while (!fusionStop.load()) {
            for (const Reading& r : queue.drainFor(50ms))
                engine.ingest(r);
            engine.tick(monotonicNs());
        }
    });

    reader.start();

    std::unique_ptr<Dashboard> ui;
    if (!opt.headless) {
        ui = std::make_unique<Dashboard>();
        if (!ui->usable()) {
            ui.reset();
            opt.headless = true;
            std::cerr << "vwsd: no usable terminal, falling back to --headless\n";
        }
    }

    const std::uint64_t startNs = monotonicNs();
    std::uint64_t lastPrintNs = 0;
    std::uint64_t lastSyncNs = 0;
    int exitCode = 0;

    while (!g_stop) {
        if (reader.failed()) {
            exitCode = 1;
            break;
        }
        if (opt.durationSec > 0 &&
            monotonicNs() - startNs >= static_cast<std::uint64_t>(opt.durationSec) * 1'000'000'000ULL)
            break;

        const std::uint64_t now = monotonicNs();

        // Pick up changes made behind our back through sysfs or vwsctl.
        if (now - lastSyncNs > 500'000'000ULL) {
            lastSyncNs = now;
            try {
                descriptors = dev->sensors();
                engine.syncDescriptors(descriptors);
            } catch (const std::exception&) { /* transient; try again next pass */ }
        }

        const FusedSnapshot snap = engine.snapshot();
        const std::vector<SensorView> views = engine.sensorViews();
        vws_stats kstats{};
        try { kstats = dev->stats(); } catch (const std::exception&) {}

        if (logger) logger->log(snap, views);

        if (ui) {
            RuntimeInfo rt;
            rt.sampleRateHz = dev->info().sample_rate_hz;
            rt.daySeconds = info.day_seconds;
            rt.fusionMode = fusionModeName(engine.mode());
            rt.logging = logger != nullptr;
            rt.logPath = logger ? logger->fusedPath() : "";
            rt.queueDepth = queue.size();
            rt.queueDropped = queue.dropped();
            rt.readerSamples = reader.samplesRead();
            rt.resyncs = reader.resyncs();
            rt.engineIngested = engine.ingested();
            rt.engineRejected = engine.rejectedTotal();

            ui->render(snap, views, kstats, rt);

            bool quit = false;
            for (UiCommand cmd = ui->poll(); cmd.kind != UiCommand::Kind::None;
                 cmd = ui->poll()) {
                try {
                    switch (cmd.kind) {
                    case UiCommand::Kind::Quit:
                        quit = true;
                        break;
                    case UiCommand::Kind::ToggleFusion:
                        engine.setMode(engine.mode() == FusionMode::Kalman
                                           ? FusionMode::InverseVariance
                                           : FusionMode::Kalman);
                        break;
                    case UiCommand::Kind::ToggleLogging:
                        if (logger) {
                            logger.reset();
                        } else {
                            logger = std::make_unique<CsvLogger>(
                                opt.csvPrefix.empty() ? "vws" : opt.csvPrefix);
                        }
                        break;
                    case UiCommand::Kind::ClearFaults:
                        dev->clearFault(VWS_ALL_SENSORS);
                        break;
                    case UiCommand::Kind::InjectFault:
                        dev->injectFault(cmd.sensorId, cmd.faultMode, cmd.faultParam);
                        break;
                    case UiCommand::Kind::ToggleEnable:
                        for (const auto& d : descriptors)
                            if (d.id == cmd.sensorId) dev->setEnabled(d.id, !d.enabled);
                        break;
                    case UiCommand::Kind::RateUp:
                        dev->setSampleRate(std::min<std::uint32_t>(
                            1000, dev->info().sample_rate_hz * 2));
                        break;
                    case UiCommand::Kind::RateDown:
                        dev->setSampleRate(std::max<std::uint32_t>(
                            1, dev->info().sample_rate_hz / 2));
                        break;
                    case UiCommand::Kind::None:
                        break;
                    }
                } catch (const std::exception&) { /* ignore a rejected ioctl */ }
                if (quit) break;
            }
            if (quit) break;
            std::this_thread::sleep_for(100ms);
        } else {
            if (now - lastPrintNs >= static_cast<std::uint64_t>(opt.intervalMs) * 1'000'000ULL) {
                lastPrintNs = now;
                printHeadless(snap, views, kstats, engine.mode());
            }
            std::this_thread::sleep_for(50ms);
        }
    }

    reader.stop();
    queue.close();
    fusionStop.store(true);
    if (fusionThread.joinable()) fusionThread.join();
    ui.reset();          // restores the terminal before anything is printed
    if (logger) logger->flush();

    if (reader.failed())
        std::cerr << "vwsd: reader thread failed: " << reader.error() << "\n";
    else
        std::cerr << "vwsd: stopped after " << reader.samplesRead()
                  << " samples (" << reader.readCalls() << " read calls, "
                  << reader.resyncs() << " resyncs)\n";

    return exitCode;
}
