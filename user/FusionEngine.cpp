#include "FusionEngine.h"

#include <algorithm>
#include <cmath>

namespace vws {

const char* fusionModeName(FusionMode m) noexcept {
    switch (m) {
    case FusionMode::Kalman:          return "kalman-1d";
    case FusionMode::InverseVariance: return "inverse-variance";
    }
    return "?";
}

FusionEngine::SensorState::SensorState(SensorDescriptor d) : desc(std::move(d)) {
    outlier = std::make_unique<MedianOutlierFilter>(desc.rangeMin, desc.rangeMax);
}

double FusionEngine::SensorState::noiseVariance() const {
    const std::size_t n = accepted.size();
    if (n < 4) {
        // Nothing measured yet: fall back on what the driver claims, treating
        // the uniform noise amplitude as +/-3 sigma.
        const double sigma = desc.noiseAmp > 0.0 ? desc.noiseAmp / 3.0 : 0.05;
        return sigma * sigma;
    }

    double sumSq = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
        const double d = accepted[i] - accepted[i - 1];
        sumSq += d * d;
    }
    const double var = sumSq / (2.0 * static_cast<double>(n - 1));
    return std::max(var, 1e-6);
}

FusionEngine::FusionEngine(std::vector<SensorDescriptor> descriptors,
                           std::uint32_t sampleRateHz,
                           double dayScale,
                           FusionMode mode)
    : trend_(dayScale), mode_(mode),
      samplePeriodNs_(sampleRateHz ? 1'000'000'000ULL / sampleRateHz : 50'000'000ULL) {
    sensors_.reserve(descriptors.size());
    for (auto& d : descriptors) {
        if (d.type == SensorType::Temperature) tempIds_.push_back(d.id);
        sensors_.emplace_back(std::move(d));
    }
    tempKalman_ = std::make_unique<KalmanFilter1D>();
}

FusionEngine::SensorState* FusionEngine::find(std::uint8_t id) {
    for (auto& s : sensors_)
        if (s.desc.id == id) return &s;
    return nullptr;
}

void FusionEngine::ingest(const Reading& r) {
    std::lock_guard<std::mutex> lk(m_);

    SensorState* s = find(r.sensorId);
    if (!s) return;   // a sensor this build does not know about

    ++ingested_;
    s->lastRaw = r.value;
    s->haveRaw = true;
    s->lastFlags = r.flags;

    const double variance = s->noiseVariance();
    const FilterInput in{r.value, variance, r.tNs, r.flags};
    const FilterResult res = s->outlier->apply(in);

    if (!res.accepted) {
        ++rejected_;
        s->health.onRejected(r.tNs, res.reason);
        return;
    }

    // A reading pinned at an end of its range repeats for honest reasons, so
    // the health monitor must not read those repeats as a wedged ADC.
    const double limitEps = 1e-9 + 1e-6 * (s->desc.rangeMax - s->desc.rangeMin);
    const bool atLimit = (r.flags & VWS_F_SATURATED) ||
                         res.value <= s->desc.rangeMin + limitEps ||
                         res.value >= s->desc.rangeMax - limitEps;

    s->health.onAccepted(res.value, r.tNs, atLimit);
    s->lastAccepted = res.value;
    s->haveAccepted = true;
    s->accepted.push(res.value);

    // A sensor the health monitor has condemned is filtered and counted, but
    // it no longer gets to move any estimate.
    if (!s->health.usable()) return;

    if (s->desc.type == SensorType::Temperature && mode_ == FusionMode::Kalman) {
        // Sequential measurement update: one call per sensor per tick with
        // that sensor's own variance, which makes the gain favour the quieter
        // unit without any explicit weighting.
        tempKalman_->apply(FilterInput{res.value, s->noiseVariance(), r.tNs, r.flags});
    } else if (s->desc.type == SensorType::Pressure) {
        trend_.add(r.tNs, res.value);
    }
}

void FusionEngine::fuseTemperature(std::uint64_t nowNs) {
    struct Contributor { SensorState* s; double var; };
    std::vector<Contributor> good;

    for (std::uint8_t id : tempIds_) {
        SensorState* s = find(id);
        if (!s || !s->desc.enabled || !s->haveAccepted || !s->health.usable()) {
            if (s) s->weight = 0.0;
            continue;
        }
        good.push_back({s, s->noiseVariance()});
    }

    snap_.tempContributors = static_cast<unsigned>(good.size());
    snap_.tempSources.clear();

    if (good.empty()) {
        // Every redundant unit is out. Say so rather than publishing a stale
        // estimate as though it were current.
        snap_.tempSources = "none";
        snap_.temperatureSigma = 0.0;
        return;
    }

    for (std::size_t i = 0; i < good.size(); ++i) {
        if (i) snap_.tempSources += "+";
        snap_.tempSources += good[i].s->desc.name;
    }
    if (good.size() < tempIds_.size()) snap_.tempSources += " (fallback)";

    if (mode_ == FusionMode::Kalman) {
        if (tempKalman_->initialised()) {
            snap_.temperatureC = tempKalman_->estimate();
            snap_.temperatureSigma = std::sqrt(std::max(tempKalman_->variance(), 0.0));
        }
        // Display weight: the gain the filter would give each unit now.
        double invSum = 0.0;
        for (auto& c : good) invSum += 1.0 / c.var;
        for (auto& c : good) c.s->weight = (1.0 / c.var) / invSum;
    } else {
        // Inverse-variance (maximum-likelihood) combination of this tick's
        // readings: x = sum(x_i/var_i) / sum(1/var_i).
        double num = 0.0, invSum = 0.0;
        for (auto& c : good) {
            num += c.s->lastAccepted / c.var;
            invSum += 1.0 / c.var;
        }
        snap_.temperatureC = num / invSum;
        snap_.temperatureSigma = std::sqrt(1.0 / invSum);
        for (auto& c : good) c.s->weight = (1.0 / c.var) / invSum;
    }

    snap_.tNs = nowNs;
}

