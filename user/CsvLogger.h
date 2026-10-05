#pragma once
/*
 * CsvLogger.h - append-only trace of what the station decided and why.
 *
 * Two files rather than one wide table: the fused series is what you plot,
 * the per-sensor series is what you go to when the fused series looks wrong.
 */
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "FusionEngine.h"

namespace vws {

class CsvLogger {
public:
    /// @param prefix  files are <prefix>_fused.csv and <prefix>_sensors.csv
    /// @throws std::runtime_error if either file cannot be opened.
    explicit CsvLogger(const std::string& prefix);
    ~CsvLogger();

    CsvLogger(const CsvLogger&) = delete;
    CsvLogger& operator=(const CsvLogger&) = delete;

    void log(const FusedSnapshot& snap, const std::vector<SensorView>& views);
    void flush();

    std::uint64_t rows() const noexcept { return rows_; }
    const std::string& fusedPath() const noexcept { return fusedPath_; }
    const std::string& sensorPath() const noexcept { return sensorPath_; }

private:
    std::string fusedPath_, sensorPath_;
    std::ofstream fused_, sensors_;
    std::uint64_t rows_ = 0;
};

}  // namespace vws
