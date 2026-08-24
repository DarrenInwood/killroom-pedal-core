// Host-native unit tests for the MIDI clock generator (src/midi_clock_out.cpp).
//
// The generator is what makes a tempo tapped on the pedal reach the delay below
// it in the chain, and the thing that is easy to get wrong is not whether it
// ticks but how it paces: a scheduler that rounds to whole milliseconds drifts,
// and one that fires a backlog after a late poll sends two ticks together, which
// a slaved device reads as a doubled tempo rather than as jitter. Both are pinned
// here. midi_clock_out.cpp is compiled into this TU behind a recording stub for
// the one transport call it makes.

#include <unity.h>
#include <cstdint>
#include <cmath>
#include <vector>

// --- recording stub for the router call under test -------------------------
namespace midi_handler {
    static std::vector<uint8_t> g_sent;
    static bool g_generating = false;
    void send_own_realtime(uint8_t status) { g_sent.push_back(status); }
    void set_generating_clock(bool on) { g_generating = on; }
}

#include "../../src/midi_clock_out.cpp"

// Run the clock from `from_ms` to `to_ms` at one poll per millisecond, the
// cadence the pedal's loop gives it.
static void run_ms(uint32_t from_ms, uint32_t to_ms)
{
    for (uint32_t t = from_ms; t <= to_ms; ++t) midi_clock_out::poll(t);
}

void setUp(void) {
    midi_handler::g_sent.clear();
    midi_clock_out::init();
}
void tearDown(void) {}

void test_silent_until_enabled_and_running(void) {
    run_ms(0, 500);
    TEST_ASSERT_EQUAL_INT(0, (int)midi_handler::g_sent.size());

    midi_clock_out::set_enabled(true);
    run_ms(501, 1000);
    TEST_ASSERT_EQUAL_INT(0, (int)midi_handler::g_sent.size());   // enabled but not master
}

// A pedal slaved to an incoming clock forwards it; generating a second one would
// put two clocks on the wire.
void test_stops_when_it_is_no_longer_the_master(void) {
    midi_clock_out::set_enabled(true);
    midi_clock_out::set_bpm(120.0f);
    midi_clock_out::set_running(true);
    run_ms(0, 1000);
    TEST_ASSERT_TRUE(midi_handler::g_sent.size() > 0);

    midi_clock_out::set_running(false);
    const size_t before = midi_handler::g_sent.size();
    run_ms(1001, 2000);
    TEST_ASSERT_EQUAL_INT((int)before, (int)midi_handler::g_sent.size());
}

void test_sends_the_clock_byte_only(void) {
    midi_clock_out::set_enabled(true);
    midi_clock_out::set_bpm(120.0f);
    midi_clock_out::set_running(true);
    run_ms(0, 1000);
    TEST_ASSERT_TRUE(midi_handler::g_sent.size() > 0);
    for (uint8_t b : midi_handler::g_sent) TEST_ASSERT_EQUAL_UINT8(0xF8, b);
}

// 24 ticks a beat, two beats a second at 120 BPM: 48 ticks per second, and the
// count has to hold over a minute, which is what says the fraction is carried
// rather than rounded away.
void test_tempo_does_not_drift_over_a_minute(void) {
    midi_clock_out::set_enabled(true);
    midi_clock_out::set_bpm(120.0f);
    midi_clock_out::set_running(true);
    run_ms(0, 60000);
    const int ticks = (int)midi_handler::g_sent.size();
    TEST_ASSERT_INT_WITHIN(1, 2880, ticks);   // 120 beats * 24
}

// A tempo that lands nowhere near a whole millisecond is the one that exposes a
// rounding scheduler: 137 BPM is 18.248 ms a tick.
void test_awkward_tempo_does_not_drift(void) {
    midi_clock_out::set_enabled(true);
    midi_clock_out::set_bpm(137.0f);
    midi_clock_out::set_running(true);
    run_ms(0, 60000);
    const int ticks = (int)midi_handler::g_sent.size();
    TEST_ASSERT_INT_WITHIN(2, 137 * 24, ticks);
}