void FusionEngine::rebuildSnapshot(std::uint64_t nowNs) {
    snap_.humidityPct.reset();
    snap_.pressureHpa.reset();
    snap_.windMs.reset();
    snap_.rainMmH.reset();
    snap_.lightLux.reset();
    snap_.dewPointC.reset();
    snap_.heatIndexC.reset();

    for (auto& s : sensors_) {
        if (s.desc.type == SensorType::Temperature) continue;
        if (!s.desc.enabled || !s.haveAccepted || !s.health.usable()) {
            s.weight = 0.0;
            continue;
        }
        s.weight = 1.0;   // no redundancy on these channels
        switch (s.desc.type) {
        case SensorType::Humidity: snap_.humidityPct = s.lastAccepted; break;
        case SensorType::Pressure: snap_.pressureHpa = s.lastAccepted; break;
        case SensorType::Wind:     snap_.windMs = s.lastAccepted; break;
        case SensorType::Rain:     snap_.rainMmH = s.lastAccepted; break;
        case SensorType::Light:    snap_.lightLux = s.lastAccepted; break;
        default: break;
        }
    }

    const bool haveTemp = snap_.tempContributors > 0;
    if (haveTemp && snap_.humidityPct) {
        snap_.dewPointC = dewPointC(snap_.temperatureC, *snap_.humidityPct);
        snap_.heatIndexC = heatIndexC(snap_.temperatureC, *snap_.humidityPct);
    }

    snap_.pressureSlopeHpaPerHour = trend_.slopePerHour();
    snap_.tendency = trend_.tendency();
    snap_.tNs = nowNs;
    snap_.valid = haveTemp || snap_.pressureHpa.has_value();
}

void FusionEngine::tick(std::uint64_t nowNs) {
    std::lock_guard<std::mutex> lk(m_);

    for (auto& s : sensors_)
        if (s.desc.enabled) s.health.onElapsed(nowNs, samplePeriodNs_);

    fuseTemperature(nowNs);
    rebuildSnapshot(nowNs);
}

FusedSnapshot FusionEngine::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return snap_;
}

std::vector<SensorView> FusionEngine::sensorViews() const {
    std::lock_guard<std::mutex> lk(m_);

    std::vector<SensorView> out;
    out.reserve(sensors_.size());
    for (const auto& s : sensors_) {
        SensorView v;
        v.id = s.desc.id;
        v.type = s.desc.type;
        v.name = s.desc.name;
        v.enabled = s.desc.enabled;
        v.haveRaw = s.haveRaw;
        v.lastRaw = s.lastRaw;
        v.haveAccepted = s.haveAccepted;
        v.lastAccepted = s.lastAccepted;
        v.health = s.health.state();
        v.reason = s.health.lastReason();
        v.noiseSigma = std::sqrt(s.noiseVariance());
        v.weight = s.weight;
        v.accepted = s.health.accepted();
        v.rejected = s.health.rejected();
        v.faultMode = s.desc.faultMode;
        v.faultParam = s.desc.faultParam;
        v.lastFlags = s.lastFlags;
        out.push_back(std::move(v));
    }
    return out;
}

void FusionEngine::setMode(FusionMode m) {
    std::lock_guard<std::mutex> lk(m_);
    if (m == mode_) return;
    mode_ = m;
    tempKalman_->reset();   // the two estimators are not interchangeable mid-flight
}

FusionMode FusionEngine::mode() const {
    std::lock_guard<std::mutex> lk(m_);
    return mode_;
}

void FusionEngine::syncDescriptors(const std::vector<SensorDescriptor>& descs) {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& d : descs) {
        SensorState* s = find(d.id);
        if (!s) continue;
        s->desc.enabled = d.enabled;
        s->desc.faultMode = d.faultMode;
        s->desc.faultParam = d.faultParam;
        s->desc.noiseAmp = d.noiseAmp;
    }
}

std::uint64_t FusionEngine::ingested() const {
    std::lock_guard<std::mutex> lk(m_);
    return ingested_;
}

std::uint64_t FusionEngine::rejectedTotal() const {
    std::lock_guard<std::mutex> lk(m_);
    return rejected_;
}

}  // namespace vws
