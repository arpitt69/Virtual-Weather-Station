/*
 * vwstest - offline checks for everything that does not need the driver.
 *
 * The filters, the health state machine, the derived metrics and the fusion
 * engine all run on synthesised readings here, so their behaviour can be
 * pinned down without loading a kernel module. scripts/selftest.sh covers the
 * half that genuinely needs /dev/vws.
 */
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "DerivedMetrics.h"
#include "Filters.h"
#include "FusionEngine.h"
#include "RingBuffer.h"
#include "SampleQueue.h"
#include "SensorHealth.h"

using namespace vws;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string& what) {
    if (cond) { ++g_pass; std::printf("  \033[32mPASS\033[0m  %s\n", what.c_str()); }
    else      { ++g_fail; std::printf("  \033[31mFAIL\033[0m  %s\n", what.c_str()); }
}

void near(double got, double want, double tol, const std::string& what) {
    const bool ok = std::fabs(got - want) <= tol;
    if (!ok)
        std::printf("  \033[31mFAIL\033[0m  %s (got %.4f, want %.4f +/- %.4f)\n",
                    what.c_str(), got, want, tol);
    else
        std::printf("  \033[32mPASS\033[0m  %s (%.4f)\n", what.c_str(), got);
    ok ? ++g_pass : ++g_fail;
}

void section(const char* title) { std::printf("\n\033[1m%s\033[0m\n", title); }

std::uint64_t ns(double seconds) {
    return static_cast<std::uint64_t>(seconds * 1e9);
}

SensorDescriptor desc(std::uint8_t id, const char* name, SensorType t,
                      double lo, double hi, double noise) {
    SensorDescriptor d;
    d.id = id; d.name = name; d.type = t;
    d.rangeMin = lo; d.rangeMax = hi; d.noiseAmp = noise;
    d.enabled = true;
    return d;
}

// -------------------------------------------------------------------------

void testRingBuffer() {
    section("RingBuffer");

    RingBuffer<int> rb(4);
    check(rb.empty() && rb.capacity() == 4, "starts empty with the requested capacity");
    for (int i = 1; i <= 4; ++i) rb.push(i);
    check(rb.full() && rb.size() == 4, "fills to capacity");
    check(rb[0] == 1 && rb[3] == 4, "index 0 is the oldest element");

    rb.push(5);
    check(rb.size() == 4, "stays at capacity after overwriting");
    check(rb[0] == 2 && rb[3] == 5, "oldest element is dropped, not the newest");

    const std::vector<int> snap = rb.snapshot();
    check(snap == std::vector<int>({2, 3, 4, 5}), "snapshot is in chronological order");

    rb.clear();
    check(rb.empty(), "clear() empties it");
}

void testSampleQueue() {
    section("SampleQueue");

    SampleQueue<int> q(3);
    const std::vector<int> in{1, 2, 3, 4, 5};
    q.pushAll(in.begin(), in.end());
    check(q.size() == 3, "bounded: never grows past its limit");
    check(q.dropped() == 2, "counts what it dropped");

    const std::vector<int> out = q.drainFor(std::chrono::milliseconds(1));
    check(out == std::vector<int>({3, 4, 5}), "drops the oldest, keeps the newest");
    check(q.size() == 0, "drain empties it");

    check(!q.popFor(std::chrono::milliseconds(5)).has_value(),
          "popFor times out on an empty queue instead of blocking forever");
}