void test_interval_carries_a_fraction(void) {
    midi_clock_out::set_enabled(true);
    midi_clock_out::set_bpm(120.0f);
    // 2500 / 120 = 20.8333 ms
    TEST_ASSERT_EQUAL_UINT32(20u, midi_clock_out::interval_ms());
    const float frac = (float)midi_clock_out::interval_frac_q16() / 65536.0f;
    TEST_ASSERT_TRUE(fabsf(frac - 0.8333f) < 0.001f);
}

// The failure this exists to prevent: a caller that went away for half a second
// must not come back to a burst of the ticks it missed.
void test_a_late_poll_resynchronises_rather_than_bursting(void) {
    midi_clock_out::set_enabled(true);
    midi_clock_out::set_bpm(120.0f);
    midi_clock_out::set_running(true);
    midi_clock_out::poll(0);
    midi_handler::g_sent.clear();

    midi_clock_out::poll(500);            // half a second late: 24 ticks were due
    TEST_ASSERT_EQUAL_INT(1, (int)midi_handler::g_sent.size());

    // And it is scheduled from now, so the next tick is one interval away rather
    // than immediately.
    midi_clock_out::poll(505);
    TEST_ASSERT_EQUAL_INT(1, (int)midi_handler::g_sent.size());
    run_ms(506, 525);
    TEST_ASSERT_EQUAL_INT(2, (int)midi_handler::g_sent.size());
}

// A tempo nudge changes the spacing of the ticks after the next one; it does not
// restart the beat, so a knob sweep does not stutter the clock.
void test_tempo_change_keeps_the_schedule(void) {
    midi_clock_out::set_enabled(true);
    midi_clock_out::set_bpm(120.0f);
    midi_clock_out::set_running(true);
    run_ms(0, 1000);
    const int at_120 = (int)midi_handler::g_sent.size();

    midi_handler::g_sent.clear();
    midi_clock_out::set_bpm(240.0f);
    run_ms(1001, 2000);
    const int at_240 = (int)midi_handler::g_sent.size();
    TEST_ASSERT_TRUE(at_240 > at_120 + 20);   // roughly twice as many
}

// The clamp is the tempo layer's, so a caller cannot schedule a tick every
// fraction of a millisecond by asking for an impossible tempo.
void test_bpm_is_clamped_to_the_family_range(void) {
    midi_clock_out::set_bpm(10000.0f);
    const uint32_t fast = midi_clock_out::interval_ms();
    midi_clock_out::set_bpm(BPM_MAX);
    TEST_ASSERT_EQUAL_UINT32(midi_clock_out::interval_ms(), fast);

    midi_clock_out::set_bpm(0.0f);
    const uint32_t slow = midi_clock_out::interval_ms();
    midi_clock_out::set_bpm(BPM_MIN);
    TEST_ASSERT_EQUAL_UINT32(midi_clock_out::interval_ms(), slow);
}

// The router drops an inbound clock while the pedal is generating one, so the
// generator has to say when that is -- both halves of it, since being enabled
// without being the master produces nothing.
void test_it_tells_the_router_when_it_is_generating(void) {
    TEST_ASSERT_FALSE(midi_handler::g_generating);

    midi_clock_out::set_enabled(true);
    TEST_ASSERT_FALSE(midi_handler::g_generating);   // enabled, but not the master

    midi_clock_out::set_running(true);
    TEST_ASSERT_TRUE(midi_handler::g_generating);

    midi_clock_out::set_running(false);              // slaved to an incoming clock
    TEST_ASSERT_FALSE(midi_handler::g_generating);

    midi_clock_out::set_running(true);
    midi_clock_out::set_enabled(false);              // switched off
    TEST_ASSERT_FALSE(midi_handler::g_generating);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_it_tells_the_router_when_it_is_generating);
    RUN_TEST(test_silent_until_enabled_and_running);
    RUN_TEST(test_stops_when_it_is_no_longer_the_master);
    RUN_TEST(test_sends_the_clock_byte_only);
    RUN_TEST(test_tempo_does_not_drift_over_a_minute);
    RUN_TEST(test_awkward_tempo_does_not_drift);
    RUN_TEST(test_interval_carries_a_fraction);
    RUN_TEST(test_a_late_poll_resynchronises_rather_than_bursting);
    RUN_TEST(test_tempo_change_keeps_the_schedule);
    RUN_TEST(test_bpm_is_clamped_to_the_family_range);
    return UNITY_END();
}
