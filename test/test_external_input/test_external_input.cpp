// Host-native unit tests for the external-input jack handler
// (src/external_input.cpp).
//
// Footswitch mode debounces the combined (tip, ring) contact state as a single
// 4-position switch, with 'Both' sticking once seen. Each switch carries two
// gestures: a hold fires mid-press, and a press is emitted on the release and
// only where no hold fired. Raw contact state arrives through the hal panel
// fake; time through the systick fake. Pin directions and the expression
// reference are the product's hal, outside this suite.

#include <unity.h>
#include <cstdint>

#include <pedal_core/external_input.hpp>
#include "pedal_core_ui_config.hpp"        // FOOTSWITCH_DEBOUNCE_MS, FOOTSWITCH_HOLD_MS
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
    // One settled tick with both contacts open, so the handler learns this cable can open
    // them. A contact never seen open is taken for a short -- a TS plug grounding the ring
    // -- and masked out, which is what stops it engaging a hold that never lets go.
    set_contact(false, false);
    tick(1);
    // Restore the default action assignment so per-test set_action() calls start clean.
    external_input::set_action(Switch::Tip,  false, (Action)EXT_DEFAULT_TIP_ACTION);
    external_input::set_action(Switch::Ring, false, (Action)EXT_DEFAULT_RING_ACTION);
    external_input::set_action(Switch::Both, false, (Action)EXT_DEFAULT_BOTH_ACTION);
    // The holds start unassigned, as they do on a pedal out of the box.
    external_input::set_action(Switch::Tip,  true, Action::None);
    external_input::set_action(Switch::Ring, true, Action::None);
    external_input::set_action(Switch::Both, true, Action::None);
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

// A lean on the switch fires the hold mid-press, so a momentary action engages while the
// foot is still down -- and the release closes that hold rather than also sending a press.
void test_tip_hold_fires_mid_press_and_releases(void) {
    external_input::set_action(Switch::Tip, true, Action::MomFreeze);
    settle(true, false);
    TEST_ASSERT_FALSE(external_input::has_event());   // nothing yet: still inside the window
    tick(FOOTSWITCH_HOLD_MS);
    TEST_ASSERT_EQUAL_INT((int)Event::TipHold, (int)external_input::get_event());
    TEST_ASSERT_FALSE(external_input::has_event());   // and only once

    settle(false, false);
    TEST_ASSERT_EQUAL_INT((int)Event::TipHoldRelease, (int)external_input::get_event());
    TEST_ASSERT_FALSE(external_input::has_event());   // never a press as well
}

// Both contacts of a three-button press close milliseconds apart, far inside the hold
// window, so the escalation lands before the threshold and the hold is a Both.
void test_both_hold_survives_the_contact_skew(void) {
    external_input::set_action(Switch::Both, true, Action::MomFreeze);
    settle(true, false);
    settle(true, true);
    tick(FOOTSWITCH_HOLD_MS);
    TEST_ASSERT_EQUAL_INT((int)Event::BothHold, (int)external_input::get_event());
    settle(false, false);
    TEST_ASSERT_EQUAL_INT((int)Event::BothHoldRelease, (int)external_input::get_event());
}

// Adding the second contact AFTER the hold has fired keeps the hold that is running.
// Promoting it would have to release one momentary action and engage another mid-gesture,
// which is an audible glitch nobody asked for -- and the tip hold is a thing the player
// did actually ask for.
void test_escalating_after_the_hold_keeps_it(void) {
    external_input::set_action(Switch::Tip,  true, Action::MomFreeze);
    external_input::set_action(Switch::Both, true, Action::MomFreeze);
    settle(true, false);
    tick(FOOTSWITCH_HOLD_MS);
    TEST_ASSERT_EQUAL_INT((int)Event::TipHold, (int)external_input::get_event());

    settle(true, true);                               // ring joins late
    TEST_ASSERT_FALSE(external_input::has_event());   // no second hold, no promotion

    settle(false, false);
    TEST_ASSERT_EQUAL_INT((int)Event::TipHoldRelease, (int)external_input::get_event());
}

