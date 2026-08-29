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
#include <cstdio>

// The hardware seam, shipped: the SPI and hal display-pin no-ops display.cpp reaches
// through, and the settable clock its reset pulse and the frame pacing both read. The
// multi-effect's screenshot generator stands the same stack up behind the same header.
#include <pedal_core/host_display.hpp>

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

    // The state the base is holding, read back through the same protected members a
    // product hook reads. A field apply() dropped would otherwise be invisible where it
    // draws nothing on the screen under test — the name cursor, for one.
    ScreenState held() const
    {
        ScreenState s;
        s.screen = m_screen;
        s.focus  = m_focus;
        s.preset = m_preset;
        std::strncpy(s.preset_name,  m_preset_name,  sizeof(s.preset_name)  - 1);
        std::strncpy(s.context_name, m_context_name, sizeof(s.context_name) - 1);
        for (uint8_t i = 0; i < MAX_COLS; ++i) {
            std::strncpy(s.param_name[i], m_param_name[i], sizeof(s.param_name[i]) - 1);
            std::strncpy(s.param_val[i],  m_param_val[i],  sizeof(s.param_val[i])  - 1);
            s.param_bar[i]    = m_param_bar[i];
            s.param_pickup[i] = m_param_pickup[i];
        }
        s.page      = m_page;
        s.num_pages = m_num_pages;
        std::strncpy(s.function, m_function, sizeof(s.function) - 1);
        for (uint8_t i = 0; i < 2u; ++i)
            std::strncpy(s.switch_label[i], m_switch_label[i], sizeof(s.switch_label[i]) - 1);
        s.name_cursor   = m_name_cursor;
        s.save_prompt   = m_save_prompt;
        s.status_badge  = m_status_badge;
        s.scene_badge   = m_scene_badge;
        return s;
    }

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

// One pixel out of that page format: bit (y % 8) of the column's byte.
static bool px(const Frame f, uint8_t x, uint8_t y)
{
    return (f[page_of(y)][x] & (uint8_t)(1u << (y % 8u))) != 0u;
}

// Is any pixel set along a row?
static bool row_has_ink(const Frame f, uint8_t y)
{
    for (uint8_t x = 0; x < OLED_WIDTH; ++x)
        if (px(f, x, y)) return true;
    return false;
}

// Where the focus panel's gauge track sits, spelled out rather than read off the
// compositor: these are the numbers the tick has to land on, so a test that derived them
// the way the code does would agree with any rule the code happened to hold.
static constexpr uint8_t PANEL_TRACK_X0 = 4u;    // draw_gauge(4, ..., OLED_WIDTH - 8, ...)
static constexpr uint8_t PANEL_TRACK_W  = 120u;
static constexpr uint8_t TICK_W         = 2u;
static constexpr uint8_t TICK_TOP       = 57u, TICK_BOTTOM = 58u;

// Both clocks move together: the producers stamp themselves from systick, and update() is
// handed the same instant.
static void at(uint32_t ms) { pedal_core::host_display::set_now_ms(ms); }

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

// A knob still waiting to be picked up keeps its marker when the panel unrolls over the
// grid: the tick is the whole reason the player knows which way to keep turning, and the
// panel is what they are looking at while they turn it.
void test_the_panel_marks_where_the_pot_is_waiting(void) {
    Frame f;
    { TestCompositor c; boot(c, 1000);
      c.show_param_change("Mix", "50%", 0u, PARAM_MID);
      at(1140); c.update(1140); display::capture(f); }        // fully unrolled

    // PARAM_MID lands 58 columns along the 118px travel (120 wide, less the tick's own 2).
    const uint8_t x = (uint8_t)(PANEL_TRACK_X0 + 58u);
    for (uint8_t y = TICK_TOP; y <= TICK_BOTTOM; ++y) {
        TEST_ASSERT_TRUE_MESSAGE(px(f, x, y) && px(f, (uint8_t)(x + 1u), y),
                                 "the panel drew no pickup tick where the pot is pointing");
        TEST_ASSERT_FALSE_MESSAGE(px(f, (uint8_t)(x - 1u), y),
                                  "the pickup tick is wider than it should be");
    }

    // And the gauge still draws, in the four rows the tick left it.
    TEST_ASSERT_TRUE_MESSAGE(row_has_ink(f, 63u), "the panel gauge lost its bottom row");
}

