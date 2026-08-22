#pragma once
#include <cstdint>

// Host-portable exponential moving average used by the ADC driver (adc.cpp).
// Extracted so the fixed-point filter math can be unit-tested off-target; the
// CMSIS-bound adc.cpp keeps only the DMA/register plumbing.
//
// alpha = 1 / (1 << S), held in a fixed-point accumulator scaled x(1 << S)
// (acc = (1 << S) * filtered):
//   acc <- acc - (acc >> S) + raw     // steady state: acc = (1 << S) * raw
//   filtered = acc >> S
// Noise rejection and settle time trade against each other along S: the filter divides
// tick-rate noise by sqrt((2 - alpha) / alpha) and settles in about 1/alpha samples. How
// far a product can lean on it depends on how much noise its front end has already
// removed, so S is the caller's to choose.
namespace adc_filter {

// Fixed-point shift: alpha = 1 / (1 << SHIFT); the accumulator holds (1 << SHIFT) * filtered.
// The default suits a driver that feeds the filter single conversions, where the filter is
// the only thing standing between converter noise and the reading.
inline constexpr unsigned SHIFT = 6;

// Accumulator seed for a desired starting filtered output (x(1<<S) fixed-point scale).
template <unsigned S = SHIFT>
inline constexpr uint32_t seed(uint16_t filtered) { return (uint32_t)filtered << S; }

// Advance the EMA by one raw sample; returns the updated filtered output.
template <unsigned S = SHIFT>
inline uint16_t step(uint32_t& acc, uint16_t raw) {
    acc = acc - (acc >> S) + raw;
    return (uint16_t)(acc >> S);
}

}  // namespace adc_filter
