#pragma once
#include <cstdint>
#include "pedal_core_config.hpp"

// Pure ADC-code → parameter-value mapping for knob handling. Header-only and
// CMSIS-free so the firmware's ACTUAL mapping math is unit-testable on the
// host; the product supplies the per-channel calibration bounds.
namespace adc_map {

// 12-bit ADC code (0-4095) → 0-PARAM_MAX, full-range pass-through.
inline uint16_t raw_to_param(uint16_t raw)
{
    return (uint16_t)((uint32_t)raw * PARAM_MAX / 4095u);
}

// 12-bit ADC code → 0-127 through the channel's observed calibration window:
// clamp raw into [lo, hi] then remap linearly to 0-127. If the window is
// degenerate (hi <= lo) fall back to the uncalibrated full-range map so a
// blank/garbage calibration never divides by zero or pins the output.
inline uint16_t calibrated_to_param(uint16_t raw, uint16_t lo, uint16_t hi)
{
    if (hi <= lo) return raw_to_param(raw);
    const uint16_t c = (raw < lo) ? lo : (raw > hi) ? hi : raw;
    return (uint16_t)((uint32_t)(c - lo) * PARAM_MAX / (uint32_t)(hi - lo));
}

}  // namespace adc_map