// The tick belongs to a knob being turned into range, so a panel that is not answering one
// leaves those rows to the screen.
void test_a_panel_with_no_pickup_leaves_the_tick_rows_clear(void) {
    Frame f;
    { TestCompositor c; boot(c, 1000);
      c.show_param_change("Mix", "50%", 512u);
      at(1140); c.update(1140); display::capture(f); }

    for (uint8_t y = TICK_TOP; y <= TICK_BOTTOM; ++y)
        TEST_ASSERT_FALSE_MESSAGE(row_has_ink(f, y),
                                  "an unmarked panel drew in the pickup tick's rows");
}

// Both ends of the travel: a pot at rest sits at the track's left edge, and one at full
// scale sits inside its right edge rather than half off the end of it.
void test_the_panel_tick_reaches_both_ends_of_its_track(void) {
    Frame low, high;
    { TestCompositor c; boot(c, 1000);
      c.show_param_change("Mix", "50%", 512u, 0u);
      at(1140); c.update(1140); display::capture(low); }

    { TestCompositor c; boot(c, 1000);
      c.show_param_change("Mix", "50%", 512u, PARAM_MAX);
      at(1140); c.update(1140); display::capture(high); }

    TEST_ASSERT_TRUE_MESSAGE(px(low, PANEL_TRACK_X0, TICK_TOP)
                             && px(low, (uint8_t)(PANEL_TRACK_X0 + 1u), TICK_TOP),
                             "a pot at rest did not sit at the left end of the track");

    // The rightmost column the tick may occupy is the track's own last.
    const uint8_t last = (uint8_t)(PANEL_TRACK_X0 + PANEL_TRACK_W - 1u);
    TEST_ASSERT_TRUE_MESSAGE(px(high, last, TICK_TOP)
                             && px(high, (uint8_t)(last - (TICK_W - 1u)), TICK_TOP),
                             "a pot at full scale did not reach the right end of the track");
    for (uint8_t x = (uint8_t)(last + 1u); x < OLED_WIDTH; ++x)
        TEST_ASSERT_FALSE_MESSAGE(px(high, x, TICK_TOP),
                                  "the pickup tick overhung the end of the track");
}

