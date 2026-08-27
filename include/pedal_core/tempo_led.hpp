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

    // `period_ms` is the beat to flash, or 0 where the algorithm has none. `idle_on` is
    // what to show instead when there is no beat -- a state the footswitch beside this LED
    // is holding, on a product whose switches can carry one. The beat outranks it: a tempo
    // is the thing this LED is for, and something a player reads continuously beats
    // something they already know because they are standing on it.
    void update(float dt_ms, uint32_t period_ms, bool idle_on = false);

    // Show the beat's negative for at least BLIP_MS: dark where it would be lit, lit
    // where it would be dark, and simply lit where there is no beat at all. Lighting
    // the LED instead would be invisible during the quarter the beat already has it on.
    //
    // The dwell is spent in whole update() calls, so it ends on the first pass after it
    // runs out. Retriggers, so a stream of messages holds the negative for as long as the
    // traffic lasts rather than flickering through it.
    void blip();
}
