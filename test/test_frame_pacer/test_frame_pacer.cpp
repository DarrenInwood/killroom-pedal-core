// Host-native unit tests for the frame pacer (include/pedal_core/ui/frame_pacer.hpp).
//
// The pacer decides whether the screen redraws this tick and what belongs on the frame
// when it does. It knows time and nothing else — every entry point is given `now`, and
// whether the display is still flushing arrives as a bool — so the whole pacing machine
// runs here with no framebuffer, no driver and no fakes.
//
// The timings under test are the pacer's own constants plus DISPLAY_PARAM_SHOW_MS from
// the product config, which test/support pins at 750 ms.

#include <unity.h>
#include <cstdint>

#include <pedal_core/ui/frame_pacer.hpp>

using pedal_core::ui::FramePacer;
using What = FramePacer::What;
using Overlay = FramePacer::Overlay;
using SlideStart = FramePacer::SlideStart;

static constexpr uint32_t REFRESH_MS    = 50u;   // the idle cap, 20 fps
static constexpr uint32_t ANIM_FRAME_MS = 16u;   // the animating cadence
static constexpr uint32_t ANIM_MS       = 140u;  // the enter/exit ramp
static constexpr uint32_t SPLASH_MS     = 2000u;
static constexpr uint32_t FAULT_HOLD_MS = 2500u;
static constexpr uint32_t DWELL_MS      = DISPLAY_PARAM_SHOW_MS;

// A pacer that has just drawn at t=1000, so tests start from a known idle rather than
// from the boot state where everything is due at once.
static FramePacer settled(uint32_t& t)
{
    FramePacer p;
    t = 1000u;
    const FramePacer::Decision d = p.decide(t, false);   // the boot frame
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)d.what);
    return p;
}

void setUp(void) {}
void tearDown(void) {}

// Nothing has changed, so nothing is drawn until the idle cap comes round.
void test_idle_holds_the_frame_until_the_cap(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + 1u, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + REFRESH_MS - 1u, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame,   (int)p.decide(t + REFRESH_MS, false).what);
}

// A change is drawn on the next tick, without waiting for the cap.
void test_a_change_draws_at_once(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + 1u, false).what);
    p.changed();
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)p.decide(t + 2u, false).what);
}

// The display is still flushing the last frame, so this one waits — and the change is
// still owed once it clears. This is the branch a real pedal never takes, because
// display::update_busy() is always false there.
void test_a_busy_display_defers_the_frame_and_keeps_it_owed(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.changed();
    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + 2u, true).what);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + 3u, true).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame,   (int)p.decide(t + 4u, false).what);
}

// A busy display defers the idle redraw too, rather than losing the tick it was due on.
void test_a_busy_display_defers_the_idle_redraw(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + REFRESH_MS, true).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame,   (int)p.decide(t + REFRESH_MS, false).what);
}

// The panel unrolls, sits open, rolls up, and hands the screen back.
void test_the_panel_unrolls_dwells_and_hands_the_screen_back(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.overlay(Overlay::Panel, t);

    const FramePacer::Decision open = p.decide(t, false);
    TEST_ASSERT_EQUAL_INT((int)What::FramePanel, (int)open.what);
    TEST_ASSERT_EQUAL_UINT32(0u, open.arg);                    // the ramp starts closed

    const FramePacer::Decision mid = p.decide(t + ANIM_MS / 2u, false);
    TEST_ASSERT_EQUAL_INT((int)What::FramePanel, (int)mid.what);
    TEST_ASSERT_EQUAL_UINT32(128u, mid.arg);                   // halfway up

    const FramePacer::Decision full = p.decide(t + ANIM_MS, false);
    TEST_ASSERT_EQUAL_UINT32(256u, full.arg);                  // fully open

    // The roll-up runs in the last ANIM_MS of the dwell.
    const FramePacer::Decision closing = p.decide(t + DWELL_MS - ANIM_MS / 2u, false);
    TEST_ASSERT_EQUAL_INT((int)What::FramePanel, (int)closing.what);
    TEST_ASSERT_EQUAL_UINT32(128u, closing.arg);

    // Past the dwell the transient is gone and the screen underneath is drawn.
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)p.decide(t + DWELL_MS, false).what);
}

