// Host-native unit tests for tap tempo + MIDI clock sync (app/tap_tempo.cpp).
//
// Tempo is a cross-cutting input: it sets the delay time / LFO rate of every
// tempo-syncable effect, so a wrong average or a missed reset is audible on the
// whole pedal. The module's math is pure (interval averaging over a ring buffer)
// but reads wall-clock time via systick::now_ms(); the systick fake in
// test/support lets us place taps and MIDI-clock messages at exact moments and
// assert the resulting BPM deterministically, with no real time elapsed.
//
// These tests pin down: BPM clamping, two-tap and multi-tap averaging, the
// "long gap restarts the window" rule, MIDI-clock BPM derivation + sync flag,
// the priority/clear interactions between tap and MIDI sync, and the
// fractional-period helper used for dotted/triplet subdivisions.

#include <unity.h>
#include <cstdint>

#include <pedal_core/tap_tempo.hpp>
#include "pedal_core_tempo_config.hpp"

namespace systick {
    void fake_set_ms(uint32_t ms);
    void fake_advance_ms(uint32_t ms);
}


void setUp(void) {
               // fake clock -> 0
    tap_tempo::midi_clock_reset();   // clear MIDI-clock ring + sync flag
    tap_tempo::set_bpm(120.0f);      // clears the tap window, baseline 120 BPM
}
void tearDown(void) {}

// Place a tap at the current fake time, then move the clock forward by gap_ms.
static void tap_then_advance(uint32_t gap_ms) {
    tap_tempo::tap();
    systick::fake_advance_ms(gap_ms);
}

// ---------------------------------------------------------------------------
// Defaults and direct set/clamp
// ---------------------------------------------------------------------------
void test_init_default_bpm(void) {
    tap_tempo::init();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, tap_tempo::get_bpm());
}

void test_init_clears_tap_window(void) {
    // init() is a full engine re-init: after building a slow tap window, a fresh set
    // of fast taps must average only over themselves. A partial init that left the
    // tap timestamps / count behind would blend the stale window into the new average.
    for (int i = 0; i < 4; ++i) tap_then_advance(1000);   // ~60 BPM window, clock now 4000
    tap_tempo::init();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, tap_tempo::get_bpm());  // BPM back to default

    tap_tempo::tap();                 // first tap of a clean window at t=4000
    systick::fake_advance_ms(250);
    tap_tempo::tap();                 // 250 ms later -> 240 BPM if the window was cleared
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 240.0f, tap_tempo::get_bpm());
}

void test_period_ms_matches_bpm(void) {
    // 120 BPM -> 500 ms per beat.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 500.0f, tap_tempo::get_period_ms());
}

void test_set_bpm_clamps_to_20_300(void) {
    // Bounds track config's shared BPM_MIN/BPM_MAX (also used by preset_manager).
    tap_tempo::set_bpm(BPM_MIN - 15.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, BPM_MIN, tap_tempo::get_bpm());   // floor (20.0)
    tap_tempo::set_bpm(BPM_MAX + 699.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, BPM_MAX, tap_tempo::get_bpm());   // ceiling (300.0)
    tap_tempo::set_bpm(145.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 145.0f, tap_tempo::get_bpm());    // in range
}

// ---------------------------------------------------------------------------
// Tap averaging
// ---------------------------------------------------------------------------
void test_single_tap_does_not_change_bpm(void) {
    // One tap has no interval to measure; BPM stays at the prior value.
    systick::fake_set_ms(1000);
    tap_tempo::tap();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, tap_tempo::get_bpm());
}

void test_two_taps_set_bpm_from_interval(void) {
    // 400 ms between two taps -> 60000/400 = 150 BPM.
    systick::fake_set_ms(1000);
    tap_then_advance(400);
    tap_tempo::tap();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 150.0f, tap_tempo::get_bpm());
}

