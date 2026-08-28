// Host-native unit tests for the family compositor (src/ui/compositor.cpp).
//
// The compositor draws; the frame pacer decides when (test_frame_pacer covers that). This
// suite is about pixels: what actually lands in the framebuffer for a given state, and
// which parts of the screen a transient is allowed to touch.
//
// Like the other driver suites, the real implementation is compiled into this TU via
// #include behind no-op SPI and hal stubs, so the framebuffer under assertion is the
// firmware's ACTUAL pixel output. display.cpp comes first because compositor.cpp draws
// through it; both are file-static inside, and display::capture() is how a frame is read
// back out.
//
// The compositor is abstract — a product supplies its own screens and splash art — so the
// suite drives it through a recording subclass, which is also how it counts the hooks the
// pacing decisions are supposed to reach.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include <pedal_core/hal.hpp>

// --- Stub the SPI and hal pin calls display.cpp makes (no-ops). systick comes from the
// shared systick_fake in test/support. ---------------------------------------
namespace spi {
    void write(const uint8_t*, uint16_t) {}
    void transfer(const uint8_t*, uint8_t*, uint16_t) {}
}
namespace pedal_core::hal {
    void display_pins_init() {}
    void display_cs(bool) {}
    void display_dc_data(bool) {}
    void display_reset(bool) {}
    void display_power(bool) {}
}
namespace systick {
    void fake_set_ms(uint32_t ms);
}

#include "../../src/display.cpp"
#include "../../src/ui/compositor.cpp"

using pedal_core::ui::Compositor;
using pedal_core::ui::FramePacer;

// A product's compositor, reduced to what the base needs from one: a screen of its own
// and some splash art. Both record, so a test can tell that a decision reached the hook.
class TestCompositor : public Compositor {
public:
    TestCompositor() : Compositor(LayoutSpec{ 4u, 32u, 0u }) {}

    uint8_t  screens_drawn  = 0;
    uint8_t  last_screen_id = 0;
    uint8_t  splash_frames  = 0;
    uint32_t last_splash_elapsed = 0;

protected:
    void draw_screen(uint8_t screen_id) override
    {
        ++screens_drawn;
        last_screen_id = screen_id;
        display::draw_text(0, 0, "PRODUCT");
    }

    void draw_splash_art(uint32_t elapsed) override
    {
        ++splash_frames;
        last_splash_elapsed = elapsed;
        display::fill_rect(0, 20, 40, 8, true);
    }
};

using Frame = uint8_t[OLED_PAGES][OLED_WIDTH];
static constexpr size_t FRAME_BYTES = sizeof(uint8_t) * OLED_PAGES * OLED_WIDTH;

static bool frames_match(const Frame a, const Frame b) { return memcmp(a, b, FRAME_BYTES) == 0; }

static bool frame_has_ink(const Frame f)
{
    for (uint8_t p = 0; p < OLED_PAGES; ++p)
        for (uint8_t c = 0; c < OLED_WIDTH; ++c)
            if (f[p][c] != 0u) return true;
    return false;
}

// The display page a pixel row falls in; the framebuffer is page-format, 8 rows apiece.
static constexpr uint8_t page_of(uint8_t y) { return (uint8_t)(y / 8u); }

// Both clocks move together: the producers stamp themselves from systick, and update() is
// handed the same instant.
static void at(uint32_t ms) { systick::fake_set_ms(ms); }

// Bring a compositor up and put both clocks on `now`. The clock is set AFTER init()
// because display::init() spends fake milliseconds on the controller's reset pulse, and a
// producer stamped from a clock update() never catches up to would time out on its first
// tick.
static void boot(TestCompositor& c, uint32_t now) { c.init(); at(now); }

void setUp(void) {}
void tearDown(void) {}

// A change to the state the frame draws from reaches the glass.
void test_a_state_change_reaches_the_framebuffer(void) {
    TestCompositor c;
    boot(c, 1000);

    Frame blank; display::capture(blank);
    TEST_ASSERT_FALSE(frame_has_ink(blank));      // init() leaves the screen clear

    c.set_context_name("Chorus");
    c.set_preset_name("Shimmer");
    at(1010); c.update(1010);

    Frame drawn; display::capture(drawn);
    TEST_ASSERT_TRUE(frame_has_ink(drawn));
}