// While the panel animates, frames come at the animation cadence rather than the idle cap.
void test_an_animating_overlay_runs_at_the_animation_cadence(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.overlay(Overlay::Panel, t);
    p.decide(t, false);                                        // the opening frame
    TEST_ASSERT_EQUAL_INT((int)What::Nothing,    (int)p.decide(t + ANIM_FRAME_MS - 1u, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::FramePanel, (int)p.decide(t + ANIM_FRAME_MS, false).what);
}

// Mid-dwell the panel is not animating, so it idles at the slower cap.
void test_a_settled_overlay_idles_at_the_slower_cap(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.overlay(Overlay::Panel, t);
    const uint32_t settled_at = t + ANIM_MS + 10u;
    p.decide(settled_at, false);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing,    (int)p.decide(settled_at + ANIM_FRAME_MS, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::FramePanel, (int)p.decide(settled_at + REFRESH_MS, false).what);
}

// A stream of knob updates refreshes the value and the dwell without replaying the
// unroll: the panel is already open, so re-opening it starts the clock past the ramp.
void test_reopening_the_panel_does_not_replay_the_unroll(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.overlay(Overlay::Panel, t);
    TEST_ASSERT_EQUAL_UINT32(256u, p.decide(t + ANIM_MS, false).arg);

    const uint32_t again = t + 300u;
    p.overlay(Overlay::Panel, again);
    const FramePacer::Decision d = p.decide(again, false);
    TEST_ASSERT_EQUAL_INT((int)What::FramePanel, (int)d.what);
    TEST_ASSERT_EQUAL_UINT32(256u, d.arg);                     // stays open, does not reopen

    // The dwell restarted from the re-open, less the ramp it skipped.
    TEST_ASSERT_EQUAL_INT((int)What::FramePanel, (int)p.decide(again + DWELL_MS - ANIM_MS - 1u, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame,      (int)p.decide(again + DWELL_MS - ANIM_MS, false).what);
}

// A banner is paced exactly as the panel is; only what gets drawn differs.
void test_a_banner_is_paced_like_the_panel(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.overlay(Overlay::Banner, t);
    TEST_ASSERT_EQUAL_INT((int)What::FrameBanner, (int)p.decide(t, false).what);
    TEST_ASSERT_EQUAL_UINT32(256u, p.decide(t + ANIM_MS, false).arg);
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)p.decide(t + DWELL_MS, false).what);
}

// The save animation owns the whole screen for its own, longer total, and animates
// throughout — it carries elapsed time rather than a ramp position.
void test_the_save_confirmation_is_a_still_frame_held_for_the_dwell(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.overlay(Overlay::Save, t);

    // A still frame carries no progress, so nothing rides along to draw it with.
    const FramePacer::Decision d = p.decide(t + 200u, false);
    TEST_ASSERT_EQUAL_INT((int)What::FrameSave, (int)d.what);
    TEST_ASSERT_EQUAL_UINT32(0u, d.arg);

    // And it never asks for the animating cadence: once drawn, the next frame is the idle
    // one, so holding SAVED on the glass costs no redraws at all.
    p.decide(t + 300u, false);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing,   (int)p.decide(t + 300u + ANIM_FRAME_MS, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::FrameSave, (int)p.decide(t + 300u + REFRESH_MS, false).what);

    // It hands the screen back on the same dwell every other transient uses.
    TEST_ASSERT_EQUAL_INT((int)What::FrameSave, (int)p.decide(t + DWELL_MS - 1u, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame,     (int)p.decide(t + DWELL_MS, false).what);
}

// The splash animates for its hold and then gives the screen up.
void test_the_splash_animates_for_its_hold(void) {
    uint32_t t = 1000u;
    FramePacer p;
    p.splash(t);   // the caller has already put the first frame up

    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t, false).what);
    const FramePacer::Decision d = p.decide(t + ANIM_FRAME_MS, false);
    TEST_ASSERT_EQUAL_INT((int)What::SplashFrame, (int)d.what);
    TEST_ASSERT_EQUAL_UINT32(ANIM_FRAME_MS, d.arg);            // ms since it began

    TEST_ASSERT_EQUAL_INT((int)What::SplashFrame, (int)p.decide(t + SPLASH_MS - 1u, false).what);
    // Expired: the screen underneath is owed a frame.
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)p.decide(t + SPLASH_MS, false).what);
}