void test_tap_returns_measured_flag(void) {
    // tap() reports whether it measured a new tempo: the first tap has no interval
    // (false); the second, a real interval later, does (true). A same-instant second
    // tap (zero span) still reports false. Callers use this to keep the first press inert.
    systick::fake_set_ms(1000);
    TEST_ASSERT_FALSE(tap_tempo::tap());          // first tap: nothing to measure
    TEST_ASSERT_FALSE(tap_tempo::tap());          // same ms: zero span, still nothing
    systick::fake_advance_ms(500);
    TEST_ASSERT_TRUE(tap_tempo::tap());           // 500 ms interval -> measured
}

void test_four_even_taps_average_to_bpm(void) {
    // Four taps 500 ms apart -> steady 120 BPM.
    systick::fake_set_ms(1000);
    tap_then_advance(500);
    tap_then_advance(500);
    tap_then_advance(500);
    tap_tempo::tap();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, tap_tempo::get_bpm());
}

void test_uneven_taps_use_window_average_not_last_interval(void) {
    // Intervals 400, 600, 400 ms. The *last* interval alone would read 150 BPM,
    // but the window average is span/3 = 1400/3 = 466.7 ms -> ~128.6 BPM. This
    // distinguishes true averaging from "BPM from the most recent interval".
    systick::fake_set_ms(1000);
    tap_then_advance(400);
    tap_then_advance(600);
    tap_then_advance(400);
    tap_tempo::tap();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 128.57f, tap_tempo::get_bpm());
}

void test_tap_count_survives_uint8_wrap(void) {
    // s_tap_count is a uint8_t bumped once per tap and reset only on a long gap. Sustained
    // continuous tapping must not let it wrap at 255 mid-window: a wrap would momentarily
    // drop it below 2 and stall the average for two taps (tap() returning false). Tap
    // steadily well past 256 taps and assert every tap after the first keeps measuring a
    // stable tempo — this fails on the two wrap-boundary taps without the fold-back guard.
    systick::fake_set_ms(1000);
    TEST_ASSERT_FALSE(tap_tempo::tap());              // first tap: no interval yet
    for (int i = 0; i < 300; ++i) {
        systick::fake_advance_ms(500);                // steady 120 BPM, under MAX_INTERVAL
        TEST_ASSERT_TRUE(tap_tempo::tap());           // never stalls across the wrap
        TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, tap_tempo::get_bpm());
    }
}

void test_two_taps_same_instant_does_not_spike_bpm(void) {
    // Two taps registered in the same systick ms give a zero interval. The BPM
    // derivation must skip rather than divide by zero (which would pin the tempo
    // to BPM_MAX); the prior BPM is kept until a real interval arrives.
    systick::fake_set_ms(1000);
    tap_tempo::tap();
    tap_tempo::tap();   // same ms, no advance -> span 0
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, tap_tempo::get_bpm());  // not BPM_MAX
}

void test_long_gap_restarts_averaging_window(void) {
    // A gap longer than TAP_TEMPO_MAX_INTERVAL discards the running window: the
    // next tap becomes a fresh "first" tap, so the BPM is then derived only from
    // the interval that follows it — not blended with the pre-gap taps.
    systick::fake_set_ms(1000);
    tap_then_advance(500);                       // establish a 120 BPM-ish history
    tap_then_advance(500);
    systick::fake_advance_ms(TAP_TEMPO_MAX_INTERVAL + 1);  // overlong gap
    tap_then_advance(300);                       // fresh first tap, then 300 ms
    tap_tempo::tap();                            // second tap of the new window
    // Only the 300 ms post-gap interval should count -> 60000/300 = 200 BPM.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 200.0f, tap_tempo::get_bpm());
}

// ---------------------------------------------------------------------------
// MIDI clock sync (24 pulses per quarter note)
// ---------------------------------------------------------------------------
static void send_midi_clock_beat(uint32_t tick_spacing_ms) {
    // 24 clock pulses spaced tick_spacing_ms apart fill one beat window.
    for (int i = 0; i < 24; ++i) {
        tap_tempo::midi_clock_tick();
        systick::fake_advance_ms(tick_spacing_ms);
    }
}

