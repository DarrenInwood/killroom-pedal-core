// Host-native unit tests for the encoder rotate-dispatch precedence
// (src/app/encoder_rotate.hpp). The decision is a pure, dependency-free helper so it
// is tested directly, without the encoder_ui collaborator graph (display_manager,
// tempo_led, midi_handler, a fake Effect, …).
//
// Pins the precedence and, in particular, the bug this seam fixed: the dev/VCA-cal
// pages are entered from any page (including while the Global overlay is open), so cal can
// be active while on_global is still true. Rotation must do exactly one thing.

#include <unity.h>

#include <pedal_core/encoder_rotate.hpp>

using encoder_ui_detail::RotateAction;
using encoder_ui_detail::rotate_action;

void setUp(void) {}
void tearDown(void) {}

// VCA-cal claims the encoder, even if cal was entered while the overlay is open (it must
// NOT also scroll the overlay pages).
void test_vca_cal_wins_even_in_overlay(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::VcaOffset,
                          (int)rotate_action(/*vca*/true, /*dev*/false, /*global*/true));
    TEST_ASSERT_EQUAL_INT((int)RotateAction::VcaOffset,
                          (int)rotate_action(true, false, false));
}

// Dev-cal swallows rotation (the knobs do the work there) — including when the overlay
// underneath is open.
void test_dev_cal_swallows_rotation(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::None,
                          (int)rotate_action(false, /*dev*/true, /*global*/true));
    TEST_ASSERT_EQUAL_INT((int)RotateAction::None,
                          (int)rotate_action(false, true, false));
}

// An open Global overlay scrolls its pages (the pages themselves are knob-edited).
void test_overlay_scrolls_pages(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::GlobalPage,
                          (int)rotate_action(false, false, /*global*/true));
}

// Default: the normal param screen, where update() dispatches by the highlighted focus.
void test_default_is_normal_screen(void) {
    TEST_ASSERT_EQUAL_INT((int)RotateAction::Normal,
                          (int)rotate_action(false, false, false));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_vca_cal_wins_even_in_overlay);
    RUN_TEST(test_dev_cal_swallows_rotation);
    RUN_TEST(test_overlay_scrolls_pages);
    RUN_TEST(test_default_is_normal_screen);
    return UNITY_END();
}
