#pragma once
#include <cstdint>

// Host-portable exponential moving average used by the ADC driver (adc.cpp).
// Extracted so the fixed-point filter math can be unit-tested off-target; the
// CMSIS-bound adc.cpp keeps only the DMA/register plumbing.
//
// alpha = 1/64, held in a fixed-point accumulator scaled x64 (acc = 64 * filtered):
//   acc <- acc - (acc >> 6) + raw     // steady state: acc = 64 * raw
//   filtered = acc >> 6
// The heavy smoothing keeps resting converter noise well inside the product's knob
// deadband at the cost of a ~64-sample settle — still imperceptible on a knob.
namespace adc_filter {

// Fixed-point shift: alpha = 1 / (1 << SHIFT); the accumulator holds (1 << SHIFT) * filtered.
inline constexpr unsigned SHIFT = 6;

// Accumulator seed for a desired starting filtered output (x(1<<SHIFT) fixed-point scale).
inline constexpr uint32_t seed(uint16_t filtered) { return (uint32_t)filtered << SHIFT; }

// Advance the EMA by one raw sample; returns the updated filtered output.
inline uint16_t step(uint32_t& acc, uint16_t raw) {
    acc = acc - (acc >> SHIFT) + raw;
    return (uint16_t)(acc >> SHIFT);
}

}  // namespace adc_filter
