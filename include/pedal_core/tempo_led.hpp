#pragma once
#include <cstdint>

// Drives the tempo LED with a 25% duty cycle locked to the algorithm's primary rate: lit for
// the first quarter of each beat, dark for the rest, so the flash reads as a clear downbeat.
// Call reset() whenever the algorithm changes; call update() every main-loop tick.
//
// The LED doubles as the pedal's MIDI receive indicator. blip() inverts it for a short
// dwell, so a player wiring a controller up can press a button and see the pedal answer --
// which is the one thing a screenful of channel and filter settings cannot tell them. The
// product decides what earns a blip; the beat resumes underneath as soon as it expires.
namespace tempo_led {
    // How long a blip inverts the LED. Long enough to see as a distinct flash against
    // any tempo -- a beat is 200 ms even at the top of the range -- and short enough
    // to be over before the eye stops calling it one.
    inline constexpr float BLIP_MS = 150.0f;

    void init();
    void reset();
    void update(float dt_ms, uint32_t period_ms);

    // Show the beat's negative for at least BLIP_MS: dark where it would be lit, lit
    // where it would be dark, and simply lit where there is no beat at all. Lighting
    // the LED instead would be invisible during the quarter the beat already has it on.
    //
    // The dwell is spent in whole update() calls, so it ends on the first pass after it
    // runs out. Retriggers, so a stream of messages holds the negative for as long as the
    // traffic lasts rather than flickering through it.
    void blip();
}