// The blind reveals the panel a row at a time, and the tick waits for both of its own
// rows: it and the gauge it is read against arrive together, or the reference mark would
// land before the thing it refers to.
void test_the_panel_tick_waits_for_the_blind(void) {
    Frame early, late;
    { TestCompositor c; boot(c, 1000);
      c.show_param_change("Mix", "50%", 512u, PARAM_MID);
      at(1016); c.update(1016); display::capture(early); }     // mid-unroll
    { TestCompositor c; boot(c, 1000);
      c.show_param_change("Mix", "50%", 512u, PARAM_MID);
      at(1140); c.update(1140); display::capture(late); }

    for (uint8_t y = TICK_TOP; y <= TICK_BOTTOM; ++y)
        TEST_ASSERT_FALSE_MESSAGE(row_has_ink(early, y),
                                  "the pickup tick drew before the blind reached its rows");
    TEST_ASSERT_TRUE_MESSAGE(row_has_ink(late, TICK_TOP),
                             "the pickup tick never arrived");
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


// ---------------------------------------------------------------------------
// The screen as one state
// ---------------------------------------------------------------------------

// Every field set to something that is not its default, so a field that fails to arrive
// shows up as itself rather than as a coincidence.
static Compositor::ScreenState sample_state(void) {
    Compositor::ScreenState s;
    s.focus  = Compositor::Focus::Preset;
    s.preset = 42u;
    std::strcpy(s.preset_name,  "Shimmer");
    std::strcpy(s.context_name, "Chorus");
    for (uint8_t i = 0; i < Compositor::MAX_COLS; ++i) {
        std::snprintf(s.param_name[i], sizeof(s.param_name[i]), "Depth%u", (unsigned)i);
        std::snprintf(s.param_val[i],  sizeof(s.param_val[i]),  "%u%%", (unsigned)(10u * i + 5u));
        s.param_bar[i]    = (uint16_t)(100u * i + 50u);
        s.param_pickup[i] = (uint16_t)(100u * i + 90u);
    }
    s.page = 1u; s.num_pages = 3u;
    std::strcpy(s.function, "Tap Tempo");
    std::strcpy(s.switch_label[0], "Bypass");
    std::strcpy(s.switch_label[1], "Freeze");
    s.name_cursor  = 2u;
    s.save_prompt  = true;
    s.status_badge = true;
    s.scene_badge  = true;
    return s;
}

static bool states_match(const Compositor::ScreenState& a, const Compositor::ScreenState& b) {
    if (a.screen != b.screen || a.focus != b.focus || a.preset != b.preset) return false;
    if (std::strcmp(a.preset_name, b.preset_name) != 0) return false;
    if (std::strcmp(a.context_name, b.context_name) != 0) return false;
    for (uint8_t i = 0; i < Compositor::MAX_COLS; ++i) {
        if (std::strcmp(a.param_name[i], b.param_name[i]) != 0) return false;
        if (std::strcmp(a.param_val[i],  b.param_val[i])  != 0) return false;
        if (a.param_bar[i] != b.param_bar[i]) return false;
        if (a.param_pickup[i] != b.param_pickup[i]) return false;
    }
    if (a.page != b.page || a.num_pages != b.num_pages) return false;
    if (std::strcmp(a.function, b.function) != 0) return false;
    for (uint8_t i = 0; i < 2u; ++i)
        if (std::strcmp(a.switch_label[i], b.switch_label[i]) != 0) return false;
    return a.name_cursor == b.name_cursor && a.save_prompt == b.save_prompt
        && a.status_badge == b.status_badge && a.scene_badge == b.scene_badge;
}

// Push the same state the long way round, so the two ways in can be compared.
static void push_by_setters(TestCompositor& c, const Compositor::ScreenState& s) {
    c.set_screen(s.screen);
    c.set_focus(s.focus);
    c.set_preset(s.preset);
    c.set_preset_name(s.preset_name);
    c.set_context_name(s.context_name);
    for (uint8_t i = 0; i < Compositor::MAX_COLS; ++i) {
        c.set_param(i, s.param_name[i], s.param_val[i], s.param_bar[i]);
        c.set_param_pickup(i, s.param_pickup[i]);
    }
    c.set_page(s.page, s.num_pages);
    c.set_function_label(s.function);
    c.set_switch_labels(s.switch_label[0], s.switch_label[1]);
    c.set_name_cursor(s.name_cursor);
    c.set_save_prompt(s.save_prompt);
    c.set_status(s.status_badge);
    c.set_scene(s.scene_badge);
}

// Every field arrives. A field apply() dropped would sit at its default here, including
// the ones that draw nothing on the performance screen.
void test_apply_carries_every_field(void) {
    const Compositor::ScreenState st = sample_state();
    TestCompositor c;
    boot(c, 1000);
    c.apply(st);
    TEST_ASSERT_TRUE_MESSAGE(states_match(c.held(), st), "apply() dropped a field");
}

// The two ways in paint the same screen: one call or fourteen, the pixels are identical.
void test_apply_paints_the_same_frame_as_the_setters(void) {
    const Compositor::ScreenState st = sample_state();
    Frame via_apply, via_setters;

    { TestCompositor c; boot(c, 1000);
      c.apply(st);
      at(1010); c.update(1010); display::capture(via_apply); }

    { TestCompositor c; boot(c, 1000);
      push_by_setters(c, st);
      at(1010); c.update(1010); display::capture(via_setters); }

    TEST_ASSERT_TRUE(frames_match(via_apply, via_setters));
}

// A producer can push the same state every tick and pay for a redraw only when the
// screen has something new to say.
void test_applying_an_unchanged_state_costs_no_redraw(void) {
    Compositor::ScreenState st = sample_state();
    st.screen = 7u;                        // a product screen, so every draw is countable

    TestCompositor c;
    boot(c, 1000);
    c.apply(st);
    at(1010); c.update(1010);
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);

    c.apply(st);                           // the same state again
    at(1011); c.update(1011);              // inside the idle cap: only a change draws
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);

    st.preset = 43u;                       // one field moves
    c.apply(st);
    at(1012); c.update(1012);
    TEST_ASSERT_EQUAL_UINT8(2, c.screens_drawn);
}

// The same holds for the setters underneath, which is what stops apply() and the long
// way round disagreeing about what a change is.
void test_pushing_an_unchanged_value_costs_no_redraw(void) {
    TestCompositor c;
    boot(c, 1000);
    c.set_screen(7u);
    c.set_preset_name("Shimmer");
    c.set_context_name("Chorus");
    at(1010); c.update(1010);
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);

    c.set_preset_name("Shimmer");          // the same words
    c.set_context_name("Chorus");
    c.set_preset(0u);                      // the same slot
    c.set_page(0u, 1u);                    // the same page
    c.set_param(0u, "P1", "--", Compositor::NO_BAR);   // what init() left there
    at(1011); c.update(1011);
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);

    c.set_preset_name("Other");            // and a real change still draws
    at(1012); c.update(1012);
    TEST_ASSERT_EQUAL_UINT8(2, c.screens_drawn);
}

