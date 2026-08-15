// Host fake for the panel half of the pedal_core::hal contract: settable raw
// footswitch state for the debouncer under test. Test-only controls are
// declared by the tests that need them (the systick fake's pattern).
#include <pedal_core/hal.hpp>

namespace pedal_core::hal {

static bool s_fs[2]   = { false, false };
static bool s_led[2]  = { false, false };
static unsigned s_led_writes = 0;
static bool s_ext[2]  = { false, false };   // tip, ring
static bool s_ext_fsw = false;              // last mode handed to ext_input_pins_mode

void footswitch_pins_init() {}
bool fs_pressed(uint8_t idx) { return s_fs[idx & 1u]; }

void panel_led_pins_init() {}
void panel_led(uint8_t idx, bool on) { s_led[idx & 1u] = on; ++s_led_writes; }

static bool s_relay = false;
void bypass_pins_init() {}
void bypass_relay(bool engaged) { s_relay = engaged; }

void ext_input_pins_mode(bool footswitch_mode) { s_ext_fsw = footswitch_mode; }
bool ext_tip_pressed()  { return s_ext[0]; }
bool ext_ring_pressed() { return s_ext[1]; }

// Test-only control.
void fake_panel_reset()
{
    s_fs[0] = s_fs[1] = s_led[0] = s_led[1] = false;
    s_ext[0] = s_ext[1] = s_ext_fsw = false;
    s_relay = false;
    s_led_writes = 0;
}
void fake_set_fs(uint8_t idx, bool pressed)  { s_fs[idx & 1u] = pressed; }
bool fake_led(uint8_t idx)                   { return s_led[idx & 1u]; }
unsigned fake_led_writes()                   { return s_led_writes; }
void fake_set_ext(bool tip, bool ring)       { s_ext[0] = tip; s_ext[1] = ring; }
bool fake_ext_footswitch_mode()              { return s_ext_fsw; }
bool fake_relay()                            { return s_relay; }

}  // namespace pedal_core::hal
