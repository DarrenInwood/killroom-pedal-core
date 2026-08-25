// Host-native unit tests for the external-input jack handler
// (src/external_input.cpp).
//
// Footswitch mode debounces the combined (tip, ring) contact state as a single
// 4-position switch and emits one press per gesture, on release, with 'Both'
// sticking once seen. Raw contact state arrives through the hal panel fake;
// time through the systick fake. Pin directions and the expression reference
// are the product's hal, outside this suite.

#include <unity.h>
#include <cstdint>

#include <pedal_core/external_input.hpp>
#include <pedal_core/ui/compositor.hpp>    // FUNCTION_LABEL_MAX
#include <cstring>
#include "pedal_core_ui_config.hpp"        // FOOTSWITCH_DEBOUNCE_MS
#include "pedal_core_extinput_config.hpp"  // EXT_DEFAULT_* actions

namespace pedal_core::hal {
    void fake_panel_reset();
    void fake_set_ext(bool tip, bool ring);
    bool fake_ext_footswitch_mode();
}
namespace systick {
    void fake_set_ms(uint32_t ms);
    void fake_advance_ms(uint32_t ms);
}

using external_input::Mode;
using external_input::Switch;
using external_input::Action;
using external_input::Event;

// Drive the contacts (logical pressed state; polarity is the product's).
static void set_contact(bool tip, bool ring) {
    pedal_core::hal::fake_set_ext(tip, ring);
}

// Advance the fake clock then run one handler tick.
static void tick(uint32_t dt_ms) {
    systick::fake_advance_ms(dt_ms);
    external_input::update();
}

// Move a raw contact change through the debounce filter so it becomes the debounced state:
// one tick to register the edge (records change_ms), one past FOOTSWITCH_DEBOUNCE_MS to
// confirm it.
static void settle(bool tip, bool ring) {
    set_contact(tip, ring);
    tick(1);
    tick(FOOTSWITCH_DEBOUNCE_MS + 1);
}

void setUp(void) {
    pedal_core::hal::fake_panel_reset();
    systick::fake_set_ms(1000u);
    // Footswitch mode also runs reset_debounce(), clearing the ring and arming state.
    external_input::set_mode(Mode::Footswitch);
    TEST_ASSERT_TRUE(pedal_core::hal::fake_ext_footswitch_mode());
    // Restore the default action assignment so per-test set_action() calls start clean.
    external_input::set_action(Switch::Tip,  (Action)EXT_DEFAULT_TIP_ACTION);
    external_input::set_action(Switch::Ring, (Action)EXT_DEFAULT_RING_ACTION);
    external_input::set_action(Switch::Both, (Action)EXT_DEFAULT_BOTH_ACTION);
}
void tearDown(void) {}

// A clean tip press emits exactly one TipPress on release, nothing on press-down.
void test_tip_press(void) {
    settle(true, false);                      // tip closed, debounced
    TEST_ASSERT_FALSE(external_input::has_event());   // nothing on press-down
    settle(false, false);                     // released, debounced
    TEST_ASSERT_EQUAL_INT((int)Event::TipPress, (int)external_input::get_event());
    TEST_ASSERT_FALSE(external_input::has_event());
}

// Ring alone maps to its own press event.
void test_ring_press(void) {
    settle(false, true);
    settle(false, false);
    TEST_ASSERT_EQUAL_INT((int)Event::RingPress, (int)external_input::get_event());
    TEST_ASSERT_FALSE(external_input::has_event());
}

// Both contacts closed together (the diode-ORed third switch) report BothPress.
void test_both_press(void) {
    settle(true, true);
    settle(false, false);
    TEST_ASSERT_EQUAL_INT((int)Event::BothPress, (int)external_input::get_event());
    TEST_ASSERT_FALSE(external_input::has_event());
}

// Contacts that settle a few ms apart on the way IN (tip first, then both) still report
// Both, because 'Both' is the strongest position seen during the gesture.
void test_escalate_tip_to_both_reports_both(void) {
    settle(true, false);                      // tip alone debounced first
    settle(true, true);                       // escalates to both
    settle(false, false);                     // release
    TEST_ASSERT_EQUAL_INT((int)Event::BothPress, (int)external_input::get_event());
}

