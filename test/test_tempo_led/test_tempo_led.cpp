// Host-native unit tests for the tempo LED blinker (src/tempo_led.cpp).
//
// The blinker drives the panel's second LED through hal::panel_led; the hal
// panel fake records the logical state and counts the calls, so the
// write-if-changed suppression is observable. Pin polarity and any no-glitch
// sequencing live in the product's hal implementation, outside this suite.
//
// Coverage: period==0 blanks the LED, the 25% duty phase (on for the first
// quarter of each period, off for the rest), the phase wrap when dt overshoots
// the period (including a multi-period overshoot), reset() restarting the
// phase, the write-if-changed suppression that keeps the seam quiet between
// transitions, and the MIDI-activity blip that borrows the LED without losing
// the beat under it.

#include <unity.h>
#include <cstdint>

#include <pedal_core/tempo_led.hpp>
#include "pedal_core_tempo_config.hpp"

namespace pedal_core::hal {
    void     fake_panel_reset();
    bool     fake_led(uint8_t idx);
    unsigned fake_led_writes();
}

static bool     led_on()  { return pedal_core::hal::fake_led(1u); }
static unsigned writes()  { return pedal_core::hal::fake_led_writes(); }

void setUp(void) {
    pedal_core::hal::fake_panel_reset();
    tempo_led::init();   // forces the LED off with exactly one write
}
void tearDown(void) {}

// init() drives the LED off through the seam with exactly one call; the
// product's pins_init owns any pre-output no-glitch sequencing.
void test_init_forces_led_off(void) {
    TEST_ASSERT_FALSE(led_on());
    TEST_ASSERT_EQUAL_INT(1, writes());
}

// period_ms == 0 (no tempo-synced rate) blanks the LED.
void test_zero_period_blanks_led(void) {
    tempo_led::reset();
    tempo_led::update(0.0f, 100u);   // phase 0 -> would be on
    TEST_ASSERT_TRUE(led_on());
    tempo_led::update(0.0f, 0u);     // period 0 -> off
    TEST_ASSERT_FALSE(led_on());
}

// LED is on for the first quarter of the period and off for the rest (25% duty).
void test_twenty_five_percent_duty(void) {
    tempo_led::reset();
    tempo_led::update(0.0f, 100u);   // phase 0  -> on
    TEST_ASSERT_TRUE(led_on());
    tempo_led::update(20.0f, 100u);  // phase 20 -> on (< 25)
    TEST_ASSERT_TRUE(led_on());
    tempo_led::update(10.0f, 100u);  // phase 30 -> off (>= 25)
    TEST_ASSERT_FALSE(led_on());
    tempo_led::update(60.0f, 100u);  // phase 90 -> off
    TEST_ASSERT_FALSE(led_on());
}

// Overshooting the period wraps the phase rather than latching.
void test_phase_wraps_on_overshoot(void) {
    tempo_led::reset();
    tempo_led::update(0.0f, 100u);    // phase 0 -> on
    tempo_led::update(110.0f, 100u);  // phase 110 -> wraps to 10 -> on
    TEST_ASSERT_TRUE(led_on());
    tempo_led::update(50.0f, 100u);   // phase 60 -> off
    TEST_ASSERT_FALSE(led_on());
}

// A dt overshooting several whole periods wraps all the way down, not just once
// (the subtraction loop keeps going while phase >= period). 250 over a 100 period
// lands at 50 -> off (past the 25 quarter-mark), proving it wrapped past 150 too.
void test_phase_wraps_multiple_periods(void) {
    tempo_led::reset();
    tempo_led::update(0.0f, 100u);    // phase 0 -> on
    tempo_led::update(250.0f, 100u);  // phase 250 -> wraps to 50 -> off (>= 25)
    TEST_ASSERT_FALSE(led_on());
    tempo_led::update(60.0f, 100u);   // phase 110 -> wraps to 10 -> on
    TEST_ASSERT_TRUE(led_on());
}

// reset() restarts the phase so the next update lands at the start (LED on).
void test_reset_restarts_phase(void) {
    tempo_led::reset();
    tempo_led::update(70.0f, 100u);   // phase 70 -> off
    TEST_ASSERT_FALSE(led_on());
    tempo_led::reset();
    tempo_led::update(0.0f, 100u);    // phase 0 -> on
    TEST_ASSERT_TRUE(led_on());
}

// No redundant seam writes while the LED stays in the same half of the period.
void test_write_if_changed_suppression(void) {
    tempo_led::reset();
    tempo_led::update(0.0f, 100u);   // off -> on : one write
    const int after_on = (int)writes();
    tempo_led::update(10.0f, 100u);  // still on : no write
    tempo_led::update(10.0f, 100u);  // still on : no write
    TEST_ASSERT_EQUAL_INT(after_on, (int)writes());
    tempo_led::update(40.0f, 100u);  // phase 60 -> off : one write
    TEST_ASSERT_EQUAL_INT(after_on + 1, (int)writes());
}

// The blip is the pedal's MIDI receive indicator, so it has to light the LED
// during the dark three-quarters of a beat -- that is when a player looking for
// an answer would otherwise see nothing.
void test_blip_lights_the_led_mid_beat(void) {
    tempo_led::reset();
    tempo_led::update(60.0f, 100u);            // phase 60 -> off
    TEST_ASSERT_FALSE(led_on());

    tempo_led::blip();
    tempo_led::update(1.0f, 100u);
    TEST_ASSERT_TRUE(led_on());
}

// And on an algorithm with no tempo at all, where the LED is otherwise dark.
void test_blip_lights_the_led_with_no_tempo(void) {
    tempo_led::reset();
    tempo_led::update(0.0f, 0u);
    TEST_ASSERT_FALSE(led_on());

    tempo_led::blip();
    tempo_led::update(1.0f, 0u);
    TEST_ASSERT_TRUE(led_on());
}

// The dwell is bounded, and the beat carried on underneath: after a blip that
// spans a downbeat the LED is back in phase rather than restarted.
void test_blip_expires_and_the_beat_kept_running(void) {
    tempo_led::reset();
    tempo_led::update(0.0f, 100u);   // phase 0, on
    tempo_led::blip();

    tempo_led::update(10.0f, 100u);  // dwell 40 left, phase 10
    tempo_led::update(30.0f, 100u);  // dwell 10 left, phase 40 -- the beat would be dark
    TEST_ASSERT_TRUE(led_on());

    tempo_led::update(20.0f, 100u);  // the pass that spends the dwell still lights it
    TEST_ASSERT_TRUE(led_on());
    tempo_led::update(0.0f, 100u);   // and the next one is back on the beat: phase 60
    TEST_ASSERT_FALSE(led_on());

    // The phase advanced through the blip, so the next downbeat lands where the
    // beat says it should rather than one dwell late.
    tempo_led::update(40.0f, 100u);  // phase wraps to 0
    TEST_ASSERT_TRUE(led_on());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_init_forces_led_off);
    RUN_TEST(test_zero_period_blanks_led);
    RUN_TEST(test_twenty_five_percent_duty);
    RUN_TEST(test_phase_wraps_on_overshoot);
    RUN_TEST(test_phase_wraps_multiple_periods);
    RUN_TEST(test_reset_restarts_phase);
    RUN_TEST(test_write_if_changed_suppression);
    RUN_TEST(test_blip_lights_the_led_mid_beat);
    RUN_TEST(test_blip_lights_the_led_with_no_tempo);
    RUN_TEST(test_blip_expires_and_the_beat_kept_running);
    return UNITY_END();
}