// A product screen replaces the performance grid wholesale.
void test_a_product_screen_replaces_the_performance_grid(void) {
    TestCompositor c;
    boot(c, 1000);
    c.set_screen(7u);
    at(1010); c.update(1010);

    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);
    TEST_ASSERT_EQUAL_UINT8(7u, c.last_screen_id);
    TEST_ASSERT_EQUAL_UINT8(7u, c.screen());
}

// The switch labels land on the bottom row, where each sits over the switch it names.
void test_the_switch_labels_draw_on_the_bottom_row(void) {
    Frame with_labels, without_labels;

    { TestCompositor c; boot(c, 1000);
      c.set_switch_labels("Tap Tempo", "Freeze");
      at(1010); c.update(1010); display::capture(with_labels); }

    { TestCompositor c; boot(c, 1000);
      c.set_switch_labels("", "");
      at(1010); c.update(1010); display::capture(without_labels); }

    TEST_ASSERT_FALSE(frames_match(with_labels, without_labels));

    // The difference is confined to the row the labels own.
    const uint8_t hint_page = page_of(58u);   // HINT_Y
    for (uint8_t p = 0; p < OLED_PAGES; ++p) {
        if (p == hint_page) continue;
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(with_labels[p], without_labels[p], OLED_WIDTH,
                                         "a switch label drew outside the bottom row");
    }
}

// A transient owns the bottom row while it is up, so the labels stand down rather than
// showing through the part of the screen the panel has not reached yet. Sampled early in
// the unroll, where the blind covers only the top of its band — at full extension it
// would cover the row anyway and this would prove nothing.
void test_a_transient_suppresses_the_switch_labels(void) {
    Frame panel_with_labels, panel_without_labels;

    { TestCompositor c; boot(c, 1000);
      c.set_switch_labels("Tap Tempo", "Freeze");
      c.show_param_change("Mix", "50%", 512u);
      at(1016); c.update(1016); display::capture(panel_with_labels); }

    { TestCompositor c; boot(c, 1000);
      c.set_switch_labels("", "");
      c.show_param_change("Mix", "50%", 512u);
      at(1016); c.update(1016); display::capture(panel_without_labels); }

    TEST_ASSERT_TRUE_MESSAGE(frames_match(panel_with_labels, panel_without_labels),
                             "the switch labels showed through under an open transient");

    // The sample really is mid-unroll: the bottom row is still the screen underneath.
    Frame plain;
    { TestCompositor c; boot(c, 1000);
      c.set_switch_labels("", "");
      at(1016); c.update(1016); display::capture(plain); }
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(plain[page_of(58u)], panel_without_labels[page_of(58u)],
                                     OLED_WIDTH,
                                     "the panel already covered the bottom row at this progress");
}

// The panel unrolls over the grid and leaves the header and context row in place, so an
// edit never costs the player their bearings.
void test_the_focus_panel_leaves_the_header_alone(void) {
    Frame plain, with_panel;

    { TestCompositor c; boot(c, 1000);
      c.set_context_name("Chorus"); c.set_preset_name("Shimmer");
      at(1010); c.update(1010); display::capture(plain); }

    { TestCompositor c; boot(c, 1000);
      c.set_context_name("Chorus"); c.set_preset_name("Shimmer");
      c.show_param_change("Mix", "50%", 512u);
      at(1140); c.update(1140); display::capture(with_panel); }   // fully unrolled

    // Everything above the panel's separator rule is untouched.
    for (uint8_t p = 0; p < page_of(27u); ++p)
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(plain[p], with_panel[p], OLED_WIDTH,
                                         "the panel painted above its band");
    // And the band itself is not what was underneath.
    TEST_ASSERT_FALSE(frames_match(plain, with_panel));
}

// The splash puts its first frame up at once and the superloop animates the rest.
void test_the_splash_animates_then_gives_the_screen_up(void) {
    TestCompositor c;
    boot(c, 1000);
    c.set_preset_name("Shimmer");

    c.show_splash();
    TEST_ASSERT_EQUAL_UINT8(1, c.splash_frames);          // drawn and flushed immediately
    TEST_ASSERT_EQUAL_UINT32(0u, c.last_splash_elapsed);

    at(1016); c.update(1016);
    TEST_ASSERT_EQUAL_UINT8(2, c.splash_frames);
    TEST_ASSERT_EQUAL_UINT32(16u, c.last_splash_elapsed);

    // Past the hold the screen underneath comes back, and the art is done.
    at(3000); c.update(3000);
    TEST_ASSERT_EQUAL_UINT8(2, c.splash_frames);
    Frame after; display::capture(after);
    TEST_ASSERT_TRUE(frame_has_ink(after));
}