void test_midi_clock_derives_bpm_and_sets_sync(void) {
    // 20 ms/pulse * 24 = 480 ms/beat -> 125 BPM. Sync flag must latch true.
    systick::fake_set_ms(1000);
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());
    send_midi_clock_beat(20);
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 125.0f, tap_tempo::get_bpm());
}

void test_midi_clock_needs_full_beat_before_sync(void) {
    // Fewer than 24 pulses: not enough data, no sync, BPM unchanged.
    systick::fake_set_ms(1000);
    for (int i = 0; i < 23; ++i) {
        tap_tempo::midi_clock_tick();
        systick::fake_advance_ms(20);
    }
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, tap_tempo::get_bpm());
}

void test_midi_clock_tracks_tempo_change_through_ring_wrap(void) {
    // The 24-slot ring keeps rolling past the first beat. midi_clock_tick() reads the
    // oldest sample as `oldest = idx` *after* overwriting that slot — the subtle
    // wraparound case the single-beat tests never exercise. Drive a sustained tempo
    // change: once a fresh full beat at the new spacing has cycled through the ring,
    // the derived BPM must reflect only the new spacing (old samples fully flushed).
    systick::fake_set_ms(1000);
    send_midi_clock_beat(20);                  // 20 ms * 24 = 480 ms/beat -> 125 BPM
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 125.0f, tap_tempo::get_bpm());

    send_midi_clock_beat(16);                  // 24 fresh pulses at 16 ms spacing
    // Ring now holds only the 16 ms samples: 16 ms * 24 = 384 ms/beat -> 156.25 BPM.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 156.25f, tap_tempo::get_bpm());
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());
}

void test_midi_clock_zero_span_does_not_spike_bpm(void) {
    // A full beat of 24 clock pulses arriving within one systick ms (a flushed
    // backlog) spans zero time. The BPM derivation must skip rather than divide by
    // zero and pin the tempo to BPM_MAX; with no valid measurement, sync stays off.
    systick::fake_set_ms(1000);
    for (int i = 0; i < 24; ++i) {
        tap_tempo::midi_clock_tick();   // no advance -> all timestamps equal
    }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, tap_tempo::get_bpm());  // not BPM_MAX
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());
}

void test_midi_clock_reset_clears_sync(void) {
    systick::fake_set_ms(1000);
    send_midi_clock_beat(20);
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());
    tap_tempo::midi_clock_reset();
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());
}

void test_measured_tap_clears_midi_sync(void) {
    // Establishing a tap tempo (a measured >=2-tap interval) takes over from MIDI clock and
    // drops the sync flag. A lone first tap is inert and leaves sync intact.
    systick::fake_set_ms(1000);
    send_midi_clock_beat(20);
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());
    tap_tempo::tap();                                 // first tap: inert
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());    // sync still held
    systick::fake_advance_ms(400);
    tap_tempo::tap();                                 // second tap: measures a tempo
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());   // now the tap takes over
}

void test_set_bpm_clears_midi_sync(void) {
    systick::fake_set_ms(1000);
    send_midi_clock_beat(20);
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());
    tap_tempo::set_bpm(100.0f);
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());
}

// Sync status is not latched forever: if clock ticks stop arriving it expires
// MIDI_SYNC_TIMEOUT_MS after the last tick. The boundary is exclusive — still
// synced one ms before the timeout, dropped exactly at it.
void test_midi_sync_status_expires_after_timeout(void) {
    systick::fake_set_ms(1000);
    // Drive a full beat but leave the clock parked on the final tick (no trailing
    // advance), so "time since last tick" starts at zero and is easy to reason about.
    for (int i = 0; i < 24; ++i) {
        tap_tempo::midi_clock_tick();
        if (i < 23) systick::fake_advance_ms(20);
    }
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());                 // 0 ms since last tick
    systick::fake_advance_ms(MIDI_SYNC_TIMEOUT_MS - 1);
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());                 // just inside the window
    systick::fake_advance_ms(1);
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());                // at the timeout: expired
}

