#include "Dashboard.h"

#include <ncurses.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>

namespace vws {

namespace {

enum : short {
    kPairHeader = 1, kPairOk, kPairSuspect, kPairFaulty, kPairRecovering,
    kPairDim, kPairValue, kPairRule
};

short healthPair(Health h) {
    switch (h) {
    case Health::Ok:         return kPairOk;
    case Health::Suspect:    return kPairSuspect;
    case Health::Faulty:     return kPairFaulty;
    case Health::Recovering: return kPairRecovering;
    }
    return kPairDim;
}

const char* kSpark[] = {"▁", "▂", "▃", "▄",
                        "▅", "▆", "▇", "█"};

/// Short unit label, narrow enough for a fixed-width column.
const char* shortUnit(SensorType t) {
    switch (t) {
    case SensorType::Temperature: return "°C";
    case SensorType::Humidity:    return "%";
    case SensorType::Pressure:    return "hPa";
    case SensorType::Wind:        return "m/s";
    case SensorType::Rain:        return "mm/h";
    case SensorType::Light:       return "lux";
    }
    return "";
}

int decimalsFor(SensorType t) {
    return t == SensorType::Light ? 0 : (t == SensorType::Pressure ? 2 : 3);
}

}  // namespace

Dashboard::Dashboard() {
    std::setlocale(LC_ALL, "");

    if (!initscr()) return;
    ok_ = true;

    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(kPairHeader,     COLOR_BLACK,  COLOR_CYAN);
        init_pair(kPairOk,         COLOR_GREEN,  -1);
        init_pair(kPairSuspect,    COLOR_YELLOW, -1);
        init_pair(kPairFaulty,     COLOR_RED,    -1);
        init_pair(kPairRecovering, COLOR_CYAN,   -1);
        init_pair(kPairDim,        COLOR_WHITE,  -1);
        init_pair(kPairValue,      COLOR_WHITE,  -1);
        init_pair(kPairRule,       COLOR_BLUE,   -1);
    }
}

Dashboard::~Dashboard() {
    if (ok_) endwin();
}

void Dashboard::rule(int row, const char* title) {
    // Drawn as U+2500 rather than via mvhline()'s ACS_HLINE. ncurses emits
    // ACS as a legacy VT100 charset shift, which any terminal that does not
    // follow the shift renders as a row of 'q' - and the rest of this UI
    // (sparkline blocks, degree signs) is already literal UTF-8, so there is
    // no reason for the rules alone to go through terminfo.
    if (static_cast<int>(hrule_.size()) != COLS) {
        hrule_.clear();
        hrule_.reserve(static_cast<std::size_t>(COLS) * 3);
        for (int x = 0; x < COLS; ++x) hrule_ += "\u2500";
    }

    attron(COLOR_PAIR(kPairRule));
    mvaddstr(row, 0, hrule_.c_str());
    attroff(COLOR_PAIR(kPairRule));
    if (title) {
        attron(A_BOLD);
        mvprintw(row, 2, " %s ", title);
        attroff(A_BOLD);
    }
}

