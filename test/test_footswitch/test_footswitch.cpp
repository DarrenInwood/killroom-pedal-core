// The footswitch gesture grammar: debounce, press vs hold, and Both_Hold
// timed from the later press — the rule that makes a two-foot save deliberate.
// Merged coverage from both consumers' suites, driven through the hal panel
// fake (raw switch state) and the systick fake (time).

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

// ---------------------------------------------------------------------------

void test_short_press_fs1(void) {
    set_fs(0, true);
    tick_ms(60);
    TEST_ASSERT_FALSE(footswitch::has_event());   // nothing on press-down
    set_fs(0, false);
    tick_ms(60);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS1_Press, (uint8_t)footswitch::get_event());
    TEST_ASSERT_FALSE(footswitch::has_event());
}

void test_short_press_fs2(void) {
    set_fs(1, true);
    tick_ms(60);
    set_fs(1, false);
    tick_ms(60);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS2_Press, (uint8_t)footswitch::get_event());
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

void test_hold_fires_once_and_eats_the_press(void) {
    set_fs(1, true);
    tick_ms(700);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::FS2_Hold, (uint8_t)footswitch::get_event());
    tick_ms(500);
    TEST_ASSERT_FALSE(footswitch::has_event());   // one hold per press
    set_fs(1, false);
    tick_ms(60);
    TEST_ASSERT_FALSE(footswitch::has_event());   // the release is not a press
}

void test_both_hold_once_suppresses_individual_holds(void) {
    set_fs(0, true);
    set_fs(1, true);
    tick_ms(60 + FOOTSWITCH_HOLD_MS);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::Both_Hold, (uint8_t)footswitch::get_event());
    TEST_ASSERT_FALSE(footswitch::has_event());   // exactly one event, no FS1/FS2_Hold
}

void test_both_hold_timed_from_later_press(void) {
    set_fs(0, true);
    tick_ms(400);                       // FS1 alone, under its own hold time
    set_fs(1, true);
    tick_ms(500);                       // overlap: 500 < 600 — nothing yet
    TEST_ASSERT_FALSE(footswitch::has_event());
    tick_ms(150);                       // overlap reaches 600
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Event::Both_Hold, (uint8_t)footswitch::get_event());
    set_fs(0, false);
    set_fs(1, false);
    tick_ms(60);
    TEST_ASSERT_FALSE(footswitch::has_event());   // no stray presses on release
}

void test_brief_overlap_never_saves(void) {
    set_fs(0, true);
    tick_ms(60);
    set_fs(1, true);
    tick_ms(100);                       // brief two-foot overlap
    set_fs(1, false);
    tick_ms(60);
    set_fs(0, false);
    tick_ms(60);
    while (footswitch::has_event())
        TEST_ASSERT_NOT_EQUAL((uint8_t)Event::Both_Hold, (uint8_t)footswitch::get_event());
}

void test_press_during_other_held_suppressed(void) {
    // Combos don't leak individual presses.
    set_fs(1, true);
    tick_ms(60);                        // FS2 held (debounced, still down)
    set_fs(0, true);
    tick_ms(60);
    set_fs(0, false);
    tick_ms(60);                        // FS1 released while FS2 still down
    TEST_ASSERT_FALSE(footswitch::has_event());   // no FS1_Press leaked
}

// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_short_press_fs1);
    RUN_TEST(test_short_press_fs2);
    RUN_TEST(test_bounce_is_ignored);
    RUN_TEST(test_sub_debounce_glitch_rejected);
    RUN_TEST(test_hold_fires_once_and_eats_the_press);
    RUN_TEST(test_both_hold_once_suppresses_individual_holds);
    RUN_TEST(test_both_hold_timed_from_later_press);
    RUN_TEST(test_brief_overlap_never_saves);
    RUN_TEST(test_press_during_other_held_suppressed);
    return UNITY_END();
}
