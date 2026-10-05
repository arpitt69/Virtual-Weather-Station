#pragma once
/*
 * Dashboard.h - ncurses front end.
 *
 * RAII around initscr()/endwin(): if the program throws or takes a signal,
 * the destructor still restores the terminal. Rendering is pull-based - the
 * dashboard asks the engine for a snapshot, it is never pushed to - so a slow
 * terminal cannot back up into the fusion thread.
 */
#include <cstdint>
#include <string>
#include <vector>

#include "FusionEngine.h"
#include "RingBuffer.h"

namespace vws {

/// What the dashboard sends back to the main loop when the user presses a key.
struct UiCommand {
    enum class Kind {
        None, Quit, ToggleFusion, ToggleLogging, ClearFaults,
        InjectFault, ToggleEnable, RateUp, RateDown
    } kind = Kind::None;

    std::uint8_t sensorId = 0;
    std::uint8_t faultMode = VWS_FAULT_NONE;
    std::int32_t faultParam = 0;
};

struct RuntimeInfo {
    std::uint32_t sampleRateHz = 0;
    std::uint32_t daySeconds = 0;
    const char*   fusionMode = "";
    bool          logging = false;
    std::string   logPath;
    std::uint64_t queueDepth = 0;
    std::uint64_t queueDropped = 0;
    std::uint64_t readerSamples = 0;
    std::uint64_t resyncs = 0;
    std::uint64_t engineIngested = 0;
    std::uint64_t engineRejected = 0;
};

class Dashboard {
public:
    Dashboard();
    ~Dashboard();

    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    void render(const FusedSnapshot& snap,
                const std::vector<SensorView>& views,
                const vws_stats& kstats,
                const RuntimeInfo& rt);

    /// Non-blocking; returns Kind::None when nothing was pressed.
    UiCommand poll();

    bool usable() const noexcept { return ok_; }

private:
    void drawSensorTable(int& row, const std::vector<SensorView>& views);
    void drawFused(int& row, const FusedSnapshot& snap);
    void drawSpark(int& row);
    void drawFooter(int row, const vws_stats& kstats, const RuntimeInfo& rt);
    void rule(int row, const char* title = nullptr);

    bool ok_ = false;
    int  selected_ = 0;
    int  nSensors_ = 0;
    RingBuffer<double> tempHistory_{240};
    bool haveHistory_ = false;
    std::string hrule_;            ///< one rendered rule, rebuilt on resize
};

}  // namespace vws
