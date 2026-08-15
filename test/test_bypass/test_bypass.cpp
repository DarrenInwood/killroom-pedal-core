// Host-native unit tests for the bypass module (src/bypass.cpp).
//
// The relay and status LED reach the panel through the hal seam; the hal panel
// fake records their logical state, and the systick fake drives the flash
// clock. Compiled into this TU (donor idiom) so the flash counter's file-local
// state is resettable between tests.

#include <unity.h>
#include <cstdint>

namespace pedal_core::hal {
    void fake_panel_reset();
    bool fake_led(uint8_t idx);
    bool fake_relay();
}
namespace systick {
    void fake_set_ms(uint32_t ms);
    void fake_advance_ms(uint32_t ms);
}

#include "../../src/bypass.cpp"

static bool relay() { return pedal_core::hal::fake_relay(); }
static bool led()   { return pedal_core::hal::fake_led(0u); }

void setUp(void) {
    pedal_core::hal::fake_panel_reset();
    s_flash_count = 0;       // clear state a previous test left in the statics
    bypass::init();          // bypassed: relay disengaged, LED off
    systick::fake_set_ms(0u);
}
void tearDown(void) {}

// Power-on init bypasses with the relay disengaged and the LED off.
void test_init_bypassed_led_off(void) {
    TEST_ASSERT_FALSE(bypass::is_active());
    TEST_ASSERT_FALSE(relay());
    TEST_ASSERT_FALSE(led());
}

// Activating engages the relay (effect in path) and lights the LED.
void test_set_active_engages_relay(void) {
    bypass::set_active(true);
    TEST_ASSERT_TRUE(bypass::is_active());
    TEST_ASSERT_TRUE(relay());
    TEST_ASSERT_TRUE(led());
}

// Deactivating disengages the relay (true bypass) and clears the LED.
void test_set_inactive_disengages_relay(void) {
    bypass::set_active(true);
    bypass::set_active(false);
    TEST_ASSERT_FALSE(bypass::is_active());
    TEST_ASSERT_FALSE(relay());
    TEST_ASSERT_FALSE(led());
}

// toggle() flips the active state, and the relay + LED follow it.
void test_toggle_flips_state(void) {
    TEST_ASSERT_FALSE(bypass::is_active());
    bypass::toggle();
    TEST_ASSERT_TRUE(bypass::is_active());
    TEST_ASSERT_TRUE(relay());
    bypass::toggle();
    TEST_ASSERT_FALSE(bypass::is_active());
    TEST_ASSERT_FALSE(relay());
}

// flash(n) blinks the LED 2n edges over the flash period, then restores it to
// the current active state.
void test_flash_blinks_then_restores(void) {
    bypass::flash(2);                       // 2 blinks = 4 toggles
    bool seen_on = false;
    for (int i = 0; i < 4; ++i) {
        systick::fake_advance_ms(FLASH_PERIOD_MS);
        bypass::update();
        if (led()) seen_on = true;
    }
    TEST_ASSERT_TRUE(seen_on);              // LED actually blinked on
    TEST_ASSERT_FALSE(led());               // restored to bypassed (LED off)
    TEST_ASSERT_EQUAL_UINT8(0, s_flash_count);
}

// set_active() called mid-flash must NOT yank the LED to the active state — the
// flash owns the LED until it finishes (the `if (s_flash_count == 0) sync_led()`
// guard). The relay engages immediately, and once the flash completes the
// restoring sync_led() reflects the NEW active state, not the one in effect when
// flash() was called.
void test_set_active_during_flash_defers_led_then_restores(void) {
    bypass::flash(2);                       // 2 blinks = 4 toggles; starts bypassed
    systick::fake_advance_ms(FLASH_PERIOD_MS);
    bypass::update();                       // edge 1: LED on, count 3

    bypass::set_active(true);               // become active mid-flash
    TEST_ASSERT_TRUE(bypass::is_active());
    TEST_ASSERT_TRUE(relay());              // relay engages right away
    TEST_ASSERT_TRUE(led());                // flash still owns LED (was on)

    systick::fake_advance_ms(FLASH_PERIOD_MS);
    bypass::update();                       // edge 2: LED off — even though active
    TEST_ASSERT_FALSE(led());               // proves flash, not s_active, drives LED
    TEST_ASSERT_TRUE(bypass::is_active());
    TEST_ASSERT_TRUE(relay());              // relay stays engaged throughout

    systick::fake_advance_ms(FLASH_PERIOD_MS);
    bypass::update();                       // edge 3: LED on, count 1
    systick::fake_advance_ms(FLASH_PERIOD_MS);
    bypass::update();                       // edge 4: count 0 -> restore to active
    TEST_ASSERT_EQUAL_UINT8(0, s_flash_count);
    TEST_ASSERT_TRUE(led());                // settled to the NEW active state
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_init_bypassed_led_off);
    RUN_TEST(test_set_active_engages_relay);
    RUN_TEST(test_set_inactive_disengages_relay);
    RUN_TEST(test_toggle_flips_state);
    RUN_TEST(test_flash_blinks_then_restores);
    RUN_TEST(test_set_active_during_flash_defers_led_then_restores);
    return UNITY_END();
}
