#pragma once
/*
 * DerivedMetrics.h - quantities computed from the fused readings rather than
 * measured: dew point, heat index, and the pressure tendency that forecasters
 * actually use to call the next few hours.
 */
#include <cstdint>

#include "RingBuffer.h"

namespace vws {

enum class Tendency { Rising, Steady, Falling };

const char* tendencyName(Tendency t) noexcept;
const char* tendencyForecast(Tendency t) noexcept;

/// Magnus-Tetens dew point. Valid for roughly -45..60 degC.
double dewPointC(double tempC, double relHumidityPct);

/**
 * NWS (Rothfusz) heat index, in degrees Celsius.
 *
 * The regression is only defined for warm, humid air; below about 27 degC it
 * returns the dry-bulb temperature unchanged, which is the convention the
 * National Weather Service uses too.
 */
double heatIndexC(double tempC, double relHumidityPct);

/**
 * Least-squares pressure slope over a sliding window.
 *
 * The driver compresses a day into a few minutes of wall time, so the slope
 * is reported per *simulated* hour: that is the scale on which the standard
 * tendency thresholds mean anything.
 */
class PressureTrend {
public:
    /// @param dayScale simulated seconds per wall-clock second.
    explicit PressureTrend(double dayScale, std::size_t window = 512);

    void add(std::uint64_t tNs, double hPa);
    void reset();

    /// hPa per simulated hour; 0 until there is enough of a baseline.
    double slopePerHour() const;
    Tendency tendency() const;
    std::size_t samples() const noexcept { return w_.size(); }

private:
    struct Point { double tSec; double hPa; };

    RingBuffer<Point> w_;
    double dayScale_;
};

}  // namespace vws
