// Host fake for the MIDI clock generator contract in <pedal_core/midi_clock_out.hpp>:
// records what it was told rather than emitting anything.
//
// tempo_controller.cpp drives the generator from tick(), and it is built for every native
// suite by the src filter, so every test binary needs these symbols. The real
// midi_clock_out.cpp is NOT in that filter — test_midi_clock_out compiles it into its own
// TU behind a recording transport stub — and because test/support is linked as a static
// library, that suite defines all of these itself and never pulls this member in. Every
// other suite leaves them undefined and gets the fake.
//
// Test-only controls are declared by the tests that need them, the systick fake's pattern.
#include <pedal_core/midi_clock_out.hpp>

namespace midi_clock_out {

static bool     s_enabled  = false;
static bool     s_running  = false;
static float    s_bpm      = 0.0f;
static uint32_t s_last_poll_ms = 0u;
static unsigned s_polls    = 0u;

void init()
{
    s_enabled = false;
    s_running = false;
    s_bpm     = 0.0f;
    s_last_poll_ms = 0u;
    s_polls   = 0u;
}

void set_enabled(bool on)     { s_enabled = on; }
void set_bpm(float bpm)       { s_bpm = bpm; }
void set_running(bool running) { s_running = running; }

void poll(uint32_t now_ms)
{
    s_last_poll_ms = now_ms;
    ++s_polls;
}

bool     enabled()           { return s_enabled; }
uint32_t interval_ms()       { return 0u; }
uint32_t interval_frac_q16() { return 0u; }

// Test-only controls.
void     fake_clock_reset()        { init(); }
bool     fake_clock_running()      { return s_running; }
float    fake_clock_bpm()          { return s_bpm; }
uint32_t fake_clock_last_poll_ms() { return s_last_poll_ms; }
unsigned fake_clock_polls()        { return s_polls; }

}  // namespace midi_clock_out