// Nothing underneath is drawn while the splash holds the screen, however dirty it is.
void test_the_splash_suppresses_the_screen_underneath(void) {
    uint32_t t = 1000u;
    FramePacer p;
    p.splash(t);
    p.changed();
    p.changed();
    TEST_ASSERT_EQUAL_INT((int)What::SplashFrame, (int)p.decide(t + ANIM_FRAME_MS, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame,       (int)p.decide(t + SPLASH_MS, false).what);
}

// A faulted boot runs Storage Fault -> Splash -> UI. The fault is a static hold: it draws
// nothing while it is up, and the splash follows it rather than the UI.
void test_a_faulted_boot_runs_the_fault_then_the_splash(void) {
    uint32_t t = 1000u;
    FramePacer p;
    p.fault_hold(t);   // the caller has already drawn the warning

    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + ANIM_FRAME_MS, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing, (int)p.decide(t + FAULT_HOLD_MS - 1u, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::SplashRestart, (int)p.decide(t + FAULT_HOLD_MS, false).what);

    // The caller answers a restart by running the splash, which then holds in its own right.
    const uint32_t splash_at = t + FAULT_HOLD_MS;
    p.splash(splash_at);
    TEST_ASSERT_EQUAL_INT((int)What::SplashFrame, (int)p.decide(splash_at + ANIM_FRAME_MS, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame,       (int)p.decide(splash_at + SPLASH_MS, false).what);
}

// The restart is offered once. A second pass past the expiry is an ordinary frame.
void test_the_fault_hold_queues_the_splash_only_once(void) {
    uint32_t t = 1000u;
    FramePacer p;
    p.fault_hold(t);
    TEST_ASSERT_EQUAL_INT((int)What::SplashRestart, (int)p.decide(t + FAULT_HOLD_MS, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)p.decide(t + FAULT_HOLD_MS + 1u, false).what);
}

// A transition renders its destination, steps across, and settles on it.
void test_a_slide_captures_steps_and_settles(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    TEST_ASSERT_EQUAL_INT((int)SlideStart::Ready, (int)p.slide());

    // The tick that starts the slide captures the destination and draws the first step.
    const FramePacer::Decision first = p.decide(t + REFRESH_MS, false);
    TEST_ASSERT_TRUE(first.capture_slide_target);
    TEST_ASSERT_EQUAL_INT((int)What::SlideStep, (int)first.what);
    TEST_ASSERT_EQUAL_UINT32(0u, first.arg);

    const uint32_t began = t + REFRESH_MS;
    const FramePacer::Decision step = p.decide(began + ANIM_FRAME_MS, false);
    TEST_ASSERT_FALSE(step.capture_slide_target);              // captured once, not per step
    TEST_ASSERT_EQUAL_INT((int)What::SlideStep, (int)step.what);
    TEST_ASSERT_EQUAL_UINT32(ANIM_FRAME_MS, step.arg);         // ms into the transition

    TEST_ASSERT_EQUAL_INT((int)What::SlideSettle, (int)p.decide(began + FramePacer::SLIDE_MS, false).what);
    // Settling leaves the screen owed a frame of its own.
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)p.decide(began + FramePacer::SLIDE_MS + 1u, false).what);
}

// A transition steps at the animation cadence, not the idle cap.
void test_a_slide_steps_at_the_animation_cadence(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.slide();
    const uint32_t began = t + REFRESH_MS;
    p.decide(began, false);
    TEST_ASSERT_EQUAL_INT((int)What::Nothing,   (int)p.decide(began + ANIM_FRAME_MS - 1u, false).what);
    TEST_ASSERT_EQUAL_INT((int)What::SlideStep, (int)p.decide(began + ANIM_FRAME_MS, false).what);
}