void testOutlierFilter() {
    section("MedianOutlierFilter");

    MedianOutlierFilter f(-40.0, 85.0);
    std::mt19937 rng(1234);
    std::normal_distribution<double> noise(20.0, 0.05);

    int rejectedClean = 0;
    for (int i = 0; i < 400; ++i)
        if (!f.apply({noise(rng), 0.0025, ns(i * 0.05), 0}).accepted) ++rejectedClean;
    check(rejectedClean <= 8, "passes clean data (rejected " +
                                  std::to_string(rejectedClean) + "/400)");
    near(f.median(), 20.0, 0.1, "tracks the median of the window");

    // Bounded uniform noise on a coarsely quantised channel: the gate is
    // scaled from the reported variance, so it must never fire here at all.
    {
        MedianOutlierFilter q(-40.0, 85.0);
        std::mt19937 qr(31);
        std::uniform_int_distribution<int> levels(-5, 5);
        const double lsb = 0.030;                 // 12-bit ADC over 125 degC
        const double var = (0.15 * 0.15) / 3.0;   // uniform +/-0.15 degC
        int rejected = 0;
        for (int i = 0; i < 1000; ++i)
            if (!q.apply({20.0 + lsb * levels(qr), var, ns(i * 0.05), 0}).accepted)
                ++rejected;
        check(rejected == 0, "bounded noise within the sensor's spec is never "
                             "rejected (rejected " + std::to_string(rejected) + "/1000)");
    }

    check(!f.apply({95.0, 0.0025, ns(100), 0}).accepted,
          "rejects a value above the sensor's physical range");
    check(!f.apply({-80.0, 0.0025, ns(100), 0}).accepted,
          "rejects a value below the sensor's physical range");
    check(!f.apply({std::nan(""), 0.0025, ns(100), 0}).accepted, "rejects NaN");

    const FilterResult jump = f.apply({32.0, 0.0025, ns(101), 0});
    check(!jump.accepted, "rejects an in-range spike as an outlier");
    check(jump.reason.rfind("z=", 0) == 0,
          "reports the z-score as the reason (" + jump.reason + ")");

    // A sustained step must eventually be believed, or a sensor that comes
    // back at a new level could never recover.
    MedianOutlierFilter g(-40.0, 85.0, 15);
    for (int i = 0; i < 15; ++i) g.apply({20.0 + 0.001 * i, 0.0025, ns(i), 0});
    int accepted = 0;
    for (int i = 0; i < 60; ++i)
        if (g.apply({30.0, 0.0025, ns(100 + i), 0}).accepted) ++accepted;
    check(accepted > 20, "a sustained step change is eventually accepted (" +
                             std::to_string(accepted) + "/60)");

    // A channel ramping fast compared with its own noise: a gate measuring
    // deviation from the bare median rejects almost everything here, because
    // the median lags behind the signal. This is the light channel at dawn.
    {
        MedianOutlierFilter r(0.0, 120000.0);
        std::mt19937 rr(5);
        std::normal_distribution<double> rn(0.0, 600.0);   // sigma 600 lux
        int rejected = 0;
        const int n = 600;
        for (int i = 0; i < n; ++i) {
            const double truth = 66.0 * i;                 // 66 lux per sample
            if (!r.apply({truth + rn(rr), 360000.0, ns(i * 0.05), 0}).accepted)
                ++rejected;
        }
        check(rejected < n / 20, "a fast ramp is not mistaken for a stream of "
                                 "outliers (rejected " + std::to_string(rejected) +
                                 "/" + std::to_string(n) + ")");
        check(r.slope() > 50.0 && r.slope() < 85.0,
              "recovers the ramp's slope (" + std::to_string(r.slope()) + " per sample)");

        // ...and a spike riding on top of that ramp is still caught. Use the
        // magnitude the driver actually injects, a sixth of full range.
        const double onTrend = 66.0 * n;
        const double spike = 120000.0 / 6.0;
        check(!r.apply({onTrend + spike, 360000.0, ns(n * 0.05), 0}).accepted,
              "a spike on top of a ramp is still rejected");
    }

    // A window of one repeated value has MAD 0; the gate must stand down
    // rather than reject everything that follows.
    MedianOutlierFilter h(-40.0, 85.0, 15);
    for (int i = 0; i < 20; ++i) h.apply({21.5, 0.0025, ns(i), 0});
    check(h.apply({21.9, 0.0025, ns(30), 0}).accepted,
          "MAD == 0 disables the gate instead of locking the sensor out");
}

