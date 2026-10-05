#include "SensorHealth.h"

namespace vws {

const char* healthName(Health h) noexcept {
    switch (h) {
    case Health::Ok:         return "OK";
    case Health::Suspect:    return "SUSPECT";
    case Health::Faulty:     return "FAULTY";
    case Health::Recovering: return "RECOVER";
    }
    return "?";
}

namespace {
unsigned popcount64(std::uint64_t v) noexcept {
    unsigned n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}
}  // namespace

void SensorHealthMonitor::recordOutcome(bool rejected) {
    recent_ = (recent_ << 1) | (rejected ? 1ULL : 0ULL);
    if (recentCount_ < kRecentWindow) ++recentCount_;
}

double SensorHealthMonitor::recentRejectRate() const noexcept {
    if (recentCount_ < p_.rateMinSamples) return 0.0;

    const std::uint64_t mask = recentCount_ >= 64
                                   ? ~0ULL
                                   : ((1ULL << recentCount_) - 1ULL);
    return static_cast<double>(popcount64(recent_ & mask)) /
           static_cast<double>(recentCount_);
}

bool SensorHealthMonitor::applyRateRule() {
    if (recentCount_ < p_.rateMinSamples) return false;

    const double frac = recentRejectRate();
    const std::string why =
        std::to_string(static_cast<int>(frac * 100.0 + 0.5)) +
        "% of recent readings rejected";

    if (frac >= p_.faultyRejectRate) {
        lastReason_ = why;
        // Hold the clean-run counter down, or the sensor climbs back to
        // RECOVERING on its next ten good readings and flaps straight back.
        acceptRun_ = 0;
        enter(Health::Faulty);
        return true;
    }
    if (frac >= p_.suspectRejectRate && state_ == Health::Ok) {
        lastReason_ = why;
        enter(Health::Suspect);
    }
    return false;
}

double SensorHealthMonitor::rejectRate() const noexcept {
    const std::uint64_t total = accepted_ + rejected_;
    return total ? static_cast<double>(rejected_) / static_cast<double>(total) : 0.0;
}

void SensorHealthMonitor::enter(Health s) {
    // Deliberately leaves acceptRun_/rejectRun_ alone: the thresholds measure
    // one uninterrupted run, and onAccepted()/onRejected() already clear the
    // counter belonging to the other outcome.
    state_ = s;
}

void SensorHealthMonitor::reset() {
    state_ = Health::Ok;
    rejectRun_ = acceptRun_ = repeatRun_ = 0;
    haveLast_ = false;
    lastValue_ = 0.0;
    accepted_ = rejected_ = 0;
    recent_ = 0;
    recentCount_ = 0;
    lastSeenNs_ = 0;
    lastReason_.clear();
}

void SensorHealthMonitor::onAccepted(double value, std::uint64_t tNs, bool atLimit) {
    ++accepted_;
    lastSeenNs_ = tNs;
    rejectRun_ = 0;
    ++acceptRun_;
    recordOutcome(false);

    // A wedged ADC keeps returning the identical code, which no range or
    // median test can catch: the value is plausible every time.
    if (atLimit) {
        repeatRun_ = 0;
    } else if (haveLast_ && value == lastValue_) {
        ++repeatRun_;
    } else {
        repeatRun_ = 0;
    }
    haveLast_ = true;
    lastValue_ = value;

    if (applyRateRule())
        return;

    if (repeatRun_ >= p_.stuckAfterRepeats) {
        lastReason_ = "stuck: " + std::to_string(repeatRun_) + " identical readings";
        // Hold the clean-run counter at zero while wedged, so recovery is
        // earned after the value moves again rather than being credited to
        // the readings that proved it stuck.
        acceptRun_ = 0;
        enter(Health::Faulty);
        return;
    }

    switch (state_) {
    case Health::Ok:
        break;
    case Health::Suspect:
        if (acceptRun_ >= p_.recoverAfterAccepts) enter(Health::Ok);
        break;
    case Health::Faulty:
        if (acceptRun_ >= p_.recoverAfterAccepts) enter(Health::Recovering);
        break;
    case Health::Recovering:
        if (acceptRun_ >= p_.confirmAfterAccepts) enter(Health::Ok);
        break;
    }
}

void SensorHealthMonitor::onRejected(std::uint64_t tNs, const std::string& reason) {
    ++rejected_;
    lastSeenNs_ = tNs;
    acceptRun_ = 0;
    ++rejectRun_;
    lastReason_ = reason;
    repeatRun_ = 0;
    haveLast_ = false;
    recordOutcome(true);

    if (rejectRun_ >= p_.faultyAfterRejects) {
        enter(Health::Faulty);
    } else if (rejectRun_ >= p_.suspectAfterRejects && state_ == Health::Ok) {
        enter(Health::Suspect);
    } else if (state_ == Health::Recovering) {
        // One bad reading during probation sends it straight back.
        enter(Health::Faulty);
    }

    applyRateRule();
}

void SensorHealthMonitor::onElapsed(std::uint64_t nowNs, std::uint64_t samplePeriodNs) {
    if (lastSeenNs_ == 0 || samplePeriodNs == 0 || nowNs <= lastSeenNs_) return;

    const double periods =
        static_cast<double>(nowNs - lastSeenNs_) / static_cast<double>(samplePeriodNs);

    if (periods >= p_.deadSamplePeriods) {
        lastReason_ = "silent for " + std::to_string(static_cast<int>(periods)) + " periods";
        enter(Health::Faulty);
    } else if (periods >= p_.staleSamplePeriods && state_ == Health::Ok) {
        lastReason_ = "sample gap";
        enter(Health::Suspect);
    }
}

}  // namespace vws