// A faulted boot runs Storage Fault, then the splash, then the UI — never straight to it.
void test_a_faulted_boot_runs_the_fault_then_the_splash(void) {
    TestCompositor c;
    boot(c, 1000);

    c.show_storage_fault();
    Frame fault; display::capture(fault);
    TEST_ASSERT_TRUE(frame_has_ink(fault));               // the warning is up
    TEST_ASSERT_EQUAL_UINT8(0, c.splash_frames);

    // A static hold: nothing redraws underneath it.
    at(1500); c.update(1500);
    Frame held; display::capture(held);
    TEST_ASSERT_TRUE(frames_match(fault, held));
    TEST_ASSERT_EQUAL_UINT8(0, c.splash_frames);

    // When it expires the splash follows, rather than the UI.
    at(3500); c.update(3500);
    TEST_ASSERT_EQUAL_UINT8(1, c.splash_frames);
}

// The save animation owns the whole screen: no product screen is drawn under it.
void test_the_save_animation_owns_the_screen(void) {
    TestCompositor c;
    boot(c, 1000);
    c.set_screen(7u);
    at(1010); c.update(1010);
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);

    c.show_saved();
    at(1100); c.update(1100);
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);          // the product screen stood down

    Frame saving; display::capture(saving);
    TEST_ASSERT_TRUE(frame_has_ink(saving));

    // SAVING becomes SAVED partway through, so the frame changes without the state doing.
    at(1700); c.update(1700);
    Frame saved; display::capture(saved);
    TEST_ASSERT_FALSE(frames_match(saving, saved));

    // And the screen comes back at the end of the animation.
    at(2300); c.update(2300);
    TEST_ASSERT_EQUAL_UINT8(2, c.screens_drawn);
}

// A transition settles on exactly the frame the destination state renders on its own.
void test_a_slide_settles_on_its_destination(void) {
    Frame destination;
    { TestCompositor ref; boot(ref, 1000);
      ref.set_preset_name("Second"); ref.set_context_name("Flanger");
      at(1010); ref.update(1010); display::capture(destination); }

    TestCompositor c;
    boot(c, 1000);
    c.set_preset_name("First");
    c.set_context_name("Chorus");
    at(1010); c.update(1010);

    // begin_slide captures the screen being left, before the state moves on.
    c.begin_slide(1);
    c.set_preset_name("Second");
    c.set_context_name("Flanger");

    at(1060); c.update(1060);                             // render the destination, first step
    Frame mid; display::capture(mid);
    TEST_ASSERT_FALSE_MESSAGE(frames_match(mid, destination), "the slide skipped to the end");

    for (uint32_t t = 1076u; t < 1210u; t += 16u) { at(t); c.update(t); }
    at(1210); c.update(1210);                             // past SLIDE_MS: settle

    Frame settled; display::capture(settled);
    TEST_ASSERT_TRUE_MESSAGE(frames_match(settled, destination),
                             "the slide did not settle on the destination frame");
}

// A splash owns the screen, so a transition under it is refused rather than queued behind.
void test_a_slide_under_a_splash_is_refused(void) {
    TestCompositor c;
    boot(c, 1000);
    c.set_preset_name("First");
    at(1010); c.update(1010);

    c.show_splash();
    Frame splash; display::capture(splash);

    c.begin_slide(1);
    at(1030); c.update(1030);                             // still the splash, not a slide
    TEST_ASSERT_EQUAL_UINT8(2, c.splash_frames);

    at(3100); c.update(3100);
    Frame after; display::capture(after);
    TEST_ASSERT_FALSE(frames_match(splash, after));       // straight to the UI, no transition
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_state_change_reaches_the_framebuffer);
    RUN_TEST(test_a_product_screen_replaces_the_performance_grid);
    RUN_TEST(test_the_switch_labels_draw_on_the_bottom_row);
    RUN_TEST(test_a_transient_suppresses_the_switch_labels);
    RUN_TEST(test_the_focus_panel_leaves_the_header_alone);
    RUN_TEST(test_the_splash_animates_then_gives_the_screen_up);
    RUN_TEST(test_a_faulted_boot_runs_the_fault_then_the_splash);
    RUN_TEST(test_the_save_animation_owns_the_screen);
    RUN_TEST(test_a_slide_settles_on_its_destination);
    RUN_TEST(test_a_slide_under_a_splash_is_refused);
    return UNITY_END();
}