// Contacts that release a few ms apart (both -> tip -> none) still report Both: once 'Both'
// is armed a lingering single contact during release must not downgrade the event.
void test_release_skew_still_reports_both(void) {
    settle(true, true);                       // both held, armed = Both
    settle(true, false);                      // ring releases first, tip lingers (debounced)
    settle(false, false);                     // tip releases
    TEST_ASSERT_EQUAL_INT((int)Event::BothPress, (int)external_input::get_event());
}

// A glitch shorter than the debounce window never registers as a press.
void test_sub_debounce_glitch_rejected(void) {
    set_contact(true, false);                 // tip closed
    tick(1);                                  // edge seen, not yet debounced
    set_contact(false, false);                // released well before DEBOUNCE_MS
    tick(FOOTSWITCH_DEBOUNCE_MS + 1);
    TEST_ASSERT_FALSE(external_input::has_event());
}

// In Expression mode update() is inert: contact changes never produce events.
void test_expression_mode_inert(void) {
    external_input::set_mode(Mode::Expression);
    TEST_ASSERT_FALSE(pedal_core::hal::fake_ext_footswitch_mode());
    settle(true, true);                       // would be a Both press in Footswitch mode
    settle(false, false);
    TEST_ASSERT_FALSE(external_input::has_event());
    TEST_ASSERT_EQUAL_INT((int)Mode::Expression, (int)external_input::mode());
}

// set_mode() resets pending state: a press queued in one mode is cleared on the next
// set_mode(), so a stale event never survives a mode change.
void test_set_mode_clears_pending(void) {
    settle(true, false);
    settle(false, false);                     // a TipPress is now queued
    TEST_ASSERT_TRUE(external_input::has_event());
    external_input::set_mode(Mode::Footswitch);
    TEST_ASSERT_FALSE(external_input::has_event());
}

// Two complete gestures queue two events, delivered oldest-first (FIFO ring).
void test_events_fifo_order(void) {
    settle(true, false);  settle(false, false);   // TipPress
    settle(false, true);  settle(false, false);   // RingPress
    TEST_ASSERT_EQUAL_INT((int)Event::TipPress,  (int)external_input::get_event());
    TEST_ASSERT_EQUAL_INT((int)Event::RingPress, (int)external_input::get_event());
    TEST_ASSERT_FALSE(external_input::has_event());
}

// set_action() stores in-range assignments and rejects out-of-range ones (leaving the
// prior value); action() on an out-of-range switch returns None.
void test_set_action_bounds(void) {
    external_input::set_action(Switch::Tip, Action::AlgoUp);
    TEST_ASSERT_EQUAL_INT((int)Action::AlgoUp, (int)external_input::action(Switch::Tip));
    // Count is out of range: the assignment is ignored, the prior value stands.
    external_input::set_action(Switch::Tip, Action::Count);
    TEST_ASSERT_EQUAL_INT((int)Action::AlgoUp, (int)external_input::action(Switch::Tip));
    // An out-of-range switch reads back None rather than indexing past the array.
    TEST_ASSERT_EQUAL_INT((int)Action::None, (int)external_input::action(Switch::Count));
}

// Every action has a name, and every name fits the row that shows it. The context line
// carries what the second footswitch currently does, and a name too long for that buffer
// is truncated silently — which reads on the pedal as a misspelling rather than as a
// layout problem, so nobody would think to look here.
void test_every_action_is_named_and_fits_the_row(void) {
    for (uint8_t v = 0; v < (uint8_t)Action::Count; ++v) {
        const char* name = external_input::action_name((Action)v);
        TEST_ASSERT_NOT_NULL(name);
        // "Off" is None's name; any other action falling back to it is a missing case.
        if (v != (uint8_t)Action::None)
            TEST_ASSERT_TRUE_MESSAGE(strcmp(name, "Off") != 0, name);
        TEST_ASSERT_TRUE_MESSAGE(
            strlen(name) <= pedal_core::ui::Compositor::FUNCTION_LABEL_MAX, name);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tip_press);
    RUN_TEST(test_ring_press);
    RUN_TEST(test_both_press);
    RUN_TEST(test_escalate_tip_to_both_reports_both);
    RUN_TEST(test_release_skew_still_reports_both);
    RUN_TEST(test_sub_debounce_glitch_rejected);
    RUN_TEST(test_expression_mode_inert);
    RUN_TEST(test_set_mode_clears_pending);
    RUN_TEST(test_events_fifo_order);
    RUN_TEST(test_set_action_bounds);
    RUN_TEST(test_every_action_is_named_and_fits_the_row);
    return UNITY_END();
}
