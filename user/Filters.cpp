#include "Filters.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vws {

namespace {

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    const double hi = v[mid];
    if (v.size() % 2 != 0) return hi;
    // Even count: average the two central order statistics.
    std::nth_element(v.begin(), v.begin() + mid - 1, v.begin() + mid);
    return 0.5 * (hi + v[mid - 1]);
}

/**
 * Robust slope in units per sample: the rise between the medians of the two
 * halves of the window, over the distance between their midpoints. This is
 * Tukey's resistant line.
 *
 * Least squares would be levered around by a single spike, and spikes are
 * exactly what this window holds, since rejected samples are kept so that a
 * real step change can still be learned. Taking one median per half also
 * gives every input the same long baseline, which a median of
 * symmetrically-opposed pair slopes does not.
 */
double robustSlope(const std::vector<double>& w) {
    const std::size_t n = w.size();
    if (n < 4) return 0.0;

    const std::size_t h = n / 2;
    const std::vector<double> lower(w.begin(), w.begin() + static_cast<long>(h));
    const std::vector<double> upper(w.end() - static_cast<long>(h), w.end());

    // Distance between the two halves' midpoints, in samples.
    const double dx = (static_cast<double>(n - h) + static_cast<double>(n - 1)) * 0.5 -
                      (static_cast<double>(h) - 1.0) * 0.5;
    if (dx <= 0.0) return 0.0;

    return (medianOf(upper) - medianOf(lower)) / dx;
}

}  // namespace

MedianOutlierFilter::MedianOutlierFilter(double rangeMin, double rangeMax,
                                         std::size_t window, double zThreshold)
    : window_(window), rangeMin_(rangeMin), rangeMax_(rangeMax), z_(zThreshold) {}

void MedianOutlierFilter::reset() { window_.clear(); }

double MedianOutlierFilter::median() const { return medianOf(window_.snapshot()); }

double MedianOutlierFilter::slope() const { return robustSlope(window_.snapshot()); }

double MedianOutlierFilter::mad() const {
    const std::vector<double> w = window_.snapshot();
    if (w.empty()) return 0.0;

    const double med = medianOf(w);
    const double k = robustSlope(w);
    const double mid = 0.5 * static_cast<double>(w.size() - 1);

    // Residuals about the fitted line, not about the bare median.
    std::vector<double> dev;
    dev.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i)
        dev.push_back(std::fabs(w[i] - (med + k * (static_cast<double>(i) - mid))));
    return medianOf(std::move(dev));
}

FilterResult MedianOutlierFilter::apply(const FilterInput& in) {
    if (!std::isfinite(in.value))
        return {false, in.value, "not finite"};

    if (in.value < rangeMin_ || in.value > rangeMax_)
        return {false, in.value, "out of range"};

    if (window_.size() >= kMinForMad) {
        const std::vector<double> w = window_.snapshot();
        const double n = static_cast<double>(w.size());
        const double med = medianOf(w);
        const double k = robustSlope(w);
        const double mid = 0.5 * (n - 1.0);

        // Where the fitted line says the incoming sample should land. For a
        // linear ramp the median of the window is the value at its midpoint,
        // so extrapolating from there costs one multiply.
        const double predicted = med + k * (n - mid);

        std::vector<double> dev;
        dev.reserve(w.size());
        for (std::size_t i = 0; i < w.size(); ++i)
            dev.push_back(std::fabs(w[i] - (med + k * (static_cast<double>(i) - mid))));
        const double madv = medianOf(std::move(dev));

        // MAD of zero means the window sits on the fitted line exactly: a
        // repeated value, or a noiseless ramp. Gating on it would reject
        // everything, including a sensor on its way back, so that case is
        // left to the stuck-value detector in the health monitor.
        if (madv > 1e-9) {
            /*
             * Widen the gate by the square root of the extrapolation
             * distance: the residual MAD is measured inside the window where
             * the line is anchored, but the test applies one step beyond its
             * trailing edge, and for a channel that wanders rather than ramps
             * the prediction error grows as the square root of that distance.
             *
             * The scale comes from this window's own MAD and nothing else.
             * Deriving it from a statistic computed downstream of the filter
             * would let accepted outliers widen the very gate meant to catch
             * them. A median-based scale cannot be moved that way.
             */
            const double lever = 0.5 * (n + 1.0);
            const double madEff = madv * std::sqrt(lever);

            const double zscore = kMadScale * (in.value - predicted) / madEff;
            if (std::fabs(zscore) > z_) {
                // Remember it anyway: a genuine step change must eventually
                // drag the window across, or the sensor could never recover.
                window_.push(in.value);
                return {false, in.value, "z=" + std::to_string(zscore).substr(0, 5)};
            }
        }
    }

    window_.push(in.value);
    return {true, in.value, {}};
}

KalmanFilter1D::KalmanFilter1D(double processVariance, double initialVariance)
    : q_(processVariance), p0_(initialVariance) {}

void KalmanFilter1D::reset() {
    init_ = false;
    x_ = 0.0;
    p_ = p0_;
    lastGain_ = 0.0;
}

FilterResult KalmanFilter1D::apply(const FilterInput& in) {
    const double r = std::max(in.variance, 1e-9);

    if (!init_) {
        x_ = in.value;
        p_ = r;
        init_ = true;
        lastGain_ = 1.0;
        return {true, x_, {}};
    }

    p_ += q_;                        // predict (identity state transition)
    const double k = p_ / (p_ + r);  // update
    x_ += k * (in.value - x_);
    p_ *= (1.0 - k);
    lastGain_ = k;

    return {true, x_, {}};
}

}  // namespace vws