void Dashboard::drawSensorTable(int& row, const std::vector<SensorView>& views) {
    nSensors_ = static_cast<int>(views.size());

    // Field origins are shared with the row loop below so the two cannot
    // drift apart; the health and fault columns are left-aligned because
    // their values are.
    attron(A_BOLD | A_UNDERLINE);
    mvprintw(row, 2, "%-3s %-10s %-12s %9s %-4s", "id", "name", "type", "raw", "");
    mvprintw(row, 44, "%13s", "accepted");
    mvprintw(row, 59, "%-8s", "health");
    mvprintw(row, 68, "%5s %8s %5s ", "w", "sigma", "rej%");
    mvprintw(row, 89, "%-12s", "fault");
    attroff(A_BOLD | A_UNDERLINE);
    ++row;

    for (int i = 0; i < nSensors_; ++i) {
        const SensorView& v = views[static_cast<std::size_t>(i)];
        const int prec = decimalsFor(v.type);
        char raw[32] = "      --", acc[32] = "      --";

        if (v.haveRaw)
            std::snprintf(raw, sizeof(raw), "%.*f", prec, v.lastRaw);
        if (v.haveAccepted)
            std::snprintf(acc, sizeof(acc), "%.*f", prec, v.lastAccepted);

        const double rejPct =
            (v.accepted + v.rejected)
                ? 100.0 * static_cast<double>(v.rejected) /
                      static_cast<double>(v.accepted + v.rejected)
                : 0.0;

        if (i == selected_) attron(A_REVERSE);
        mvprintw(row, 0, "%s", i == selected_ ? ">" : " ");
        mvprintw(row, 2, "%-3u %-10s %-12s %9s %-4s",
                 static_cast<unsigned>(v.id), v.name.c_str(), typeName(v.type),
                 raw, shortUnit(v.type));
        mvprintw(row, 44, "%13s", acc);

        attron(COLOR_PAIR(healthPair(v.health)) | A_BOLD);
        mvprintw(row, 59, "%-8s", v.enabled ? healthName(v.health) : "OFF");
        attroff(COLOR_PAIR(healthPair(v.health)) | A_BOLD);

        mvprintw(row, 68, "%5.2f %8.4f %5.1f ", v.weight, v.noiseSigma, rejPct);
        if (v.faultMode != VWS_FAULT_NONE) {
            attron(COLOR_PAIR(kPairFaulty));
            mvprintw(row, 89, "%s:%d", faultName(v.faultMode), v.faultParam);
            attroff(COLOR_PAIR(kPairFaulty));
        } else {
            mvprintw(row, 89, "%-12s", "-");
        }
        if (i == selected_) attroff(A_REVERSE);
        ++row;
    }

    // Why the selected sensor is in the state it is in.
    if (selected_ < nSensors_) {
        const SensorView& v = views[static_cast<std::size_t>(selected_)];
        if (!v.reason.empty()) {
            attron(COLOR_PAIR(kPairSuspect));
            mvprintw(row, 4, "last rejection: %s", v.reason.c_str());
            attroff(COLOR_PAIR(kPairSuspect));
        }
        ++row;
    }
}

void Dashboard::drawFused(int& row, const FusedSnapshot& snap) {
    const int col2 = COLS / 2 + 2;
    const int top = row;

    if (snap.tempContributors) {
        attron(A_BOLD);
        mvprintw(row, 4, "temperature   %8.2f +/- %.3f %s", snap.temperatureC,
                 snap.temperatureSigma, "°C");
        attroff(A_BOLD);
        tempHistory_.push(snap.temperatureC);
        haveHistory_ = true;
    } else {
        attron(COLOR_PAIR(kPairFaulty) | A_BOLD);
        mvprintw(row, 4, "temperature   NO USABLE SENSOR");
        attroff(COLOR_PAIR(kPairFaulty) | A_BOLD);
    }
    ++row;
    mvprintw(row++, 4, "sources       %s (%u of 2)", snap.tempSources.c_str(),
             snap.tempContributors);

    if (snap.humidityPct) mvprintw(row, 4, "humidity      %8.2f %%RH", *snap.humidityPct);
    else                  mvprintw(row, 4, "humidity           --");
    ++row;
    if (snap.pressureHpa) mvprintw(row, 4, "pressure      %8.2f hPa", *snap.pressureHpa);
    else                  mvprintw(row, 4, "pressure           --");
    ++row;
    if (snap.windMs)      mvprintw(row, 4, "wind          %8.2f m/s", *snap.windMs);
    else                  mvprintw(row, 4, "wind               --");
    ++row;
    if (snap.rainMmH)     mvprintw(row, 4, "rain          %8.2f mm/h", *snap.rainMmH);
    else                  mvprintw(row, 4, "rain               --");
    ++row;
    if (snap.lightLux)    mvprintw(row, 4, "light         %8.0f lux", *snap.lightLux);
    else                  mvprintw(row, 4, "light              --");
    ++row;

    int r = top;
    attron(A_BOLD);
    mvprintw(r++, col2, "DERIVED");
    attroff(A_BOLD);
    if (snap.dewPointC)  mvprintw(r, col2, "dew point     %8.2f %s", *snap.dewPointC, "°C");
    else                 mvprintw(r, col2, "dew point          --");
    ++r;
    if (snap.heatIndexC) mvprintw(r, col2, "heat index    %8.2f %s", *snap.heatIndexC, "°C");
    else                 mvprintw(r, col2, "heat index         --");
    ++r;
    mvprintw(r++, col2, "tendency      %8s  (%+.3f hPa/sim-h)",
             tendencyName(snap.tendency), snap.pressureSlopeHpaPerHour);
    mvprintw(r++, col2, "outlook       %s", tendencyForecast(snap.tendency));

    row = std::max(row, r);
}

