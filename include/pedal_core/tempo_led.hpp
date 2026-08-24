#pragma once
#include <cstdint>

// Drives the tempo LED with a 25% duty cycle locked to the algorithm's primary rate: lit for
// the first quarter of each beat, dark for the rest, so the flash reads as a clear downbeat.
// Call reset() whenever the algorithm changes; call update() every main-loop tick.
//
// The LED doubles as the pedal's MIDI receive indicator. blip() lights it for a
// short dwell whatever the beat is doing, so a player wiring a controller up can
// press a button and see the pedal answer -- which is the one thing a screenful of
// channel and filter settings cannot tell them. The product decides what earns a
// blip; the beat resumes underneath as soon as the dwell expires.
namespace tempo_led {
    // How long a blip holds the LED on. Long enough to see as a distinct flash,
    // short enough that a stream of messages reads as flicker rather than as a
    // solid light.
    inline constexpr float BLIP_MS = 50.0f;

    void init();
    void reset();
    void update(float dt_ms, uint32_t period_ms);

    // Light the LED for at least BLIP_MS, overriding the beat -- the dwell is spent
    // in whole update() calls, so it ends on the first pass after it runs out.
    // Retriggers.
    void blip();
}
