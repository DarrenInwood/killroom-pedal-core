// Host-native unit tests for the encoder rotate-dispatch precedence
// (pedal_core/encoder_rotate.hpp). The decision is a pure, dependency-free helper so it
// is tested directly, without the encoder_ui collaborator graph (display_manager,
// tempo_led, midi_handler, a fake Effect, …).
//
// Pins the precedence and, in particular, the bug this seam exists to close: the
// dev/VCA-cal pages are entered by SysEx from any page, so cal can be active while the UI
// still believes it is on a param or settings page. Rotation must do exactly one thing.

#include <unity.h>

#include <pedal_core/encoder_rotate.hpp>

using encoder_ui_detail::RotateAction;
using encoder_ui_detail::rotate_action;

void setUp(void) {}
void tearDown(void) {}

// VCA-cal claims the encoder outright: a rotation nudges the offset and must not also
// reach the page the wizard was opened over.
void test_vca_cal_claims_the_encoder(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::VcaOffset,
                          (int)rotate_action(/*vca*/true, /*dev*/false));
}

// Dev-cal swallows rotation entirely — the knobs do the work on that page.
void test_dev_cal_swallows_rotation(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::None,
                          (int)rotate_action(/*vca*/false, /*dev*/true));
}

// VCA-cal outranks dev-cal: the wizard chains out of the dev page, and only one of the
// two can own a detent.
void test_vca_cal_outranks_dev_cal(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::VcaOffset,
                          (int)rotate_action(/*vca*/true, /*dev*/true));
}

// Default: the ordinary UI, where update() dispatches on its own Play/Edit mode.
void test_default_is_navigate(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::Navigate,
                          (int)rotate_action(false, false));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_vca_cal_claims_the_encoder);
    RUN_TEST(test_dev_cal_swallows_rotation);
    RUN_TEST(test_vca_cal_outranks_dev_cal);
    RUN_TEST(test_default_is_navigate);
    return UNITY_END();
}
