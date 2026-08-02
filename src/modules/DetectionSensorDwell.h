#pragma once

#include <stdint.h>

// Pure dwell/confirm helper for DetectionSensorModule.
// pinActive must already account for trigger polarity (active-high vs active-low).
// When minimum_detect_secs == 0, behaves like legacy immediate detection.
inline bool detectionSensorUpdateDwell(bool pinActive, uint32_t minimum_detect_secs, uint32_t nowMs, bool &dwellArmed,
                                       uint32_t &dwellStartedMs)
{
    if (!pinActive) {
        dwellArmed = false;
        return false;
    }
    if (minimum_detect_secs == 0)
        return true;

    if (!dwellArmed) {
        dwellArmed = true;
        dwellStartedMs = nowMs;
        return false;
    }

    // Unsigned subtraction is rollover-safe (same approach as Throttle).
    return (nowMs - dwellStartedMs) >= (minimum_detect_secs * 1000UL);
}
