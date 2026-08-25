// The footswitch gesture grammar: debounce, press vs hold, hold-release, and
// the independence of the two switches — the rule that makes both feet down
// mean both actions rather than a third, timing-sensitive one.
// Driven through the hal panel fake (raw switch state) and the systick fake (time).

#include <unity.h>
#include <cstdint>

#include <pedal_core/footswitch.hpp>
#include "pedal_core_ui_config.hpp"   // the stub's DEBOUNCE/HOLD values

namespace pedal_core::hal {
    void fake_panel_reset();
    void fake_set_fs(uint8_t idx, bool pressed);
}
namespace systick {
    void fake_set_ms(uint32_t ms);
    void fake_advance_ms(uint32_t ms);
}

using footswitch::Event;

void setUp(void)
{
    pedal_core::hal::fake_panel_reset();
    systick::fake_set_ms(1000u);
    footswitch::init();
    // Drain state left by a previous test: run the debouncer with both
    // switches released until quiet.
    for (int i = 0; i < 700; ++i) { systick::fake_advance_ms(1); footswitch::update(); }
    while (footswitch::has_event()) footswitch::get_event();
}
void tearDown(void) {}

static void tick_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; ++i) {
        systick::fake_advance_ms(1);
        footswitch::update();
    }
}

static void set_fs(uint8_t idx, bool pressed) { pedal_core::hal::fake_set_fs(idx, pressed); }

static uint8_t next_event(void) { return (uint8_t)footswitch::get_event(); }

// ---------------------------------------------------------------------------

void test_short_press_fs1(void) {
    set_fs(0, true);
    tick_ms(60);
    TEST_ASSERT_FALSE(footswitch::has_event());   // nothing on press-down
    set_fs(0, false);
    tick_ms(60);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS1_Press, next_event());
    TEST_ASSERT_FALSE(footswitch::has_event());
}

void test_short_press_fs2(void) {
    set_fs(1, true);
    tick_ms(60);
    set_fs(1, false);
    tick_ms(60);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS2_Press, next_event());
    TEST_ASSERT_FALSE(footswitch::has_event());
}

void test_bounce_is_ignored(void) {
    // Sustained chatter never clears the debounce window.
    for (int i = 0; i < 6; ++i) {
        set_fs(0, (i & 1) != 0);
        tick_ms(5);
    }
    set_fs(0, false);
    tick_ms(60);
    TEST_ASSERT_FALSE(footswitch::has_event());
}

void test_sub_debounce_glitch_rejected(void) {
    // A single glitch shorter than the debounce window never registers.
    set_fs(0, true);
    tick_ms(1);
    set_fs(0, false);
    tick_ms(FOOTSWITCH_DEBOUNCE_MS + 1u);
    TEST_ASSERT_FALSE(footswitch::has_event());
}

void test_hold_fires_mid_press_and_eats_the_press(void) {
    set_fs(1, true);
    tick_ms(FOOTSWITCH_HOLD_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS2_Hold, next_event());
    tick_ms(500);
    TEST_ASSERT_FALSE(footswitch::has_event());   // one hold per press
}

void test_hold_release_closes_the_hold(void) {
    // The hold/release pair is what a momentary action rides on: engage while
    // the foot is down, return when it lifts — and never a stray press.
    set_fs(0, true);
    tick_ms(FOOTSWITCH_HOLD_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS1_Hold, next_event());
    set_fs(0, false);
    tick_ms(60);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS1_HoldRelease, next_event());
    TEST_ASSERT_FALSE(footswitch::has_event());
}

void test_short_press_emits_no_hold_release(void) {
    set_fs(1, true);
    tick_ms(60);
    set_fs(1, false);
    tick_ms(60);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS2_Press, next_event());
    TEST_ASSERT_FALSE(footswitch::has_event());   // no HoldRelease without a hold
}

void test_both_pressed_reports_both_presses(void) {
    // Both feet down is both actions, in whatever order the switches settle —
    // there is no combined gesture to swallow them.
    set_fs(0, true);
    set_fs(1, true);
    tick_ms(60);
    set_fs(0, false);
    set_fs(1, false);
    tick_ms(60);

    bool saw_fs1 = false, saw_fs2 = false;
    while (footswitch::has_event()) {
        const uint8_t e = next_event();
        if (e == (uint8_t)Event::FS1_Press) saw_fs1 = true;
        if (e == (uint8_t)Event::FS2_Press) saw_fs2 = true;
    }
    TEST_ASSERT_TRUE(saw_fs1);
    TEST_ASSERT_TRUE(saw_fs2);
}

void test_both_held_reports_both_holds(void) {
    set_fs(0, true);
    set_fs(1, true);
    tick_ms(FOOTSWITCH_HOLD_MS + 100u);

    bool saw_fs1 = false, saw_fs2 = false;
    while (footswitch::has_event()) {
        const uint8_t e = next_event();
        if (e == (uint8_t)Event::FS1_Hold) saw_fs1 = true;
        if (e == (uint8_t)Event::FS2_Hold) saw_fs2 = true;
    }
    TEST_ASSERT_TRUE(saw_fs1);
    TEST_ASSERT_TRUE(saw_fs2);
}

void test_press_during_other_held_still_reports(void) {
    // Tapping one switch while leaning on the other is an ordinary press: a
    // momentary freeze on one foot does not mute the other switch.
    set_fs(1, true);
    tick_ms(FOOTSWITCH_HOLD_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS2_Hold, next_event());

    set_fs(0, true);
    tick_ms(60);
    set_fs(0, false);
    tick_ms(60);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS1_Press, next_event());
    TEST_ASSERT_FALSE(footswitch::has_event());   // FS2 stays held, silent
}

void test_staggered_holds_are_timed_separately(void) {
    // Each switch times its own hold from its own press, so a late-joining foot
    // waits out its own window rather than inheriting the first one's.
    set_fs(0, true);
    tick_ms(400);
    set_fs(1, true);
    tick_ms(FOOTSWITCH_HOLD_MS - 300u);

    // FS1 passed its window during that span; FS2 has not yet.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS1_Hold, next_event());
    TEST_ASSERT_FALSE(footswitch::has_event());

    tick_ms(400);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS2_Hold, next_event());
}

// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_short_press_fs1);
    RUN_TEST(test_short_press_fs2);
    RUN_TEST(test_bounce_is_ignored);
    RUN_TEST(test_sub_debounce_glitch_rejected);
    RUN_TEST(test_hold_fires_mid_press_and_eats_the_press);
    RUN_TEST(test_hold_release_closes_the_hold);
    RUN_TEST(test_short_press_emits_no_hold_release);
    RUN_TEST(test_both_pressed_reports_both_presses);
    RUN_TEST(test_both_held_reports_both_holds);
    RUN_TEST(test_press_during_other_held_still_reports);
    RUN_TEST(test_staggered_holds_are_timed_separately);
    return UNITY_END();
}
