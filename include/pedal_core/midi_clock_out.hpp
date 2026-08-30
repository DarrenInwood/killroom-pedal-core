#pragma once
#include "pedal_core_features.hpp"

#if !PEDAL_CORE_HAS_TEMPO
#  error "midi_clock_out.hpp needs PEDAL_CORE_HAS_TEMPO. Set it in pedal_core_features.hpp and supply pedal_core_tempo_config.hpp."
#endif

#include <cstdint>

// MIDI clock generation: 24 pulses per quarter note out of the MIDI jack, so a
// tempo tapped on this pedal reaches the delay below it in the chain.
//
// The product owns the two facts this cannot know — the tempo, and whether the
// pedal is the master right now. A pedal slaved to an incoming clock passes that
// clock through (midi_handler's clock_thru) rather than generating a second one,
// so set_running(false) while the tempo layer reports it is being driven.
//
// Pacing: poll() emits at most one tick per call and schedules the next in
// fixed-point milliseconds, so the average tempo is exact no matter how the
// caller's cadence wanders -- a run of late ticks never becomes a burst of catch-up
// ones, which a slaved device would read as a tempo spike rather than as jitter.
// Falling a whole interval behind resynchronises instead, losing a tick rather
// than sending two together.
//
// It runs from the caller's loop, not from a timer interrupt: the jack has one
// producer, and that producer asks uart::tx_room() for a whole message before it
// begins one, because a message begun without room for all of it reaches the wire
// truncated. An interrupt writing to the jack would be a second producer racing that
// ring -- taking room already spoken for, mid-message, and truncating the very
// message the check was protecting. The cost is that tick spacing carries whatever
// jitter the caller's loop has; the tempo itself does not drift.
//
// Only the clock byte is generated. Start and Stop belong to a sequencer's
// transport, and a pedal that emitted them would start and stop every looper
// downstream of it whenever a player tapped a new tempo.
namespace midi_clock_out {

    inline constexpr uint8_t TICKS_PER_BEAT = 24u;

    // Forget any schedule and hold silent. Call once at boot.
    void init();

    // The switch. Off is silent whatever the tempo is.
    void set_enabled(bool on);

    // The tempo to generate at. A change takes effect on the next tick, and the
    // schedule keeps its phase, so nudging the tempo does not restart the beat.
    void set_bpm(float bpm);

    // Whether the pedal is the tempo master right now. Going false stops the
    // clock; going true starts a fresh schedule from the next poll().
    void set_running(bool running);

    // Emit at most one tick. Call every loop pass with a millisecond timestamp.
    void poll(uint32_t now_ms);

    bool enabled();

    // The scheduled interval, whole milliseconds and the Q16 fraction under it —
    // exposed for the tests that pin the pacing.
    uint32_t interval_ms();
    uint32_t interval_frac_q16();
}
