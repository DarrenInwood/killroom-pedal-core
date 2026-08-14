// Host fake for the systick contract in <pedal_core/hal.hpp>: a settable
// millisecond clock. delay_ms() advances it, so time-based logic under test
// moves forward instead of spinning.
#include <pedal_core/hal.hpp>

namespace systick {

static uint32_t s_now_ms = 0;

uint32_t now_ms() { return s_now_ms; }
void     delay_ms(uint32_t ms) { s_now_ms += ms; }

// Test-only control, declared by tests that need it.
void fake_set_ms(uint32_t ms) { s_now_ms = ms; }
void fake_advance_ms(uint32_t ms) { s_now_ms += ms; }

}  // namespace systick