void Dashboard::drawSpark(int& row) {
    if (!haveHistory_ || tempHistory_.size() < 2) { ++row; return; }

    const std::vector<double> h = tempHistory_.snapshot();
    const auto [lo, hi] = std::minmax_element(h.begin(), h.end());
    const double span = std::max(*hi - *lo, 1e-6);

    // Reserve room for the trailing label up front. Sizing the bars to the
    // full width instead just pushed the label off the right edge, where
    // ncurses clipped it mid-word.
    constexpr int kBarCol = 12;
    constexpr int kLabelRoom = 30;
    const int avail = COLS - kBarCol - kLabelRoom;
    if (avail < 4) { ++row; return; }

    const int width = std::min(avail, static_cast<int>(h.size()));
    const std::size_t start = h.size() - static_cast<std::size_t>(width);

    mvprintw(row, 4, "%6.2f ", *lo);
    for (int i = 0; i < width; ++i) {
        const double norm = (h[start + static_cast<std::size_t>(i)] - *lo) / span;
        const int level = std::clamp(static_cast<int>(norm * 7.999), 0, 7);
        attron(COLOR_PAIR(kPairOk));
        mvaddstr(row, kBarCol + i, kSpark[level]);
        attroff(COLOR_PAIR(kPairOk));
    }
    mvprintw(row, kBarCol + width + 1, " %.2f  fused temp, last %d", *hi, width);
    ++row;
}

void Dashboard::drawFooter(int row, const vws_stats& kstats, const RuntimeInfo& rt) {
    attron(COLOR_PAIR(kPairDim));
    mvprintw(row, 2,
             "kernel  ticks %llu  gen %llu  read %llu  dropped %llu  overflow %llu  "
             "fifo %u  dropouts %llu",
             static_cast<unsigned long long>(kstats.timer_ticks),
             static_cast<unsigned long long>(kstats.samples_generated),
             static_cast<unsigned long long>(kstats.samples_read),
             static_cast<unsigned long long>(kstats.samples_dropped),
             static_cast<unsigned long long>(kstats.fifo_overflows),
             kstats.fifo_used,
             static_cast<unsigned long long>(kstats.dropouts_injected));
    mvprintw(row + 1, 2,
             "user    queue %llu (dropped %llu)  ingested %llu  rejected %llu  "
             "resyncs %llu  log %s",
             static_cast<unsigned long long>(rt.queueDepth),
             static_cast<unsigned long long>(rt.queueDropped),
             static_cast<unsigned long long>(rt.engineIngested),
             static_cast<unsigned long long>(rt.engineRejected),
             static_cast<unsigned long long>(rt.resyncs),
             rt.logging ? rt.logPath.c_str() : "off");
    attroff(COLOR_PAIR(kPairDim));

    attron(A_BOLD);
    mvprintw(row + 2, 2,
             "up/down select  s stuck  d drift  k spike  o dropout  n noise  "
             "e enable  c clear  f fusion  l log  +/- rate  q quit");
    attroff(A_BOLD);
}

