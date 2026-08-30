// Compiled only where the product asks for the tempo layer; elsewhere this TU is empty
// and costs nothing.
#include "pedal_core_features.hpp"
// An undefined switch is a preprocessor zero, which would compile this file to nothing
// and lose its symbols somewhere the linker reports as an unrelated failure. Say so here
// instead: the product names every domain, including the ones it does not want.
#if !defined(PEDAL_CORE_HAS_TEMPO)
#  error "pedal_core_features.hpp must define PEDAL_CORE_HAS_TEMPO (0 or 1)."
#endif
#if PEDAL_CORE_HAS_TEMPO

#include <pedal_core/tap_tempo.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_tempo_config.hpp"   // BPM_* / TAP_TEMPO_* / MIDI_SYNC_TIMEOUT_MS

static float    s_bpm           = BPM_DEFAULT;
static uint32_t s_tap_times[TAP_TEMPO_AVERAGE_TAPS] = {};
static uint8_t  s_tap_count     = 0;
static uint32_t s_last_tap_ms   = 0;
static bool     s_midi_synced   = false;

// MIDI clock: 24 ticks per beat
static uint32_t s_clock_tick_ms[24] = {};
static uint8_t  s_clock_tick_idx    = 0;
static uint8_t  s_clock_tick_count  = 0;
// MIDI-sync tempo kept separately from the working BPM so the app can seed a
// just-loaded preset from the live clock even if a tap has changed s_bpm since.
static float    s_midi_bpm          = BPM_DEFAULT;
static uint32_t s_last_clock_ms     = 0;

static void update_bpm(float new_bpm)
{
    if (new_bpm < BPM_MIN) new_bpm = BPM_MIN;
    if (new_bpm > BPM_MAX) new_bpm = BPM_MAX;
    s_bpm = new_bpm;
}

void tap_tempo::init()
{
    // Full engine re-init to the boot state: the BPM defaults plus every tap-window
    // and MIDI-clock-ring static, so a call at any time leaves no stale tap timestamps
    // or half-filled clock ring to blend into the next average.
    s_bpm           = BPM_DEFAULT;
    s_midi_bpm      = BPM_DEFAULT;
    s_last_clock_ms = 0;
    s_midi_synced   = false;

    for (uint32_t& t : s_tap_times) t = 0;
    s_tap_count     = 0;
    s_last_tap_ms   = 0;

    for (uint32_t& t : s_clock_tick_ms) t = 0;
    s_clock_tick_idx   = 0;
    s_clock_tick_count = 0;
}

bool tap_tempo::tap()
{
    const uint32_t now = systick::now_ms();

    if (s_last_tap_ms > 0 && (now - s_last_tap_ms) > TAP_TEMPO_MAX_INTERVAL) {
        s_tap_count = 0;  // gap too long – restart
    }

    s_tap_times[s_tap_count % TAP_TEMPO_AVERAGE_TAPS] = now;
    ++s_tap_count;
    // s_tap_count only has to distinguish "window not yet full" (< TAPS) from full and
    // supply the rotating write/oldest index mod TAPS. Fold it back by a whole window once
    // it is safely past full so the uint8_t never wraps at 255 mid-window — a wrap would
    // momentarily drop the count below 2 and stall the average for two taps. Subtracting a
    // whole window preserves both s_tap_count % TAPS and the ">= TAPS" full test.
    if (s_tap_count >= 2u * TAP_TEMPO_AVERAGE_TAPS)
        s_tap_count -= TAP_TEMPO_AVERAGE_TAPS;
    s_last_tap_ms = now;

    bool measured = false;
    if (s_tap_count >= 2) {
        const uint8_t valid = (s_tap_count < TAP_TEMPO_AVERAGE_TAPS)
                              ? s_tap_count
                              : TAP_TEMPO_AVERAGE_TAPS;
        // Average interval over the last `valid` taps
        const uint8_t oldest_idx = (s_tap_count - valid) % TAP_TEMPO_AVERAGE_TAPS;
        const uint32_t span_ms = now - s_tap_times[oldest_idx];
        // Taps landing in the same systick ms give a zero span; skip the update
        // rather than divide by zero and pin the tempo to BPM_MAX.
        if (span_ms > 0u) {
            const float interval_ms = (float)span_ms / (float)(valid - 1u);
            update_bpm(60000.0f / interval_ms);
            measured = true;
        }
    }

    // A lone first tap only marks the beat — it changes no tempo, so it leaves MIDI sync
    // intact. Only a tap that actually measures a new tempo takes over from MIDI.
    if (measured) s_midi_synced = false;
    return measured;
}

void tap_tempo::midi_clock_tick()
{
    const uint32_t now = systick::now_ms();
    s_last_clock_ms = now;  // arm the inactivity timeout in is_midi_synced()
    s_clock_tick_ms[s_clock_tick_idx] = now;
    s_clock_tick_idx = (s_clock_tick_idx + 1u) % 24u;
    if (s_clock_tick_count < 24u) ++s_clock_tick_count;

    // After one full beat (24 ticks accumulated), calculate BPM.
    // 24 stored timestamps span 23 intervals; one beat = 24 intervals.
    // avg_interval = span/23, beat = avg_interval*24.
    if (s_clock_tick_count >= 24u) {
        const uint8_t oldest = s_clock_tick_idx;  // next write position = oldest stored
        const uint32_t span_ms = now - s_clock_tick_ms[oldest];
        // A full beat of ticks arriving within one systick ms (e.g. a backlog
        // flushed in a single superloop pass) gives a zero span; skip it rather
        // than divide by zero and pin the tempo to BPM_MAX. The ring keeps rolling,
        // so the next tick with real spacing re-derives the tempo and latches sync.
        if (span_ms > 0u) {
            const float avg_tick_ms = (float)span_ms / 23.0f;
            update_bpm(60000.0f / (avg_tick_ms * 24.0f));
            s_midi_bpm    = s_bpm;  // remember the clock tempo independent of later taps
            s_midi_synced = true;
        }
    }
}

void tap_tempo::midi_clock_reset()
{
    s_clock_tick_count = 0;
    s_clock_tick_idx   = 0;
    s_midi_synced      = false;
}

void tap_tempo::set_bpm(float bpm)      { update_bpm(bpm); s_midi_synced = false; s_tap_count = 0; }
float tap_tempo::get_bpm()              { return s_bpm; }
float tap_tempo::get_midi_bpm()         { return s_midi_bpm; }
float tap_tempo::get_period_ms()        { return 60000.0f / s_bpm; }

// "Synced" latches on a full clock beat (see midi_clock_tick) but expires if the
// master stops sending: report false once no tick has arrived for the timeout.
bool  tap_tempo::is_midi_synced()
{
    if (!s_midi_synced) return false;
    return (systick::now_ms() - s_last_clock_ms) < MIDI_SYNC_TIMEOUT_MS;
}

float tap_tempo::get_period_fractional(uint8_t num, uint8_t denom)
{
    if (denom == 0) return 0.0f;
    return get_period_ms() * (float)num / (float)denom;
}

#endif  // PEDAL_CORE_HAS_TEMPO