void testKalman() {
    section("KalmanFilter1D");

    KalmanFilter1D k(0.01, 100.0);
    check(!k.initialised(), "starts uninitialised");

    k.apply({10.0, 0.25, ns(0), 0});
    check(k.initialised(), "first measurement initialises the state");
    near(k.estimate(), 10.0, 1e-9, "first estimate is the first measurement");

    std::mt19937 rng(7);
    std::normal_distribution<double> noise(25.0, 0.5);
    for (int i = 0; i < 500; ++i) k.apply({noise(rng), 0.25, ns(i), 0});
    near(k.estimate(), 25.0, 0.25, "converges on the true value through noise");
    check(k.variance() < 0.25, "posterior variance drops below the measurement variance");

    // The gain is what makes a noisy sensor count for less.
    KalmanFilter1D a(0.01, 1.0), b(0.01, 1.0);
    for (int i = 0; i < 50; ++i) { a.apply({20.0, 0.01, ns(i), 0}); b.apply({20.0, 4.0, ns(i), 0}); }
    check(a.lastGain() > b.lastGain(),
          "a quiet sensor gets a larger gain than a noisy one (" +
              std::to_string(a.lastGain()) + " vs " + std::to_string(b.lastGain()) + ")");

    k.reset();
    check(!k.initialised(), "reset() clears the state");
}

void testHealth() {
    section("SensorHealthMonitor");

    HealthPolicy pol;

    {
        SensorHealthMonitor m(pol);
        check(m.state() == Health::Ok, "starts OK");

        for (unsigned i = 0; i < pol.suspectAfterRejects; ++i)
            m.onRejected(ns(i * 0.05), "z=9");
        check(m.state() == Health::Suspect, "consecutive rejections raise SUSPECT");

        check(m.usable(), "a SUSPECT sensor still contributes");

        for (unsigned i = pol.suspectAfterRejects; i < pol.faultyAfterRejects; ++i)
            m.onRejected(ns(i * 0.05), "z=9");
        check(m.state() == Health::Faulty, "sustained rejections raise FAULTY");
        check(!m.usable(), "a FAULTY sensor is not usable");

        for (unsigned i = 0; i < pol.recoverAfterAccepts; ++i)
            m.onAccepted(20.0 + 0.01 * i, ns(10 + i * 0.05));
        check(m.state() == Health::Recovering, "a clean run moves FAULTY to RECOVERING");
        check(m.usable(), "a RECOVERING sensor contributes again, on probation");

        m.onRejected(ns(30), "z=9");
        check(m.state() == Health::Faulty, "one bad reading during probation returns to FAULTY");
    }

    {
        SensorHealthMonitor m(pol);
        for (unsigned i = 0; i < pol.stuckAfterRepeats + 1; ++i)
            m.onAccepted(21.5, ns(i * 0.05));
        check(m.state() == Health::Faulty, "identical readings are caught as stuck");
        check(m.lastReason().rfind("stuck", 0) == 0,
              "the reason names the stuck detector (" + m.lastReason() + ")");
        check(m.rejected() == 0, "a stuck sensor is caught without any rejection at all");
    }

    {
        SensorHealthMonitor m(pol);
        const std::uint64_t period = ns(0.05);
        m.onAccepted(20.0, ns(1.0));
        m.onElapsed(ns(1.0) + period * 7, period);
        check(m.state() == Health::Suspect, "a sample gap raises SUSPECT");
        m.onElapsed(ns(1.0) + period * 25, period);
        check(m.state() == Health::Faulty, "prolonged silence raises FAULTY");
    }

    {
        // The rate rule must leave a healthy sensor alone. Rejecting 1 in 50
        // is already far worse than anything this bank does in practice.
        SensorHealthMonitor m(pol);
        for (int i = 0; i < 400; ++i) {
            if (i % 50 == 49) m.onRejected(ns(i * 0.05), "z=4");
            else              m.onAccepted(20.0 + 0.001 * i, ns(i * 0.05));
        }
        check(m.state() == Health::Ok, "a 2% reject rate does not condemn");
        check(m.recentRejectRate() < pol.suspectRejectRate,
              "recent reject rate stays below the SUSPECT gate (" +
                  std::to_string(m.recentRejectRate()) + ")");
    }

    {
        // Interleaved rejections the run-based rules cannot see: never eight
        // in a row, but a third of everything is bad.
        SensorHealthMonitor m(pol);
        for (int i = 0; i < 200; ++i) {
            if (i % 3 == 0) m.onRejected(ns(i * 0.05), "z=9");
            else            m.onAccepted(20.0 + 0.001 * i, ns(i * 0.05));
        }
        check(m.state() == Health::Faulty,
              "a sustained 33% reject rate condemns even with no long run");
        check(m.lastReason().find("% of recent") != std::string::npos,
              "the reason names the rate (" + m.lastReason() + ")");
    }

    {
        SensorHealthMonitor m(pol);
        for (int i = 0; i < 4; ++i) m.onRejected(ns(i * 0.05), "z=9");
        near(m.rejectRate(), 1.0, 1e-9, "reject rate with no accepted samples");
        for (int i = 0; i < 4; ++i) m.onAccepted(20.0 + 0.1 * i, ns(1.0 + i * 0.05));
        near(m.rejectRate(), 0.5, 1e-9, "reject rate counts both outcomes");
    }
}

