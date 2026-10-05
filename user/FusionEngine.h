#pragma once
/*
 * FusionEngine.h - turn a stream of noisy, occasionally broken readings into
 * one trustworthy snapshot.
 *
 * Pipeline per reading:
 *   outlier filter -> health monitor -> noise variance estimate -> fusion
 * and then, once per tick, the derived metrics.
 *
 * Only the two temperature sensors are redundant, so they are the ones that
 * get fused; every other channel is filtered and health-monitored but passes
 * through on its own. If one temperature sensor goes FAULTY the engine falls
 * back to the survivor without any special case: a FAULTY sensor simply stops
 * contributing measurements.
 */
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "DerivedMetrics.h"
#include "Filters.h"
#include "RingBuffer.h"
#include "SensorHealth.h"
#include "Units.h"
#include "VwsDevice.h"

namespace vws {

enum class FusionMode { Kalman, InverseVariance };

const char* fusionModeName(FusionMode m) noexcept;

/// What the dashboard and the CSV logger need to know about one sensor.
struct SensorView {
    std::uint8_t id = 0;
    SensorType   type = SensorType::Temperature;
    std::string  name;
    bool         enabled = true;

    bool   haveRaw = false;
    double lastRaw = 0.0;
    bool   haveAccepted = false;
    double lastAccepted = 0.0;

    Health      health = Health::Ok;
    std::string reason;
    double      noiseSigma = 0.0;      // estimated 1-sigma, engineering units
    double      weight = 0.0;          // share of the fused value, 0..1

    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint8_t  faultMode = VWS_FAULT_NONE;
    std::int32_t  faultParam = 0;
    std::uint16_t lastFlags = 0;
};

struct FusedSnapshot {
    bool   valid = false;
    std::uint64_t tNs = 0;

    double temperatureC = 0.0;
    double temperatureSigma = 0.0;
    unsigned tempContributors = 0;
    std::string tempSources;        // e.g. "temp_a+temp_b" or "temp_b (fallback)"

    std::optional<double> humidityPct;
    std::optional<double> pressureHpa;
    std::optional<double> windMs;
    std::optional<double> rainMmH;
    std::optional<double> lightLux;

    std::optional<double> dewPointC;
    std::optional<double> heatIndexC;

    double   pressureSlopeHpaPerHour = 0.0;
    Tendency tendency = Tendency::Steady;
};

class FusionEngine {
public:
    FusionEngine(std::vector<SensorDescriptor> descriptors,
                 std::uint32_t sampleRateHz,
                 double dayScale,
                 FusionMode mode = FusionMode::Kalman);

    /// Push one reading through the pipeline. Called from the fusion thread.
    void ingest(const Reading& r);

    /// Re-evaluate staleness and rebuild the published snapshot.
    void tick(std::uint64_t nowNs);

    FusedSnapshot snapshot() const;
    std::vector<SensorView> sensorViews() const;

    void setMode(FusionMode m);
    FusionMode mode() const;

    /// Pick up fault/enable changes made out-of-band (sysfs, or vwsctl).
    void syncDescriptors(const std::vector<SensorDescriptor>& descs);

    std::uint64_t ingested() const;
    std::uint64_t rejectedTotal() const;

private:
    struct SensorState {
        SensorDescriptor desc;
        std::unique_ptr<MedianOutlierFilter> outlier;
        SensorHealthMonitor health;
        RingBuffer<double> accepted{64};   // for the noise estimate
        double lastRaw = 0.0;
        bool   haveRaw = false;
        double lastAccepted = 0.0;
        bool   haveAccepted = false;
        std::uint16_t lastFlags = 0;
        double weight = 0.0;

        explicit SensorState(SensorDescriptor d);
        /// Noise variance from the mean square of successive differences:
        /// Var(noise) ~= Var(dx)/2 for a signal that moves slowly compared
        /// with the sampling rate, which is far more robust than the plain
        /// window variance when the true value is on a diurnal ramp.
        double noiseVariance() const;
    };

    SensorState* find(std::uint8_t id);
    void fuseTemperature(std::uint64_t nowNs);
    void rebuildSnapshot(std::uint64_t nowNs);

    mutable std::mutex m_;

    std::vector<SensorState> sensors_;
    std::vector<std::uint8_t> tempIds_;

    std::unique_ptr<KalmanFilter1D> tempKalman_;
    PressureTrend trend_;
    FusionMode mode_;

    std::uint64_t samplePeriodNs_;
    std::uint64_t ingested_ = 0;
    std::uint64_t rejected_ = 0;

    FusedSnapshot snap_;
};

}  // namespace vws
