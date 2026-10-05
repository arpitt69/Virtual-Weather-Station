#pragma once
/*
 * Units.h - the fixed-point / floating-point boundary.
 *
 * The driver works exclusively in milli-units because kernel code cannot use
 * the FPU freely. User space is the first place where it is safe to convert
 * to doubles, and this header is the only place that knows the scale factors.
 */
#include <cstdint>
#include <string>

#include "../include/vws_ioctl.h"

namespace vws {

enum class SensorType {
    Temperature = VWS_TYPE_TEMPERATURE,
    Humidity    = VWS_TYPE_HUMIDITY,
    Pressure    = VWS_TYPE_PRESSURE,
    Wind        = VWS_TYPE_WIND,
    Rain        = VWS_TYPE_RAIN,
    Light       = VWS_TYPE_LIGHT,
};

/// Convert a raw milli-unit reading into engineering units.
inline double toEngineering(SensorType t, std::int32_t milli) {
    switch (t) {
    case SensorType::Temperature: return milli / 1000.0;   // degC
    case SensorType::Humidity:    return milli / 1000.0;   // %RH
    case SensorType::Pressure:    return milli / 1000.0;   // hPa
    case SensorType::Wind:        return milli / 1000.0;   // m/s
    case SensorType::Rain:        return milli / 1000.0;   // mm/h
    case SensorType::Light:       return milli / 1000.0;   // lux
    }
    return milli / 1000.0;
}

inline const char* typeName(SensorType t) {
    switch (t) {
    case SensorType::Temperature: return "temperature";
    case SensorType::Humidity:    return "humidity";
    case SensorType::Pressure:    return "pressure";
    case SensorType::Wind:        return "wind";
    case SensorType::Rain:        return "rain";
    case SensorType::Light:       return "light";
    }
    return "unknown";
}

inline const char* unitName(SensorType t) {
    switch (t) {
    case SensorType::Temperature: return "°C";
    case SensorType::Humidity:    return "%RH";
    case SensorType::Pressure:    return "hPa";
    case SensorType::Wind:        return "m/s";
    case SensorType::Rain:        return "mm/h";
    case SensorType::Light:       return "lux";
    }
    return "";
}

inline const char* faultName(std::uint8_t mode) {
    switch (mode) {
    case VWS_FAULT_NONE:    return "none";
    case VWS_FAULT_STUCK:   return "stuck";
    case VWS_FAULT_DRIFT:   return "drift";
    case VWS_FAULT_SPIKE:   return "spike";
    case VWS_FAULT_DROPOUT: return "dropout";
    case VWS_FAULT_NOISE:   return "noise";
    default:                return "?";
    }
}

/// One reading, in engineering units, as the fusion engine sees it.
struct Reading {
    std::uint8_t  sensorId = 0;
    SensorType    type     = SensorType::Temperature;
    double        value    = 0.0;
    std::uint64_t tNs      = 0;
    std::uint16_t flags    = 0;

    bool fromSample(const vws_sample& s) {
        if (s.type >= VWS_TYPE_COUNT) return false;
        sensorId = s.sensor_id;
        type     = static_cast<SensorType>(s.type);
        value    = toEngineering(type, s.value);
        tNs      = s.timestamp_ns;
        flags    = s.flags;
        return true;
    }
};

}  // namespace vws
