#pragma once
#include <cstdint>

// Drives the tempo LED with a 25% duty cycle locked to the algorithm's primary rate: lit for
// the first quarter of each beat, dark for the rest, so the flash reads as a clear downbeat.
// Call reset() whenever the algorithm changes; call update() every main-loop tick.
namespace tempo_led {
    void init();
    void reset();
    void update(float dt_ms, uint32_t period_ms);
}