void testDerived() {
    section("DerivedMetrics");

    near(dewPointC(25.0, 60.0), 16.7, 0.2, "dew point at 25 C / 60% RH");
    near(dewPointC(20.0, 100.0), 20.0, 0.2, "dew point equals the temperature at saturation");
    check(dewPointC(25.0, 30.0) < dewPointC(25.0, 60.0),
          "dew point falls as the air dries out");

    near(heatIndexC(20.0, 60.0), 20.0, 1e-9,
         "heat index returns the dry-bulb below the fit's range");
    near(heatIndexC(32.2, 70.0), 41.1, 1.5, "heat index at 90 F / 70% RH (~106 F)");
    check(heatIndexC(35.0, 80.0) > 35.0, "humid heat feels hotter than it is");

    {
        // Falling pressure: 1 hPa down over 3 simulated hours.
        PressureTrend t(288.0);   // a 300 s simulated day
        check(t.slopePerHour() == 0.0, "no slope before there is a baseline");
        for (int i = 0; i < 200; ++i) {
            const double wallSec = i * 0.1875;            // 10800 sim s / 288
            t.add(ns(wallSec), 1013.0 - 1.0 * (i / 199.0));
        }
        near(t.slopePerHour(), -1.0 / 3.0, 0.05, "least-squares slope, hPa per simulated hour");
        check(t.tendency() == Tendency::Falling, "classified as falling");
    }
    {
        PressureTrend t(288.0);
        for (int i = 0; i < 200; ++i) t.add(ns(i * 0.1875), 1013.0);
        check(t.tendency() == Tendency::Steady, "flat pressure is steady");
    }
}

