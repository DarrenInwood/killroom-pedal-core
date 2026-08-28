// Host-native unit tests for the action vocabulary (include/pedal_core/action.hpp).
//
// The vocabulary is what every switch on the pedal is assigned from — the two on the
// front panel and both contacts of the external jack — so it is tested on its own,
// with neither a jack nor a screen present. What the pedal DOES with an action is the
// product's; this suite covers the vocabulary's own rules: what each action is called,
// which gesture it belongs on, how a settings row sweeps it, and the row width the
// names imply.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include <pedal_core/action.hpp>
#include <pedal_core/ui/compositor.hpp>    // FUNCTION_LABEL_MAX, derived from this vocabulary

// The vocabulary must not drag in the product's external-input config: a product with no
// jack still assigns its panel switches from it. If action.hpp reached that header, this
// would redeclare its EXT_ACTION_COUNT with a different type and fail to compile.
inline constexpr int EXT_ACTION_COUNT = 0;

using pedal_core::action::Action;
using pedal_core::action::action_name;
using pedal_core::action::action_is_momentary;
using pedal_core::action::action_allows_hold;
using pedal_core::action::action_allows_press;
using pedal_core::action::action_for_gesture;
using pedal_core::action::action_count_for;
using pedal_core::action::action_at;
using pedal_core::action::action_pos_of;

void setUp(void) {}
void tearDown(void) {}

// Every action has a name of its own. "Off" is None's name, so any other action
// answering it is a missing case in the switch rather than a deliberate label.
void test_every_action_is_named(void) {
    for (uint8_t v = 0; v < (uint8_t)Action::Count; ++v) {
        const char* name = action_name((Action)v);
        TEST_ASSERT_NOT_NULL(name);
        if (v != (uint8_t)Action::None)
            TEST_ASSERT_TRUE_MESSAGE(strcmp(name, "Off") != 0, name);
    }
}

// LONGEST_NAME is the vocabulary's own answer, computed at compile time. Pinned against
// the name it currently belongs to, so a longer one added later shows up here as the
// derivation moving rather than as a row that quietly truncates.
void test_longest_name_is_derived(void) {
    uint8_t longest = 0;
    for (uint8_t v = 0; v < (uint8_t)Action::Count; ++v) {
        const uint8_t n = (uint8_t)strlen(action_name((Action)v));
        if (n > longest) longest = n;
    }
    TEST_ASSERT_EQUAL_UINT8(longest, pedal_core::action::LONGEST_NAME);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)strlen("Algorithm Down"), pedal_core::action::LONGEST_NAME);
}

// The context row sizes itself from the vocabulary with a character to spare, so every
// name fits the buffer that shows it with room left for the terminator.
void test_every_name_fits_the_row(void) {
    for (uint8_t v = 0; v < (uint8_t)Action::Count; ++v) {
        const char* name = action_name((Action)v);
        TEST_ASSERT_TRUE_MESSAGE(
            strlen(name) <= pedal_core::ui::Compositor::FUNCTION_LABEL_MAX, name);
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(pedal_core::action::LONGEST_NAME + 1u),
                            pedal_core::ui::Compositor::FUNCTION_LABEL_MAX);
}

// Every momentary action is hold-only, and every hold-only action is momentary: what a
// switch does does not depend on which gesture reached it.
void test_momentary_actions_are_exactly_the_hold_only_ones(void) {
    for (uint8_t v = 0; v < (uint8_t)Action::Count; ++v) {
        const Action a = (Action)v;
        TEST_ASSERT_EQUAL_MESSAGE(action_is_momentary(a), !action_allows_press(a),
                                  action_name(a));
    }
}

// A tap needs one event per press, and a hold gives one event per lean on the switch,
// so tapping a tempo with a hold is not something anyone can do.
void test_tap_is_press_only_and_freeze_splits_by_gesture(void) {
    TEST_ASSERT_FALSE(action_allows_hold(Action::Tap));
    TEST_ASSERT_TRUE(action_allows_press(Action::Tap));
    TEST_ASSERT_FALSE(action_allows_hold(Action::Freeze));    // the latching one
    TEST_ASSERT_TRUE(action_allows_hold(Action::MomFreeze));  // the held one
}

// An assignment naming one gesture's action is read as the other gesture's counterpart,
// so what the switch does is unchanged.
void test_gesture_counterparts(void) {
    TEST_ASSERT_EQUAL_INT((int)Action::MomFreeze, (int)action_for_gesture(Action::Freeze, true));
    TEST_ASSERT_EQUAL_INT((int)Action::Freeze,    (int)action_for_gesture(Action::MomFreeze, false));
    TEST_ASSERT_EQUAL_INT((int)Action::Bypass,    (int)action_for_gesture(Action::MomentaryBypass, false));
    // Tap has no hold counterpart at all, so a hold naming it does nothing.
    TEST_ASSERT_EQUAL_INT((int)Action::None,      (int)action_for_gesture(Action::Tap, true));
    // One that fires once and is done works either way, so it comes back unchanged.
    TEST_ASSERT_EQUAL_INT((int)Action::PresetUp,  (int)action_for_gesture(Action::PresetUp, true));
    TEST_ASSERT_EQUAL_INT((int)Action::PresetUp,  (int)action_for_gesture(Action::PresetUp, false));
}

// The sweep a settings row steps through: the count, the lookup and the inverse agree,
// which is what stops a row landing on a value it cannot step from.
void test_the_sweep_is_consistent_in_both_directions(void) {
    for (uint8_t h = 0; h < 2u; ++h) {
        const bool hold = (h == 1u);
        const uint8_t n = action_count_for(hold);
        TEST_ASSERT_GREATER_THAN_UINT8(0, n);
        for (uint8_t pos = 0; pos < n; ++pos) {
            const Action a = action_at(hold, pos);
            TEST_ASSERT_TRUE(hold ? action_allows_hold(a) : action_allows_press(a));
            TEST_ASSERT_EQUAL_UINT8(pos, action_pos_of(a, hold));
        }
        // Off is code 0 and legal on either gesture, so the sweep always starts there.
        TEST_ASSERT_EQUAL_INT((int)Action::None, (int)action_at(hold, 0));
        // Past the end there is nothing to land on.
        TEST_ASSERT_EQUAL_INT((int)Action::None, (int)action_at(hold, n));
    }
}

// An action the gesture cannot carry has no position, and answers Off's, so a row is
// always on a value it can step from.
void test_an_action_the_gesture_cannot_carry_has_no_position(void) {
    TEST_ASSERT_EQUAL_UINT8(0, action_pos_of(Action::Tap, true));
    TEST_ASSERT_EQUAL_UINT8(0, action_pos_of(Action::MomFreeze, false));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_action_is_named);
    RUN_TEST(test_longest_name_is_derived);
    RUN_TEST(test_every_name_fits_the_row);
    RUN_TEST(test_momentary_actions_are_exactly_the_hold_only_ones);
    RUN_TEST(test_tap_is_press_only_and_freeze_splits_by_gesture);
    RUN_TEST(test_gesture_counterparts);
    RUN_TEST(test_the_sweep_is_consistent_in_both_directions);
    RUN_TEST(test_an_action_the_gesture_cannot_carry_has_no_position);
    return UNITY_END();
}
