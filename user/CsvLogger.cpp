#include "CsvLogger.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace vws {

namespace {

/// Empty cell for an absent optional, so gaps stay visible in a plot.
std::string cell(const std::optional<double>& v, int prec = 3) {
    if (!v) return {};
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << *v;
    return os.str();
}

std::string num(double v, int prec = 3) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

}  // namespace

CsvLogger::CsvLogger(const std::string& prefix)
    : fusedPath_(prefix + "_fused.csv"), sensorPath_(prefix + "_sensors.csv") {
    fused_.open(fusedPath_, std::ios::out | std::ios::trunc);
    sensors_.open(sensorPath_, std::ios::out | std::ios::trunc);
    if (!fused_ || !sensors_)
        throw std::runtime_error("cannot open CSV output with prefix '" + prefix + "'");

    fused_ << "t_ns,temp_c,temp_sigma,contributors,sources,humidity_pct,"
              "pressure_hpa,wind_ms,rain_mmh,light_lux,dewpoint_c,heatindex_c,"
              "pressure_slope_hpa_per_h,tendency\n";
    sensors_ << "t_ns,id,name,type,raw,accepted,health,weight,noise_sigma,"
                "n_accepted,n_rejected,fault_mode,fault_param,flags,reason\n";
}

CsvLogger::~CsvLogger() { flush(); }

void CsvLogger::log(const FusedSnapshot& snap, const std::vector<SensorView>& views) {
    if (!snap.valid) return;

    fused_ << snap.tNs << ','
           << (snap.tempContributors ? num(snap.temperatureC) : std::string{}) << ','
           << (snap.tempContributors ? num(snap.temperatureSigma, 4) : std::string{}) << ','
           << snap.tempContributors << ','
           << snap.tempSources << ','
           << cell(snap.humidityPct) << ','
           << cell(snap.pressureHpa) << ','
           << cell(snap.windMs) << ','
           << cell(snap.rainMmH) << ','
           << cell(snap.lightLux, 0) << ','
           << cell(snap.dewPointC) << ','
           << cell(snap.heatIndexC) << ','
           << num(snap.pressureSlopeHpaPerHour, 4) << ','
           << tendencyName(snap.tendency) << '\n';

    for (const SensorView& v : views) {
        sensors_ << snap.tNs << ','
                 << static_cast<unsigned>(v.id) << ','
                 << v.name << ','
                 << typeName(v.type) << ','
                 << (v.haveRaw ? num(v.lastRaw) : std::string{}) << ','
                 << (v.haveAccepted ? num(v.lastAccepted) : std::string{}) << ','
                 << healthName(v.health) << ','
                 << num(v.weight, 3) << ','
                 << num(v.noiseSigma, 4) << ','
                 << v.accepted << ','
                 << v.rejected << ','
                 << faultName(v.faultMode) << ','
                 << v.faultParam << ','
                 << v.lastFlags << ','
                 << '"' << v.reason << '"' << '\n';
    }

    ++rows_;
    if (rows_ % 50 == 0) flush();
}

void CsvLogger::flush() {
    fused_.flush();
    sensors_.flush();
}

}  // namespace vws