// Two names that differ only past the end of the buffer draw the same pixels, so they
// are the same screen and cost no redraw.
void test_names_differing_past_the_buffer_are_the_same_screen(void) {
    TestCompositor c;
    boot(c, 1000);
    c.set_screen(7u);
    c.set_preset_name("SixteenCharsHere_ONE");   // truncated to sixteen
    at(1010); c.update(1010);
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);

    c.set_preset_name("SixteenCharsHere_TWO");   // same sixteen, different tail
    at(1011); c.update(1011);
    TEST_ASSERT_EQUAL_UINT8(1, c.screens_drawn);
}

// A table of screens, each drawing its own frame — the shape a suite takes once the
// screen is a value rather than a script of calls.
void test_a_table_of_screens_each_draws_its_own(void) {
    Compositor::ScreenState table[4];
    table[0] = sample_state();
    table[1] = sample_state(); table[1].preset = 7u;
                               std::strcpy(table[1].preset_name, "Second");
    table[2] = sample_state(); table[2].status_badge = false;
                               table[2].scene_badge  = false;
    table[3] = sample_state(); table[3].focus = Compositor::Focus::Algo;
                               table[3].page = 2u;

    Frame frames[4];
    for (uint8_t i = 0; i < 4u; ++i) {
        TestCompositor c;
        boot(c, 1000);
        c.apply(table[i]);
        at(1010); c.update(1010);
        display::capture(frames[i]);
        TEST_ASSERT_TRUE(frame_has_ink(frames[i]));
    }

    for (uint8_t i = 0; i < 4u; ++i)
        for (uint8_t j = (uint8_t)(i + 1u); j < 4u; ++j)
            TEST_ASSERT_FALSE_MESSAGE(frames_match(frames[i], frames[j]),
                                      "two states drew the same frame");
}


// The seam's own contract. display::init() spends time on the controller's reset pulse,
// so a host program that pinned the clock at zero would freeze the splash and the save
// animation on their first frame -- both timestamp themselves from here. This keeps the
// header's warning true rather than merely written down.
void test_the_shipped_clock_advances_over_the_reset_pulse(void) {
    pedal_core::host_display::set_now_ms(0u);
    TestCompositor c;
    c.init();
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0u, pedal_core::host_display::now_ms(),
        "display::init() spent no time: a host pinning the clock would freeze animations");

    pedal_core::host_display::set_now_ms(1000u);
    TEST_ASSERT_EQUAL_UINT32(1016u, pedal_core::host_display::advance_ms(16u));
    TEST_ASSERT_EQUAL_UINT32(1016u, pedal_core::host_display::now_ms());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_state_change_reaches_the_framebuffer);
    RUN_TEST(test_a_product_screen_replaces_the_performance_grid);
    RUN_TEST(test_the_switch_labels_draw_on_the_bottom_row);
    RUN_TEST(test_a_transient_suppresses_the_switch_labels);
    RUN_TEST(test_the_focus_panel_leaves_the_header_alone);
    RUN_TEST(test_the_panel_marks_where_the_pot_is_waiting);
    RUN_TEST(test_a_panel_with_no_pickup_leaves_the_tick_rows_clear);
    RUN_TEST(test_the_panel_tick_reaches_both_ends_of_its_track);
    RUN_TEST(test_the_panel_tick_waits_for_the_blind);
    RUN_TEST(test_the_splash_animates_then_gives_the_screen_up);
    RUN_TEST(test_a_faulted_boot_runs_the_fault_then_the_splash);
    RUN_TEST(test_the_save_animation_owns_the_screen);
    RUN_TEST(test_a_slide_settles_on_its_destination);
    RUN_TEST(test_a_slide_under_a_splash_is_refused);
    RUN_TEST(test_apply_carries_every_field);
    RUN_TEST(test_apply_paints_the_same_frame_as_the_setters);
    RUN_TEST(test_applying_an_unchanged_state_costs_no_redraw);
    RUN_TEST(test_pushing_an_unchanged_value_costs_no_redraw);
    RUN_TEST(test_names_differing_past_the_buffer_are_the_same_screen);
    RUN_TEST(test_a_table_of_screens_each_draws_its_own);
    RUN_TEST(test_the_shipped_clock_advances_over_the_reset_pulse);
    return UNITY_END();
}