// A fresh clock tick after expiry re-arms the timeout window (status tracks the
// most recent tick, not the first beat). With the ring already full one tick is
// enough to re-derive BPM and re-latch sync.
void test_midi_sync_status_rearms_on_new_tick(void) {
    systick::fake_set_ms(1000);
    send_midi_clock_beat(20);
    systick::fake_advance_ms(MIDI_SYNC_TIMEOUT_MS + 50);
    TEST_ASSERT_FALSE(tap_tempo::is_midi_synced());               // lapsed
    tap_tempo::midi_clock_tick();                                 // one more tick
    TEST_ASSERT_TRUE(tap_tempo::is_midi_synced());               // re-synced
}

// The MIDI-clock tempo is remembered separately from the working/tap BPM, so a
// preset that loads while synced can be seeded from the live clock even after a
// tap has moved the working BPM. get_midi_bpm() must stay put across a tap.
void test_midi_bpm_kept_separate_from_tap(void) {
    systick::fake_set_ms(1000);
    send_midi_clock_beat(20);                          // 480 ms/beat -> 125 BPM
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 125.0f, tap_tempo::get_midi_bpm());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 125.0f, tap_tempo::get_bpm());

    // Tap a clearly different tempo: 300 ms interval -> 200 BPM working tempo.
    tap_then_advance(300);
    tap_tempo::tap();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 200.0f, tap_tempo::get_bpm());        // working BPM moved
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 125.0f, tap_tempo::get_midi_bpm());   // MIDI BPM unchanged
}

// ---------------------------------------------------------------------------
// Fractional period (dotted / triplet subdivisions)
// ---------------------------------------------------------------------------
void test_period_fractional_subdivisions(void) {
    tap_tempo::set_bpm(120.0f);   // 500 ms/beat
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 500.0f, tap_tempo::get_period_fractional(1, 1));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 250.0f, tap_tempo::get_period_fractional(1, 2));  // eighth
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 187.5f, tap_tempo::get_period_fractional(3, 8));  // dotted 16th
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 750.0f, tap_tempo::get_period_fractional(3, 2));  // dotted quarter
}

void test_period_fractional_zero_denominator_is_safe(void) {
    // Guard against divide-by-zero: denom 0 returns 0, not NaN/inf.
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, tap_tempo::get_period_fractional(1, 0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_init_default_bpm);
    RUN_TEST(test_init_clears_tap_window);
    RUN_TEST(test_period_ms_matches_bpm);
    RUN_TEST(test_set_bpm_clamps_to_20_300);

    RUN_TEST(test_single_tap_does_not_change_bpm);
    RUN_TEST(test_two_taps_set_bpm_from_interval);
    RUN_TEST(test_tap_returns_measured_flag);
    RUN_TEST(test_four_even_taps_average_to_bpm);
    RUN_TEST(test_uneven_taps_use_window_average_not_last_interval);
    RUN_TEST(test_tap_count_survives_uint8_wrap);
    RUN_TEST(test_two_taps_same_instant_does_not_spike_bpm);
    RUN_TEST(test_long_gap_restarts_averaging_window);

    RUN_TEST(test_midi_clock_derives_bpm_and_sets_sync);
    RUN_TEST(test_midi_clock_zero_span_does_not_spike_bpm);
    RUN_TEST(test_midi_clock_needs_full_beat_before_sync);
    RUN_TEST(test_midi_clock_tracks_tempo_change_through_ring_wrap);
    RUN_TEST(test_midi_clock_reset_clears_sync);
    RUN_TEST(test_measured_tap_clears_midi_sync);
    RUN_TEST(test_set_bpm_clears_midi_sync);
    RUN_TEST(test_midi_sync_status_expires_after_timeout);
    RUN_TEST(test_midi_sync_status_rearms_on_new_tick);
    RUN_TEST(test_midi_bpm_kept_separate_from_tap);

    RUN_TEST(test_period_fractional_subdivisions);
    RUN_TEST(test_period_fractional_zero_denominator_is_safe);
    return UNITY_END();
}