// A contact with nothing on its hold keeps its press, however long the stomp. The press is
// emitted on the release and only where no hold fired, so a hold that fired regardless
// would leave that contact dead to anyone who leant on it -- which is most feet on a switch
// under a boot, and every contact on a pedal out of the box.
void test_an_unassigned_hold_leaves_the_press_alone(void) {
    settle(true, false);                              // Tip hold is None, from setUp
    tick(FOOTSWITCH_HOLD_MS * 3u);                    // lean on it, well past the threshold
    TEST_ASSERT_FALSE(external_input::has_event());   // no hold fired

    settle(false, false);
    TEST_ASSERT_EQUAL_INT((int)Event::TipPress, (int)external_input::get_event());
    TEST_ASSERT_FALSE(external_input::has_event());
}

// A contact that has never been seen open is a short, not a foot: a TS plug grounds the
// ring, and reading that as a press held forever would engage the ring's hold action and
// never release it. It is masked out instead -- and the tip still works, so a TS cable
// degrades to a single-switch pedal rather than to a stuck one.
void test_a_contact_never_seen_open_is_ignored(void) {
    external_input::set_action(Switch::Ring, true, Action::MomFreeze);
    external_input::set_mode(Mode::Footswitch);       // clears what setUp learned
    set_contact(false, true);                         // ring shorted from the outset
    tick(1);
    tick(FOOTSWITCH_DEBOUNCE_MS + 1);
    tick(FOOTSWITCH_HOLD_MS);
    TEST_ASSERT_FALSE(external_input::has_event());   // no ring press, and no ring hold

    settle(true, true);                               // tip pressed, ring still shorted
    settle(false, true);
    TEST_ASSERT_EQUAL_INT((int)Event::TipPress, (int)external_input::get_event());
}

// The two gestures are assigned apart, and read back apart.
void test_press_and_hold_are_separate_assignments(void) {
    external_input::set_action(Switch::Tip, false, Action::Bypass);
    external_input::set_action(Switch::Tip, true,  Action::MomFreeze);
    TEST_ASSERT_EQUAL_INT((int)Action::Bypass,    (int)external_input::action(Switch::Tip, false));
    TEST_ASSERT_EQUAL_INT((int)Action::MomFreeze, (int)external_input::action(Switch::Tip, true));
    // And a switch out of range reads None for either gesture rather than indexing past.
    TEST_ASSERT_EQUAL_INT((int)Action::None, (int)external_input::action(Switch::Count, true));
}

// set_action() stores in-range assignments and rejects out-of-range ones (leaving the
// prior value); action() on an out-of-range switch returns None.
void test_set_action_bounds(void) {
    external_input::set_action(Switch::Tip, false, Action::AlgoUp);
    TEST_ASSERT_EQUAL_INT((int)Action::AlgoUp, (int)external_input::action(Switch::Tip, false));
    // Count is out of range: the assignment is ignored, the prior value stands.
    external_input::set_action(Switch::Tip, false, Action::Count);
    TEST_ASSERT_EQUAL_INT((int)Action::AlgoUp, (int)external_input::action(Switch::Tip, false));
    // An out-of-range switch reads back None rather than indexing past the array.
    TEST_ASSERT_EQUAL_INT((int)Action::None, (int)external_input::action(Switch::Count, false));
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
    RUN_TEST(test_tip_hold_fires_mid_press_and_releases);
    RUN_TEST(test_both_hold_survives_the_contact_skew);
    RUN_TEST(test_escalating_after_the_hold_keeps_it);
    RUN_TEST(test_an_unassigned_hold_leaves_the_press_alone);
    RUN_TEST(test_a_contact_never_seen_open_is_ignored);
    RUN_TEST(test_press_and_hold_are_separate_assignments);
    return UNITY_END();
}
