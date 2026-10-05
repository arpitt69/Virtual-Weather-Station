#include "DerivedMetrics.h"

#include <algorithm>
#include <cmath>

namespace vws {

namespace {
constexpr double kMagnusA = 17.62;
constexpr double kMagnusB = 243.12;   // degC
/// Standard meteorological tendency gate, in hPa per hour.
constexpr double kTendencyGate = 0.10;
}  // namespace

const char* tendencyName(Tendency t) noexcept {
    switch (t) {
    case Tendency::Rising:  return "rising";
    case Tendency::Steady:  return "steady";
    case Tendency::Falling: return "falling";
    }
    return "?";
}

const char* tendencyForecast(Tendency t) noexcept {
    switch (t) {
    case Tendency::Rising:  return "clearing / settled";
    case Tendency::Steady:  return "no change expected";
    case Tendency::Falling: return "unsettled, rain possible";
    }
    return "";
}

double dewPointC(double tempC, double relHumidityPct) {
    const double rh = std::clamp(relHumidityPct, 0.5, 100.0);
    const double gamma = std::log(rh / 100.0) + (kMagnusA * tempC) / (kMagnusB + tempC);
    return (kMagnusB * gamma) / (kMagnusA - gamma);
}

double heatIndexC(double tempC, double relHumidityPct) {
    const double t = tempC * 9.0 / 5.0 + 32.0;    // the regression is in degF
    const double rh = std::clamp(relHumidityPct, 0.0, 100.0);

    if (t < 80.0) return tempC;

    double hi = -42.379 + 2.04901523 * t + 10.14333127 * rh
                - 0.22475541 * t * rh - 0.00683783 * t * t
                - 0.05481717 * rh * rh + 0.00122874 * t * t * rh
                + 0.00085282 * t * rh * rh - 0.00000199 * t * t * rh * rh;

    // The two corrections the NWS applies at the edges of the fit.
    if (rh < 13.0 && t >= 80.0 && t <= 112.0)
        hi -= ((13.0 - rh) / 4.0) * std::sqrt((17.0 - std::fabs(t - 95.0)) / 17.0);
    else if (rh > 85.0 && t >= 80.0 && t <= 87.0)
        hi += ((rh - 85.0) / 10.0) * ((87.0 - t) / 5.0);

    return (hi - 32.0) * 5.0 / 9.0;
}

PressureTrend::PressureTrend(double dayScale, std::size_t window)
    : w_(window), dayScale_(dayScale > 0.0 ? dayScale : 1.0) {}

void PressureTrend::reset() { w_.clear(); }

void PressureTrend::add(std::uint64_t tNs, double hPa) {
    // Convert wall-clock nanoseconds into simulated seconds up front, so the
    // regression below is already on the meteorological time axis.
    w_.push(Point{static_cast<double>(tNs) * 1e-9 * dayScale_, hPa});
}

double PressureTrend::slopePerHour() const {
    const std::size_t n = w_.size();
    if (n < 16) return 0.0;

    // Shift to the first timestamp: raw CLOCK_MONOTONIC seconds are large
    // enough that squaring them loses precision in the normal equations.
    const double t0 = w_[0].tSec;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = w_[i].tSec - t0;
        const double y = w_[i].hPa;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }

    const double dn = static_cast<double>(n);
    const double denom = dn * sxx - sx * sx;
    if (std::fabs(denom) < 1e-12) return 0.0;

    const double slopePerSec = (dn * sxy - sx * sy) / denom;
    return slopePerSec * 3600.0;
}

Tendency PressureTrend::tendency() const {
    const double s = slopePerHour();
    if (s > kTendencyGate) return Tendency::Rising;
    if (s < -kTendencyGate) return Tendency::Falling;
    return Tendency::Steady;
}

}  // namespace vws