// An open transient is closed before the transition captures the frame it is leaving,
// so the panel does not animate as part of the slide and reappear over what it landed on.
void test_a_slide_closes_an_open_transient_first(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.overlay(Overlay::Panel, t);
    p.decide(t, false);
    TEST_ASSERT_EQUAL_INT((int)SlideStart::RedrawFirst, (int)p.slide());
    // The transient is gone: what follows the transition is the plain screen.
    const uint32_t began = t + REFRESH_MS;
    p.decide(began, false);
    p.decide(began + FramePacer::SLIDE_MS, false);             // settle
    TEST_ASSERT_EQUAL_INT((int)What::Frame, (int)p.decide(began + FramePacer::SLIDE_MS + 1u, false).what);
}

// A splash owns the screen, so a transition under it is refused rather than queued.
void test_a_slide_is_refused_under_a_splash(void) {
    uint32_t t = 1000u;
    FramePacer p;
    p.splash(t);
    TEST_ASSERT_EQUAL_INT((int)SlideStart::Refused, (int)p.slide());
    // Once the splash is over, a transition is accepted again.
    p.decide(t + SPLASH_MS, false);
    TEST_ASSERT_EQUAL_INT((int)SlideStart::Ready, (int)p.slide());
}

// A second transition while one is running is refused, so the "from" frame it captured
// cannot be overwritten mid-flight.
void test_a_slide_is_refused_while_one_is_running(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    TEST_ASSERT_EQUAL_INT((int)SlideStart::Ready, (int)p.slide());
    TEST_ASSERT_EQUAL_INT((int)SlideStart::Refused, (int)p.slide());   // still pending
    const uint32_t began = t + REFRESH_MS;
    p.decide(began, false);
    TEST_ASSERT_EQUAL_INT((int)SlideStart::Refused, (int)p.slide());   // now active
    p.decide(began + FramePacer::SLIDE_MS, false);
    TEST_ASSERT_EQUAL_INT((int)SlideStart::Ready, (int)p.slide());     // settled
}

// A transition outranks the overlays and the ordinary redraw while it runs.
void test_a_slide_outranks_a_pending_change(void) {
    uint32_t t = 0; FramePacer p = settled(t);
    p.slide();
    p.changed();
    const FramePacer::Decision d = p.decide(t + REFRESH_MS, false);
    TEST_ASSERT_EQUAL_INT((int)What::SlideStep, (int)d.what);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_holds_the_frame_until_the_cap);
    RUN_TEST(test_a_change_draws_at_once);
    RUN_TEST(test_a_busy_display_defers_the_frame_and_keeps_it_owed);
    RUN_TEST(test_a_busy_display_defers_the_idle_redraw);
    RUN_TEST(test_the_panel_unrolls_dwells_and_hands_the_screen_back);
    RUN_TEST(test_an_animating_overlay_runs_at_the_animation_cadence);
    RUN_TEST(test_a_settled_overlay_idles_at_the_slower_cap);
    RUN_TEST(test_reopening_the_panel_does_not_replay_the_unroll);
    RUN_TEST(test_a_banner_is_paced_like_the_panel);
    RUN_TEST(test_the_save_confirmation_is_a_still_frame_held_for_the_dwell);
    RUN_TEST(test_the_splash_animates_for_its_hold);
    RUN_TEST(test_the_splash_suppresses_the_screen_underneath);
    RUN_TEST(test_a_faulted_boot_runs_the_fault_then_the_splash);
    RUN_TEST(test_the_fault_hold_queues_the_splash_only_once);
    RUN_TEST(test_a_slide_captures_steps_and_settles);
    RUN_TEST(test_a_slide_steps_at_the_animation_cadence);
    RUN_TEST(test_a_slide_closes_an_open_transient_first);
    RUN_TEST(test_a_slide_is_refused_under_a_splash);
    RUN_TEST(test_a_slide_is_refused_while_one_is_running);
    RUN_TEST(test_a_slide_outranks_a_pending_change);
    return UNITY_END();
}