void Dashboard::render(const FusedSnapshot& snap,
                       const std::vector<SensorView>& views,
                       const vws_stats& kstats,
                       const RuntimeInfo& rt) {
    if (!ok_) return;

    erase();

    attron(COLOR_PAIR(kPairHeader) | A_BOLD);
    mvhline(0, 0, ' ', COLS);
    mvprintw(0, 2, " Virtual Weather Station  Multi-Sensor Fusion ");
    mvprintw(0, std::max(34, COLS - 54),
             "%4u Hz   day=%us   fusion: %-16s %s",
             rt.sampleRateHz, rt.daySeconds, rt.fusionMode,
             snap.valid ? "live" : "warming up");
    attroff(COLOR_PAIR(kPairHeader) | A_BOLD);

    int row = 1;
    rule(row++, "SENSOR BANK");
    drawSensorTable(row, views);
    rule(row++, "FUSED");
    drawFused(row, snap);
    rule(row++, "HISTORY");
    drawSpark(row);
    rule(row++);
    drawFooter(row, kstats, rt);

    refresh();
}

UiCommand Dashboard::poll() {
    UiCommand cmd;
    if (!ok_) return cmd;

    const int ch = getch();
    if (ch == ERR) return cmd;

    const auto sel = static_cast<std::uint8_t>(std::max(0, selected_));

    switch (ch) {
    case 'q': case 'Q':
        cmd.kind = UiCommand::Kind::Quit;
        break;
    case KEY_UP:
        selected_ = std::max(0, selected_ - 1);
        break;
    case KEY_DOWN:
        selected_ = std::min(std::max(0, nSensors_ - 1), selected_ + 1);
        break;
    case 'f': case 'F':
        cmd.kind = UiCommand::Kind::ToggleFusion;
        break;
    case 'l': case 'L':
        cmd.kind = UiCommand::Kind::ToggleLogging;
        break;
    case 'c': case 'C':
        cmd.kind = UiCommand::Kind::ClearFaults;
        break;
    case 'e': case 'E':
        cmd.kind = UiCommand::Kind::ToggleEnable;
        cmd.sensorId = sel;
        break;
    case '+': case '=':
        cmd.kind = UiCommand::Kind::RateUp;
        break;
    case '-': case '_':
        cmd.kind = UiCommand::Kind::RateDown;
        break;
    case 's': case 'S':
        cmd.kind = UiCommand::Kind::InjectFault;
        cmd.sensorId = sel;
        cmd.faultMode = VWS_FAULT_STUCK;
        break;
    case 'd': case 'D':
        cmd.kind = UiCommand::Kind::InjectFault;
        cmd.sensorId = sel;
        cmd.faultMode = VWS_FAULT_DRIFT;
        cmd.faultParam = 40;        // +40 milli-units per sample
        break;
    case 'k': case 'K':
        cmd.kind = UiCommand::Kind::InjectFault;
        cmd.sensorId = sel;
        cmd.faultMode = VWS_FAULT_SPIKE;
        cmd.faultParam = 15;        // 15% of samples
        break;
    case 'o': case 'O':
        cmd.kind = UiCommand::Kind::InjectFault;
        cmd.sensorId = sel;
        cmd.faultMode = VWS_FAULT_DROPOUT;
        cmd.faultParam = 90;        // 90% of samples never produced
        break;
    case 'n': case 'N':
        cmd.kind = UiCommand::Kind::InjectFault;
        cmd.sensorId = sel;
        cmd.faultMode = VWS_FAULT_NOISE;
        cmd.faultParam = 6000;      // +/-6 units of noise
        break;
    default:
        break;
    }
    return cmd;
}

}  // namespace vws
