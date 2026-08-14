// Host fake for the watchdog contract in <pedal_core/hal.hpp>. Counts kicks so
// a test can assert a bounded busy-wait fed the dog.
#include <pedal_core/hal.hpp>

namespace watchdog {

static uint32_t s_kicks = 0;

void     kick() { ++s_kicks; }
uint32_t fake_kick_count() { return s_kicks; }
void     fake_reset() { s_kicks = 0; }

}  // namespace watchdog
