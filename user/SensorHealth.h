#pragma once
/*
 * SensorHealth.h - per-sensor health state machine.
 *
 * States are OK, SUSPECT, FAULTY and RECOVERING. Rejection runs promote a
 * sensor towards FAULTY, clean runs demote it back through RECOVERING to OK.
 *
 * A sensor also goes FAULTY on a stuck register (the same value repeated), on
 * silence for longer than a few sample periods (which is how an injected
 * dropout shows up: there is nothing to reject, only nothing to read), and on
 * a sustained *rate* of rejection.
 *
 * The rate rule covers a blind spot in the run-based ones: a sensor failing a
 * large fraction of its readings, but never many in a row, never accumulates a
 * long enough run to be condemned. Naming the unit that needs replacing is
 * half the point of a health monitor.
 *
 * Every threshold counts an uninterrupted *run*: one accepted reading clears
 * the rejection counter and vice versa, and a state transition does not clear
 * either. So confirmAfterAccepts is the total length of the clean run needed
 * to confirm a recovery, not a further count on top of recoverAfterAccepts.
 *
 * Only FAULTY is excluded from fusion. SUSPECT keeps contributing: it means
 * "watch this one", and the variance estimate already shrinks the weight of a
 * sensor that has started misbehaving.
 */
#include <cstdint>
#include <string>

namespace vws {

enum class Health { Ok, Suspect, Faulty, Recovering };

const char* healthName(Health h) noexcept;

struct HealthPolicy {
    unsigned suspectAfterRejects   = 3;
    unsigned faultyAfterRejects    = 8;
    unsigned stuckAfterRepeats     = 12;
    unsigned recoverAfterAccepts   = 10;   // FAULTY -> RECOVERING
    unsigned confirmAfterAccepts   = 25;   // RECOVERING -> OK
    double   staleSamplePeriods    = 6.0;  // SUSPECT after this much silence
    double   deadSamplePeriods     = 20.0; // FAULTY after this much silence

    /* Sustained rejection rate, over the last kRecentWindow outcomes. A
     * healthy sensor in this bank rejects well under 1%, so these leave a
     * wide margin. */
    double   suspectRejectRate     = 0.10;
    double   faultyRejectRate      = 0.25;
    unsigned rateMinSamples        = 32;
};

class SensorHealthMonitor {
public:
    explicit SensorHealthMonitor(HealthPolicy policy = {}) : p_(policy) {}

    /**
     * @param atLimit the reading sits at an end of the sensor's range (or the
     *        driver flagged it saturated). A sensor pinned at a limit repeats
     *        its value legitimately - zero rainfall, zero lux at night - so
     *        those repeats must not be mistaken for a wedged converter.
     */
    void onAccepted(double value, std::uint64_t tNs, bool atLimit = false);
    void onRejected(std::uint64_t tNs, const std::string& reason);

    /// Silence check; @p samplePeriodNs is the expected gap between readings.
    void onElapsed(std::uint64_t nowNs, std::uint64_t samplePeriodNs);

    void recordOutcome(bool rejected);

    Health state() const noexcept { return state_; }
    bool usable() const noexcept { return state_ != Health::Faulty; }

    std::uint64_t accepted() const noexcept { return accepted_; }
    std::uint64_t rejected() const noexcept { return rejected_; }
    /// Lifetime rejected / total.
    double rejectRate() const noexcept;
    /// Rejected fraction over the last kRecentWindow outcomes, or 0 until
    /// there are rateMinSamples of them.
    double recentRejectRate() const noexcept;
    unsigned repeatRun() const noexcept { return repeatRun_; }
    const std::string& lastReason() const noexcept { return lastReason_; }
    std::uint64_t lastSeenNs() const noexcept { return lastSeenNs_; }

    void reset();

private:
    static constexpr unsigned kRecentWindow = 64;

    void enter(Health s);
    /// Escalate on a sustained reject rate. Never de-escalates: recovery is
    /// the run-based rules' job.
    bool applyRateRule();

    HealthPolicy p_;
    Health state_ = Health::Ok;

    unsigned rejectRun_ = 0;
    unsigned acceptRun_ = 0;
    unsigned repeatRun_ = 0;
    bool     haveLast_ = false;
    double   lastValue_ = 0.0;

    /* Outcomes of the last kRecentWindow readings, newest in bit 0;
     * 1 = rejected. A shift register is enough and costs one word. */
    std::uint64_t recent_ = 0;
    unsigned      recentCount_ = 0;

    std::uint64_t accepted_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t lastSeenNs_ = 0;
    std::string   lastReason_;
};

}  // namespace vws
