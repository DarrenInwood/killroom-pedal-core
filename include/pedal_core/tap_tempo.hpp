#pragma once
#include <cstdint>

// Tap tempo and MIDI clock synchronisation.
// Tap: call tap() on each footswitch press; BPM is averaged over last N taps.
// MIDI clock: call midi_clock_tick() on every MIDI clock message (24 per beat).
// MIDI clock takes priority over tap when active.
namespace tap_tempo {
    void  init();
    bool  tap();                     // record a tap; true only when it measured a new tempo
                                     // (>=2 taps within the window) — a lone first tap is inert
    void  midi_clock_tick();         // call on MIDI F8 clock message
    void  midi_clock_reset();        // call on MIDI FA/FC (start/stop)
    float get_bpm();                 // returns current working BPM (default 120)
    float get_midi_bpm();            // last MIDI-clock-derived BPM (independent of taps)
    float get_period_ms();           // ms per beat
    float get_period_fractional(uint8_t num, uint8_t denom); // e.g. 3/8 of a beat
    bool  is_midi_synced();          // true only while clock ticks keep arriving
    void  set_bpm(float bpm);  // set BPM directly (clears MIDI sync state)
}
