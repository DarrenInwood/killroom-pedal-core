#if __has_include("pedal_core_tempo_config.hpp")

#include <pedal_core/tempo_led.hpp>
#include <pedal_core/hal.hpp>

static float s_phase_ms  = 0.0f;
static bool  s_led_on    = false;  // last written state; avoid redundant pin writes

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
    s_led_on   = true;   // force write on first set_led(false)
    set_led(false);
}

void tempo_led::reset()
{
    s_phase_ms = 0.0f;
}

void tempo_led::update(float dt_ms, uint32_t period_ms)
{
    if (period_ms == 0u) {
        set_led(false);
        return;
    }

    s_phase_ms += dt_ms;
    // Wrap the phase into [0, period). dt_ms and period_ms are whole milliseconds,
    // so the phase is always integer-valued and each subtraction is exact — no need
    // for an fmodf call on the per-tick path.
    const float fp = (float)period_ms;
    while (s_phase_ms >= fp)
        s_phase_ms -= fp;

    // 25% duty: lit for the first quarter of each beat, dark for the rest — a short flash
    // that reads as a clear downbeat rather than a square blink.
    set_led(s_phase_ms < (float)period_ms * 0.25f);
}

#endif  // __has_include(pedal_core_tempo_config.hpp)
