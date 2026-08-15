#pragma once
#include <cstdint>
#include "pedal_core_config.hpp"   // PARAM_MAX

// Host-portable expression-pedal interpolation for products with an expression input.
// Maps a pedal position linearly onto [min_val, max_val]. Handles min > max (reversed
// pedal): the slope goes negative so the output descends from min to max as the pedal
// rises.
namespace expr_map {

inline uint16_t interp(uint16_t min_val, uint16_t max_val, uint16_t pedal) {
    // int32 throughout: the product reaches PARAM_MAX squared, which leaves int16
    // as soon as the parameter scale passes 8 bits.
    const int32_t span = (int32_t)max_val - (int32_t)min_val;
    const int32_t num  = (int32_t)pedal * span;
    const int32_t half = (int32_t)PARAM_MAX / 2;
    // Round to nearest, away from zero, so a reversed pedal is as accurate as a forward
    // one and both endpoints land exactly. Truncation would bias a forward sweep low and
    // a reversed one high, by up to a step at every position.
    const int32_t val = (int32_t)min_val
                      + ((num >= 0) ? (num + half) : (num - half)) / (int32_t)PARAM_MAX;
    return (uint16_t)val;
}

}  // namespace expr_map
