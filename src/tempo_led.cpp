#include "pedal_core_features.hpp"
// An undefined switch is a preprocessor zero, which would compile this file to nothing
// and lose its symbols somewhere the linker reports as an unrelated failure. Say so here
// instead: the product names every domain, including the ones it does not want.
#if !defined(PEDAL_CORE_HAS_TEMPO)
#  error "pedal_core_features.hpp must define PEDAL_CORE_HAS_TEMPO (0 or 1)."
#endif
#if PEDAL_CORE_HAS_TEMPO

#include <pedal_core/tempo_led.hpp>
#include <pedal_core/hal.hpp>

static float s_phase_ms  = 0.0f;
static bool  s_led_on    = false;  // last written state; avoid redundant pin writes
static float s_blip_ms   = 0.0f;   // remaining MIDI-activity dwell

static void set_led(bool on)
{
    if (on == s_led_on) return;
    s_led_on = on;
    // The LED is the panel's second (index 1); polarity and any no-glitch
    // sequencing live in the product's hal implementation.
    pedal_core::hal::panel_led(1u, on);
}

void tempo_led::init()
{
    pedal_core::hal::panel_led_pins_init();
    s_phase_ms = 0.0f;
    s_blip_ms  = 0.0f;
    s_led_on   = true;   // force write on first set_led(false)
    set_led(false);
}

void tempo_led::reset()
{
    s_phase_ms = 0.0f;
}

void tempo_led::blip()
{
    s_blip_ms = BLIP_MS;
}

void tempo_led::update(float dt_ms, uint32_t period_ms, bool idle_on)
{
    // The beat runs whatever else is happening, so a blip never knocks the bar out of
    // phase: the LED rejoins it where it would have been rather than one dwell late.
    //
    // 25% duty: lit for the first quarter of each beat, dark for the rest — a short flash
    // that reads as a clear downbeat rather than a square blink. The phase wraps into
    // [0, period) by subtraction; dt_ms and period_ms are whole milliseconds, so it is
    // always integer-valued and each subtraction is exact — no fmodf on the per-tick path.
    bool beat_on = idle_on;
    if (period_ms != 0u) {
        s_phase_ms += dt_ms;
        const float fp = (float)period_ms;
        while (s_phase_ms >= fp) s_phase_ms -= fp;
        beat_on = s_phase_ms < fp * 0.25f;
    }

    // A blip is the beat's negative, not a light. Forcing the LED on cannot be seen
    // during the quarter it is already lit, which is exactly when a controller synced to
    // the downbeat sends; inverting reads as a flash from either state. On an algorithm
    // with no beat there is nothing to negate, so it is simply the LED coming on.
    if (s_blip_ms > 0.0f) {
        s_blip_ms -= dt_ms;
        set_led(!beat_on);
        return;
    }

    set_led(beat_on);
}

#endif  // PEDAL_CORE_HAS_TEMPO
