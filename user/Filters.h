#pragma once
/*
 * Filters.h - the per-reading processing stages.
 *
 * Every stage implements the same interface: it looks at one reading and
 * either rejects it or passes a (possibly adjusted) value downstream. That
 * lets the fusion engine hold a chain of std::unique_ptr<IFilter> without
 * caring whether a given stage throws readings away (MedianOutlierFilter) or
 * smooths them (KalmanFilter1D).
 */
#include <cstdint>
#include <memory>
#include <string>

#include "RingBuffer.h"

namespace vws {

struct FilterInput {
    double        value    = 0.0;   // engineering units
    /// Measurement variance, same units squared. Used by KalmanFilter1D as
    /// its measurement noise; MedianOutlierFilter deliberately ignores it and
    /// works only from its own window, so that no statistic derived from the
    /// filter's own output can feed back into its threshold.
    double        variance = 0.0;
    std::uint64_t tNs      = 0;
    std::uint16_t flags    = 0;     // VWS_F_* as reported by the driver
};

struct FilterResult {
    bool        accepted = true;
    double      value    = 0.0;
    std::string reason;             // why it was rejected; empty when accepted
};

class IFilter {
public:
    virtual ~IFilter() = default;
    virtual const char* name() const noexcept = 0;
    virtual FilterResult apply(const FilterInput& in) = 0;
    virtual void reset() = 0;
};

/**
 * Rejects readings that are physically impossible for the sensor, and
 * readings too far from the rolling median to be credible.
 *
 * Distance is measured with the median absolute deviation rather than the
 * standard deviation: MAD is not itself dragged around by the outlier it is
 * supposed to catch, so one 20-degree spike cannot widen the gate enough to
 * let the next one through.
 *
 * The window is detrended first. A plain median gate assumes the true value is
 * roughly stationary across the window, which is false for a channel moving
 * fast compared with its own noise: daylight swings 0 to 100 klux in a
 * simulated morning, so the median lags and every fresh sample looks like an
 * outlier. The filter therefore fits a robust slope through the window,
 * extrapolates it to the incoming sample, and applies the MAD test to the
 * residual about that line. A flat channel gives slope 0 and behaves as before.
 */
class MedianOutlierFilter final : public IFilter {
public:
    MedianOutlierFilter(double rangeMin, double rangeMax,
                        std::size_t window = 31, double zThreshold = 3.5);

    const char* name() const noexcept override { return "median/MAD"; }
    FilterResult apply(const FilterInput& in) override;
    void reset() override;

    double median() const;
    double mad() const;
    /// Robust slope across the window, in units per sample.
    double slope() const;
    std::size_t windowFill() const noexcept { return window_.size(); }

private:
    static constexpr std::size_t kMinForMad = 8;
    /// 0.6745 = inverse of the normal distribution's MAD, so the modified
    /// z-score is comparable to an ordinary one on clean Gaussian data.
    static constexpr double kMadScale = 0.6745;

    RingBuffer<double> window_;
    double rangeMin_;
    double rangeMax_;
    double z_;
};

/**
 * Scalar Kalman filter: a random-walk state driven by noisy measurements.
 *
 *   predict:  x = x,            P = P + q
 *   update:   K = P / (P + r),  x = x + K*(z - x),  P = (1 - K)*P
 *
 * With one filter and several redundant sensors, calling apply() once per
 * sensor per tick performs a sequential measurement update, and a sensor with
 * a larger reported variance r automatically gets a smaller gain K. That is
 * inverse-variance weighting, arrived at recursively.
 */
class KalmanFilter1D final : public IFilter {
public:
    /// @param processVariance  how fast the true value is allowed to wander,
    ///                         in (engineering units)^2 per update.
    /// The default process variance suits a channel sampled far faster than
    /// it changes - a few milli-units of true movement per sample - which is
    /// the case for every sensor in this bank.
    explicit KalmanFilter1D(double processVariance = 1e-4,
                            double initialVariance = 100.0);

    const char* name() const noexcept override { return "kalman-1d"; }
    FilterResult apply(const FilterInput& in) override;
    void reset() override;

    double estimate() const noexcept { return x_; }
    double variance() const noexcept { return p_; }
    double lastGain() const noexcept { return lastGain_; }
    bool initialised() const noexcept { return init_; }

private:
    double q_;
    double p0_;
    double x_ = 0.0;
    double p_ = 0.0;
    double lastGain_ = 0.0;
    bool   init_ = false;
};

}  // namespace vws