void testFusion() {
    section("FusionEngine");

    const std::vector<SensorDescriptor> bank{
        desc(0, "temp_a",   SensorType::Temperature, -40, 85, 0.15),
        desc(1, "temp_b",   SensorType::Temperature, -40, 85, 0.60),
        desc(2, "humidity", SensorType::Humidity,      0, 100, 0.8),
        desc(3, "pressure", SensorType::Pressure,    870, 1085, 0.12),
    };

    std::mt19937 rng(99);
    std::normal_distribution<double> qa(0.0, 0.05);   // quiet unit
    std::normal_distribution<double> qb(0.0, 0.40);   // noisy unit

    const auto feed = [&](FusionEngine& e, int ticks, double truth,
                          bool feedA = true, bool feedB = true,
                          double biasB = 0.0) {
        for (int i = 0; i < ticks; ++i) {
            const std::uint64_t t = ns(1.0 + i * 0.05);
            if (feedA) e.ingest(Reading{0, SensorType::Temperature, truth + qa(rng), t, 0});
            if (feedB) e.ingest(Reading{1, SensorType::Temperature, truth + biasB + qb(rng), t, 0});
            // Real readings always move a little; a dead-constant feed would
            // (correctly) trip the stuck detector.
            e.ingest(Reading{2, SensorType::Humidity, 55.0 + 0.01 * (i % 7), t, 0});
            e.ingest(Reading{3, SensorType::Pressure, 1013.0 + 0.001 * (i % 5), t, 0});
            e.tick(t);
        }
    };

    {
        FusionEngine e(bank, 20, 288.0, FusionMode::InverseVariance);
        feed(e, 300, 20.0);
        const FusedSnapshot s = e.snapshot();
        check(s.valid, "publishes a valid snapshot");
        check(s.tempContributors == 2, "both temperature units contribute while healthy");
        check(s.tempSources == "temp_a+temp_b", "names its sources");
        near(s.temperatureC, 20.0, 0.1, "inverse-variance fused temperature");
        check(s.humidityPct && s.pressureHpa, "single-sensor channels pass through");
        check(s.dewPointC.has_value() && s.heatIndexC.has_value(), "derived metrics present");

        const std::vector<SensorView> v = e.sensorViews();
        check(v[0].weight > v[1].weight,
              "the quieter unit carries more weight (" + std::to_string(v[0].weight) +
                  " vs " + std::to_string(v[1].weight) + ")");
        near(v[0].weight + v[1].weight, 1.0, 1e-6, "temperature weights sum to 1");
        check(v[0].noiseSigma < v[1].noiseSigma, "the noise estimate ranks the two units correctly");
    }

    {
        // A biased noisy unit must not drag the estimate far: it is weighted
        // down, but inverse-variance weighting cannot remove a bias outright.
        FusionEngine e(bank, 20, 288.0, FusionMode::InverseVariance);
        feed(e, 300, 20.0, true, true, /*biasB=*/2.0);
        const FusedSnapshot s = e.snapshot();
        check(s.temperatureC < 20.0 + 2.0 * 0.5,
              "a biased noisy unit is pulled toward the quiet one, not the midpoint");
    }

    {
        FusionEngine e(bank, 20, 288.0, FusionMode::Kalman);
        feed(e, 300, 20.0);
        const FusedSnapshot s = e.snapshot();
        near(s.temperatureC, 20.0, 0.1, "Kalman fused temperature");
        check(s.temperatureSigma < 0.1, "Kalman reports a tighter sigma than either sensor");
    }

    {
        // temp_a wedges: the health monitor must condemn it on repeats alone
        // and the estimate must follow the survivor.
        FusionEngine e(bank, 20, 288.0, FusionMode::InverseVariance);
        feed(e, 200, 20.0);
        for (int i = 0; i < 60; ++i) {
            const std::uint64_t t = ns(100.0 + i * 0.05);
            e.ingest(Reading{0, SensorType::Temperature, 20.0, t, VWS_F_STUCK});
            e.ingest(Reading{1, SensorType::Temperature, 26.0 + qb(rng), t, 0});
            e.tick(t);
        }
        const FusedSnapshot s = e.snapshot();
        const std::vector<SensorView> v = e.sensorViews();
        check(v[0].health == Health::Faulty, "the wedged unit goes FAULTY");
        check(s.tempContributors == 1, "only the survivor contributes");
        check(s.tempSources == "temp_b (fallback)", "the snapshot says it is on fallback");
        check(s.temperatureC > 24.0, "the fused value follows the survivor, not the wedged unit");
        near(v[0].weight, 0.0, 1e-9, "a FAULTY sensor carries no weight");

        // The same repeats, but from a sensor sitting at the end of its range,
        // must not be read as a fault.
        FusionEngine z({desc(4, "rain", SensorType::Rain, 0.0, 200.0, 0.0)},
                       20, 288.0, FusionMode::InverseVariance);
        for (int i = 0; i < 200; ++i) {
            const std::uint64_t t = ns(1.0 + i * 0.05);
            z.ingest(Reading{4, SensorType::Rain, 0.0, t, 0});
            z.tick(t);
        }
        check(z.sensorViews()[0].health == Health::Ok,
              "a sensor pinned at a range limit is not called stuck");
    }

    {
        // Both temperature units gone: report nothing rather than something stale.
        FusionEngine e(bank, 20, 288.0, FusionMode::InverseVariance);
        feed(e, 200, 20.0);
        for (int i = 0; i < 60; ++i) {
            const std::uint64_t t = ns(100.0 + i * 0.05);
            e.ingest(Reading{0, SensorType::Temperature, 20.0, t, VWS_F_STUCK});
            e.ingest(Reading{1, SensorType::Temperature, 21.0, t, VWS_F_STUCK});
            e.tick(t);
        }
        const FusedSnapshot s = e.snapshot();
        check(s.tempContributors == 0, "no usable temperature sensor");
        check(s.tempSources == "none", "says so explicitly");
        check(!s.dewPointC.has_value(), "derived metrics are withheld without a temperature");
    }

    {
        // Dropout: nothing arrives at all, so only staleness can catch it.
        FusionEngine e(bank, 20, 288.0, FusionMode::InverseVariance);
        feed(e, 200, 20.0);
        for (int i = 0; i < 40; ++i) {
            const std::uint64_t t = ns(100.0 + i * 0.05);
            e.ingest(Reading{0, SensorType::Temperature, 20.0 + qa(rng), t, 0});
            e.ingest(Reading{1, SensorType::Temperature, 20.0 + qb(rng), t, 0});
            e.ingest(Reading{3, SensorType::Pressure, 1013.0 + 0.001 * (i % 5), t, 0});
            e.tick(t);                       // humidity sends nothing
        }
        const FusedSnapshot s = e.snapshot();
        const std::vector<SensorView> v = e.sensorViews();
        check(v[2].health == Health::Faulty, "a silent sensor is caught by staleness");
        check(!s.humidityPct.has_value(), "its channel is withheld from the snapshot");
        check(s.pressureHpa.has_value(), "the other channels are unaffected");
    }

    {
        /*
         * Heavy spiking from a cold start. The gate does not exist until the
         * window holds kMinForMad samples, so the opening spikes are accepted
         * - and an earlier build fed the engine's own variance estimate back
         * into the gate's threshold, which turned that into a permanent
         * failure: the accepted spikes inflated the estimate, the estimate
         * widened the gate past the spike magnitude, and nothing was ever
         * rejected again. 600 of 600 accepted, every spike included.
         */
        const std::vector<SensorDescriptor> one{
            desc(1, "temp_b", SensorType::Temperature, -40, 85, 0.6)};
        FusionEngine e(one, 20, 288.0, FusionMode::InverseVariance);

        std::mt19937 sr(3);
        std::normal_distribution<double> sn(0.0, 0.35);
        std::uniform_int_distribution<int> pct(0, 99);
        const double spike = (85.0 - -40.0) / 6.0;      // what the driver injects

        for (int i = 0; i < 600; ++i) {
            double v = 20.0 + sn(sr);
            if (pct(sr) < 40) v += (i & 1) ? spike : -spike;
            const std::uint64_t t = ns(1.0 + i * 0.05);
            e.ingest(Reading{1, SensorType::Temperature, v, t, 0});
            e.tick(t);
        }

        const SensorView v = e.sensorViews()[0];
        check(v.rejected > 100, "spikes are still caught at 40% contamination "
                                "from a cold start (rejected " +
                                    std::to_string(v.rejected) + "/600)");
        check(v.health == Health::Faulty,
              "a sensor spiking 40% of the time is condemned");
    }

    {
        FusionEngine e(bank, 20, 288.0, FusionMode::Kalman);
        check(e.mode() == FusionMode::Kalman, "mode is readable");
        e.setMode(FusionMode::InverseVariance);
        check(e.mode() == FusionMode::InverseVariance, "mode can be switched at runtime");

        SensorDescriptor off = bank[1];
        off.enabled = false;
        off.faultMode = VWS_FAULT_STUCK;
        e.syncDescriptors({off});
        feed(e, 60, 20.0);
        const FusedSnapshot s = e.snapshot();
        check(s.tempContributors == 1, "a sensor disabled out-of-band stops contributing");
        const std::vector<SensorView> v = e.sensorViews();
        check(v[1].faultMode == VWS_FAULT_STUCK, "fault state picked up from the descriptor");
    }

    {
        FusionEngine e(bank, 20, 288.0, FusionMode::InverseVariance);
        e.ingest(Reading{99, SensorType::Temperature, 20.0, ns(1), 0});
        check(e.ingested() == 0, "a reading from an unknown sensor id is ignored");
    }
}

}  // namespace

int main() {
    std::printf("\033[1mvwstest - offline checks (no /dev/vws needed)\033[0m\n");

    testRingBuffer();
    testSampleQueue();
    testOutlierFilter();
    testKalman();
    testHealth();
    testDerived();
    testFusion();

    std::printf("\n\033[1m%d passed, %d failed\033[0m\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
