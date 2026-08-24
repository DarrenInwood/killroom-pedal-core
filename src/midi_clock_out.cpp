#include <pedal_core/midi_clock_out.hpp>
#include <pedal_core/midi_handler.hpp>
#include "pedal_core_tempo_config.hpp"   // BPM_MIN / BPM_MAX

namespace {

constexpr uint8_t MIDI_CLOCK = 0xF8u;

// 60000 ms per minute / 24 ticks per beat = 2500 ms-per-tick-per-BPM.
constexpr float MS_PER_TICK_NUMERATOR = 60000.0f / (float)midi_clock_out::TICKS_PER_BEAT;

bool     s_enabled  = false;
bool     s_running  = false;
bool     s_armed    = false;   // a schedule exists; false until the first poll

uint32_t s_interval_ms   = 0;  // whole milliseconds between ticks
uint32_t s_interval_frac = 0;  // and the fraction under it, Q16

uint32_t s_next_ms = 0;        // when the next tick is due
uint32_t s_carry   = 0;        // fractional milliseconds owed, Q16

void set_interval_from_bpm(float bpm)
{
    if (bpm < BPM_MIN) bpm = BPM_MIN;
    if (bpm > BPM_MAX) bpm = BPM_MAX;
    const float ms = MS_PER_TICK_NUMERATOR / bpm;
    s_interval_ms   = (uint32_t)ms;
    s_interval_frac = (uint32_t)((ms - (float)s_interval_ms) * 65536.0f);
}

// Move the schedule on by one interval, carrying the fraction so the accumulated
// error stays under a millisecond however long the clock runs.
void advance()
{
    s_carry  += s_interval_frac;
    s_next_ms += s_interval_ms + (s_carry >> 16);
    s_carry  &= 0xFFFFu;
}

void arm(uint32_t now_ms)
{
    s_armed   = true;
    s_carry   = 0;
    s_next_ms = now_ms;
    advance();
}

// Tell the router whether a clock is coming from here, so it can drop the one
// arriving at the input rather than letting both onto the jack.
void publish_generating()
{
    midi_handler::set_generating_clock(s_enabled && s_running);
}

}  // namespace

void midi_clock_out::init()
{
    s_enabled = false;
    s_running = false;
    s_armed   = false;
    set_interval_from_bpm(BPM_DEFAULT);
    publish_generating();
}

void midi_clock_out::set_enabled(bool on)
{
    if (on == s_enabled) return;
    s_enabled = on;
    s_armed   = false;   // a fresh schedule on the next poll
    publish_generating();
}

void midi_clock_out::set_bpm(float bpm)
{
    // The schedule keeps its phase: only the spacing of the ticks after the next
    // one changes, so a tempo nudge does not restart the beat.
    set_interval_from_bpm(bpm);
}

void midi_clock_out::set_running(bool running)
{
    if (running == s_running) return;
    s_running = running;
    s_armed   = false;
    publish_generating();
}

void midi_clock_out::poll(uint32_t now_ms)
{
    if (!s_enabled || !s_running) { s_armed = false; return; }
    if (!s_armed) { arm(now_ms); return; }

    // Wrap-safe: the difference is what matters, not the absolute values.
    if ((int32_t)(now_ms - s_next_ms) < 0) return;

    midi_handler::send_own_realtime(MIDI_CLOCK);
    advance();

    // Still due means the caller was away for longer than a whole interval.
    // Resynchronise rather than firing the backlog: two ticks in one pass reads
    // as a doubled tempo downstream, which is worse than the tick that is lost.
    if ((int32_t)(now_ms - s_next_ms) >= 0) arm(now_ms);
}

bool     midi_clock_out::enabled()           { return s_enabled; }
uint32_t midi_clock_out::interval_ms()       { return s_interval_ms; }
uint32_t midi_clock_out::interval_frac_q16() { return s_interval_frac; }
