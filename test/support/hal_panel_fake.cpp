// Host fake for the panel half of the pedal_core::hal contract: settable raw
// footswitch state for the debouncer under test. Test-only controls are
// declared by the tests that need them (the systick fake's pattern).
#include <pedal_core/hal.hpp>

namespace pedal_core::hal {

static bool s_fs[2] = { false, false };

void footswitch_pins_init() {}
bool fs_pressed(uint8_t idx) { return s_fs[idx & 1u]; }

// Test-only control.
void fake_panel_reset() { s_fs[0] = s_fs[1] = false; }
void fake_set_fs(uint8_t idx, bool pressed) { s_fs[idx & 1u] = pressed; }

}  // namespace pedal_core::hal
