// Host-native unit tests for the display driver (drivers/display.cpp).
//
// display.cpp talks to the SPI bus + the hal pins + systick, so — like the other
// driver tests — we compile the real driver into this TU via #include behind
// no-op SPI and hal stubs. The drawing
// primitives operate on the file-static framebuffer s_fb, which is the firmware's
// ACTUAL pixel output. They are controller-agnostic, so this exercises the same
// code path for every DISPLAY_* backend; the default (un-flagged) build selects
// SSD1309. These tests lock the real C++ glyph layout, double-height expansion,
// line/rect/pixel math, and bounds guards so a refactor that diverges from the
// tools/render_display.py simulator is caught at build time.

#include <unity.h>
#include <cstdint>

#include <pedal_core/hal.hpp>

// --- Stub the SPI and hal pin calls display.cpp makes (no-ops). systick comes
// from the shared systick_fake in test/support. ------------------------------
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

// Pull in the implementation under test (after the stubs it depends on).
// FONT_5X8, s_fb, expand_lo/expand_hi are file-static — in scope below.
#include "../../src/display.cpp"

void setUp(void) { display::clear(); }
void tearDown(void) {}

// clear() zeroes every byte of the framebuffer.
void test_clear_zeroes_framebuffer(void) {
    display::set_pixel(10, 10, true);
    display::clear();
    for (uint8_t p = 0; p < OLED_PAGES; ++p)
        for (uint8_t c = 0; c < OLED_WIDTH; ++c)
            TEST_ASSERT_EQUAL_UINT8(0, s_fb[p][c]);
}

// draw_char lays the 5 glyph columns, a blank spacing column, and advances by 6.
void test_draw_char_glyph_gap_advance(void) {
    uint8_t nx = display::draw_char(0, 0, 'A');
    const uint8_t* g = FONT_5X8['A' - 0x20];
    for (uint8_t col = 0; col < 5; ++col)
        TEST_ASSERT_EQUAL_UINT8(g[col], s_fb[0][col]);
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][5]);   // spacing gap
    TEST_ASSERT_EQUAL_UINT8(6, nx);              // CHAR_WIDTH advance
}

// Out-of-range characters render as '?'.
void test_draw_char_non_printable_maps_to_question(void) {
    display::draw_char(0, 0, '\x01');            // below 0x20
    const uint8_t* q = FONT_5X8['?' - 0x20];
    for (uint8_t col = 0; col < 5; ++col)
        TEST_ASSERT_EQUAL_UINT8(q[col], s_fb[0][col]);
}

// page >= OLED_PAGES is a no-op that returns x unchanged and draws nothing.
void test_draw_char_page_guard(void) {
    uint8_t nx = display::draw_char(3, OLED_PAGES, 'A');
    TEST_ASSERT_EQUAL_UINT8(3, nx);
    for (uint8_t p = 0; p < OLED_PAGES; ++p)
        for (uint8_t c = 0; c < OLED_WIDTH; ++c)
            TEST_ASSERT_EQUAL_UINT8(0, s_fb[p][c]);
}

// draw_text places successive glyphs 6 px apart.
void test_draw_text_lays_out_chars(void) {
    display::draw_text(0, 1, "Hi");
    TEST_ASSERT_EQUAL_UINT8(FONT_5X8['H' - 0x20][0], s_fb[1][0]);
    TEST_ASSERT_EQUAL_UINT8(FONT_5X8['i' - 0x20][0], s_fb[1][6]);
}

// set_pixel targets the right page/bit and clears cleanly; OOB coords are safe.
void test_set_pixel_sets_and_clears_bit(void) {
    display::set_pixel(5, 9, true);              // page 1, bit (9&7)=1
    TEST_ASSERT_EQUAL_UINT8(1u << 1, s_fb[1][5]);
    display::set_pixel(5, 9, false);
    TEST_ASSERT_EQUAL_UINT8(0, s_fb[1][5]);
    display::set_pixel(OLED_WIDTH, 0, true);     // out of range — must not crash
    display::set_pixel(0, OLED_HEIGHT, true);
}

// draw_hline sets one row's bit across the whole width; OOB row is a no-op.
void test_draw_hline_sets_row(void) {
    display::draw_hline(8);                       // page 1, bit 0
    for (uint8_t c = 0; c < OLED_WIDTH; ++c)
        TEST_ASSERT_EQUAL_UINT8(1u << 0, s_fb[1][c]);
    display::draw_hline(OLED_HEIGHT);             // out of range — must not crash
}

// invert_rect XORs the region; applying it twice restores the original.
void test_invert_rect_xors_region(void) {
    display::invert_rect(2, 0, 3, 1);            // x 2..4, page 0
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][2]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][5]);   // outside the rect
    display::invert_rect(2, 0, 3, 1);
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][2]);
}

// Variable-width font: draw_glyph blits a glyph's columns at an arbitrary y (not
// page-aligned) and returns x + the glyph's pen advance; text_width sums advances.
void test_var_font_glyph_and_width(void) {
    // Minimal 1-glyph font: 'A', 2 ink columns, advance 3, height 8, both cols solid.
    static const uint8_t  w[1] = {2};
    static const uint8_t  a[1] = {3};
    static const uint16_t o[1] = {0};
    static const uint8_t  b[2] = {0xFF, 0xFF};   // 2 columns x (8/8)=1 byte each
    const display::Font f = {8, 7, (uint8_t)'A', 1, w, a, o, b};

    uint8_t nx = display::draw_glyph(f, 5, 10, 'A');
    TEST_ASSERT_EQUAL_UINT8(5 + 3, nx);                       // advanced by 3
    TEST_ASSERT_TRUE(s_fb[10 >> 3][5] & (1u << (10 & 7u)));   // col 5, row 10 set
    TEST_ASSERT_TRUE(s_fb[17 >> 3][6] & (1u << (17 & 7u)));   // col 6, row 17 set
    TEST_ASSERT_EQUAL_UINT8(6, display::text_width(f, "AA")); // 3 + 3

    // draw_text_clipped cuts off at x_max, mid-glyph: only column 0 of "AA" (at x=0,1)
    // lands before x_max=1; column 1 (x=1) and the second glyph are clipped.
    display::draw_text_clipped(f, 0, 0, "AA", 1);
    TEST_ASSERT_TRUE(s_fb[0][0]  & 1u);   // first column drawn
    TEST_ASSERT_FALSE(s_fb[0][1] & 1u);   // second column clipped at x_max=1
}

// The whole printable range renders as itself, not as '?'.
//
// This is here because getting it wrong is invisible on the target and total on
// the host: a real font spans 0x20..0x7F, so `first + count` is 128, and a
// signed-char comparison against that wraps negative and swallows every
// character. ARM's char is unsigned so the pedal looked fine; every host render
// came out as question marks. The check must not depend on the sign of a char,
// and this pins it with a font whose range crosses 0x7F.
void test_var_font_spans_the_whole_printable_range(void) {
    // 96 glyphs from 0x20, so the last is 0x7F and first + count == 128 —
    // exactly the boundary a signed comparison mishandles. Each glyph is one
    // column, its bits taken from its own index, so glyphs are distinguishable.
    static uint8_t  w[96], a[96], b[96];
    static uint16_t o[96];
    for (uint8_t i = 0; i < 96; ++i) { w[i] = 1; a[i] = 1; o[i] = i; b[i] = (uint8_t)(i | 0x80u); }
    const display::Font f = {8, 7, 0x20u, 96, w, a, o, b};

    static const char probes[] = { ' ', 'A', 'z', '~', (char)0x7F };
    for (char c : probes) {
        display::clear();
        display::draw_glyph(f, 0, 0, c);
        TEST_ASSERT_EQUAL_UINT8(b[(uint8_t)c - 0x20u], s_fb[0][0]);
    }

    // Outside the font, and only outside it, substitutes '?'.
    display::clear();
    display::draw_glyph(f, 0, 0, (char)0x80);
    TEST_ASSERT_EQUAL_UINT8(b['?' - 0x20], s_fb[0][0]);
    display::clear();
    display::draw_glyph(f, 0, 0, (char)0x1F);
    TEST_ASSERT_EQUAL_UINT8(b['?' - 0x20], s_fb[0][0]);
}

// text_width measures the same characters draw_glyph draws — one substitution
// rule, not two.
void test_text_width_agrees_with_the_glyph_range(void) {
    static uint8_t  w[96], a[96], b[96];
    static uint16_t o[96];
    for (uint8_t i = 0; i < 96; ++i) { w[i] = 1; a[i] = (uint8_t)(1u + (i & 3u)); o[i] = i; b[i] = 0xFF; }
    const display::Font f = {8, 7, 0x20u, 96, w, a, o, b};

    TEST_ASSERT_EQUAL_UINT8(a['A' - 0x20] + a['z' - 0x20], display::text_width(f, "Az"));
    // An out-of-range byte measures as the '?' it will draw as.
    const char out[] = { (char)0x80, '\0' };
    TEST_ASSERT_EQUAL_UINT8(a['?' - 0x20], display::text_width(f, out));
}

// draw_text_right shifts the run so it ends at x+width.
void test_draw_text_right_aligns(void) {
    display::draw_text_right(0, 2, "Hi", 30);    // 2*6=12 px in a 30 px field
    TEST_ASSERT_EQUAL_UINT8(FONT_5X8['H' - 0x20][0], s_fb[2][30 - 12]);
}

// text_width returns the true pen-advance sum for a string wider than 255 px rather
// than the sum wrapped mod 256. The panel is 128 px, so no caller reaches this today;
// the return type is uint16_t so that a longer string cannot quietly report a small
// width and fool the right-align and font-fit callers that divide by it.
void test_text_width_wide_string_no_truncation(void) {
    // Minimal one-glyph font: 'A', advance 3.
    static const uint8_t  w[1] = {2};
    static const uint8_t  a[1] = {3};
    static const uint16_t o[1] = {0};
    static const uint8_t  b[2] = {0xFF, 0xFF};
    const display::Font f = {8, 7, (uint8_t)'A', 1, w, a, o, b};

    char s[101];                                 // 100 glyphs x advance 3 = 300 px
    for (int i = 0; i < 100; ++i) s[i] = 'A';
    s[100] = '\0';
    TEST_ASSERT_EQUAL_UINT16(300, display::text_width(f, s));   // not 300 % 256 = 44
}

// The fixed-font right-align holds its pixel count in a width wide enough for a long
// run, so a string wider than its field stays left-aligned instead of wrapping mod 256
// into a shift that looks like it fits.
void test_draw_text_right_wide_string_left_aligns(void) {
    char s[45];                                  // 44 chars x CHAR_ADVANCE(6) = 264 px
    for (int i = 0; i < 44; ++i) s[i] = 'A';
    s[44] = '\0';
    display::draw_text_right(0, 0, s, 100);      // 264 > 100: it cannot be right-aligned
    // 264 mod 256 = 8, which is < 100, so the unwidened version shifted x by 92 and left
    // column 0 blank. Held at x = 0, the first glyph column lands there.
    TEST_ASSERT_EQUAL_UINT8(FONT_5X8['A' - 0x20][0], s_fb[0][0]);
}

// compose_hslide composites two captured frames into the framebuffer as a
// horizontal slide. Fill `from`/`to` so each column carries a unique, side-tagged
// byte — `from` columns are 0..127, `to` columns are 128..255 — so every output
// column's source column is identifiable and any out-of-bounds index would surface
// as a wrong value. The expected bytes below are hand-computed, not re-derived from
// the driver's index expressions, so they pin the documented mapping independently.
static void fill_slide_frames(uint8_t from[OLED_PAGES][OLED_WIDTH],
                              uint8_t to[OLED_PAGES][OLED_WIDTH]) {
    for (uint8_t p = 0; p < OLED_PAGES; ++p)
        for (uint8_t c = 0; c < OLED_WIDTH; ++c) {
            from[p][c] = c;                    // 0..127  -> tags a `from` column
            to[p][c]   = (uint8_t)(128u + c);  // 128..255 -> tags a `to` column
        }
}

// offset 0 and offset OLED_WIDTH are the slide endpoints: nothing of the other
// frame is visible yet / any more, for either direction.
void test_compose_hslide_endpoints_show_single_frame(void) {
    static uint8_t from[OLED_PAGES][OLED_WIDTH], to[OLED_PAGES][OLED_WIDTH];
    fill_slide_frames(from, to);

    // offset 0: transition not started — the whole frame is still `from`.
    display::compose_hslide(from, to, 0, +1);
    TEST_ASSERT_EQUAL_UINT8(0,   s_fb[0][0]);     // from col 0
    TEST_ASSERT_EQUAL_UINT8(127, s_fb[3][127]);   // from col 127 (another page)
    display::compose_hslide(from, to, 0, -1);
    TEST_ASSERT_EQUAL_UINT8(0,   s_fb[0][0]);
    TEST_ASSERT_EQUAL_UINT8(127, s_fb[7][127]);

    // offset == width: transition complete — the whole frame is `to`.
    display::compose_hslide(from, to, OLED_WIDTH, +1);
    TEST_ASSERT_EQUAL_UINT8(128, s_fb[0][0]);     // to col 0
    TEST_ASSERT_EQUAL_UINT8(255, s_fb[5][127]);   // to col 127
    display::compose_hslide(from, to, OLED_WIDTH, -1);
    TEST_ASSERT_EQUAL_UINT8(128, s_fb[0][0]);
    TEST_ASSERT_EQUAL_UINT8(255, s_fb[1][127]);
}

// dir > 0: `to` enters from the right, `from` exits left. At offset 30 the left
// 98 columns show from[c+30]; the right 30 columns show to[c-98].
void test_compose_hslide_dir_pos_slides_to_in_from_right(void) {
    static uint8_t from[OLED_PAGES][OLED_WIDTH], to[OLED_PAGES][OLED_WIDTH];
    fill_slide_frames(from, to);

    display::compose_hslide(from, to, 30, +1);     // keep = 128 - 30 = 98
    TEST_ASSERT_EQUAL_UINT8(30,  s_fb[2][0]);      // from col 0+30
    TEST_ASSERT_EQUAL_UINT8(127, s_fb[2][97]);     // from col 97+30 (last `from`)
    TEST_ASSERT_EQUAL_UINT8(128, s_fb[2][98]);     // to col 98-98=0 (first `to`)
    TEST_ASSERT_EQUAL_UINT8(157, s_fb[2][127]);    // to col 127-98=29 -> 128+29
}

// dir < 0: `to` enters from the left, `from` exits right. At offset 30 the left
// 30 columns show to[98+c]; the right 98 columns show from[c-30].
void test_compose_hslide_dir_neg_slides_to_in_from_left(void) {
    static uint8_t from[OLED_PAGES][OLED_WIDTH], to[OLED_PAGES][OLED_WIDTH];
    fill_slide_frames(from, to);

    display::compose_hslide(from, to, 30, -1);     // keep = 98
    TEST_ASSERT_EQUAL_UINT8(226, s_fb[4][0]);      // to col 98+0 -> 128+98
    TEST_ASSERT_EQUAL_UINT8(255, s_fb[4][29]);     // to col 98+29=127 -> 255 (last `to`)
    TEST_ASSERT_EQUAL_UINT8(0,   s_fb[4][30]);     // from col 30-30=0 (first `from`)
    TEST_ASSERT_EQUAL_UINT8(97,  s_fb[4][127]);    // from col 127-30=97
}

// An offset past the panel width is clamped to OLED_WIDTH (fully settled on `to`),
// so no column indexes past the end of either frame.
void test_compose_hslide_offset_clamped_to_width(void) {
    static uint8_t from[OLED_PAGES][OLED_WIDTH], to[OLED_PAGES][OLED_WIDTH];
    fill_slide_frames(from, to);

    display::compose_hslide(from, to, 200, +1);    // 200 > 128 -> clamps to 128
    for (uint8_t c = 0; c < OLED_WIDTH; ++c)
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(128u + c), s_fb[0][c]);   // every column is `to`
}

// --- the band form ---------------------------------------------------------
// compose_hslide_band composites the same slide over a range of PIXEL rows and leaves
// every row outside it standing. Assert both halves of that against the full-screen
// compositor: inside the band the framebuffer is bit-for-bit what compose_hslide writes —
// the masked edge pages held against the whole-byte path of the same slide — and outside
// it is bit-for-bit what was there before the call. Rows rather than pages is the whole
// point, so a band whose edges fall mid-page has to keep the other rows of those pages.
static void assert_band(uint8_t y0, uint8_t y1, uint8_t offset, int8_t dir) {
    static uint8_t from[OLED_PAGES][OLED_WIDTH], to[OLED_PAGES][OLED_WIDTH];
    fill_slide_frames(from, to);

    // A background belonging to neither frame, so a row the band should not have touched
    // is recognisable however it went wrong.
    uint8_t before[OLED_PAGES][OLED_WIDTH];
    for (uint8_t p = 0; p < OLED_PAGES; ++p)
        for (uint8_t c = 0; c < OLED_WIDTH; ++c)
            before[p][c] = (uint8_t)(0x5Au ^ (uint8_t)(p * 7u + c));

    uint8_t full[OLED_PAGES][OLED_WIDTH];
    display::compose_hslide(from, to, offset, dir);
    memcpy(full, s_fb, sizeof(full));

    display::draw_framebuffer(before);
    display::compose_hslide_band(from, to, offset, dir, y0, y1);

    // The first row that disagrees, reported as the assertion's value: -1 is a pass.
    int first_bad = -1;
    for (uint8_t y = 0; y < OLED_HEIGHT && first_bad < 0; ++y) {
        const uint8_t page = (uint8_t)(y >> 3), bit = (uint8_t)(1u << (y & 7u));
        const bool inside = (y >= y0 && y <= y1);
        for (uint8_t c = 0; c < OLED_WIDTH; ++c) {
            const uint8_t want = (uint8_t)((inside ? full[page][c] : before[page][c]) & bit);
            if ((uint8_t)(s_fb[page][c] & bit) != want) { first_bad = (int)y; break; }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, first_bad,
        "a row inside the band did not slide, or one outside it moved (value = the row)");
}

// The caller's band is the parameter grid, y27-56: it starts partway down page 3 and
// ends on the first row of page 7, whose remaining rows carry the static footswitch
// labels. Both ends, both directions, and both endpoints of the transition.
void test_compose_hslide_band_keeps_the_rows_outside_it(void) {
    assert_band(27u, 56u, 30u, +1);
    assert_band(27u, 56u, 30u, -1);
    assert_band(27u, 56u, 0u,  +1);            // nothing of `to` on screen yet
    assert_band(27u, 56u, OLED_WIDTH, -1);     // fully arrived
}

// A page-aligned band is whole bytes with no edge mask at either end.
void test_compose_hslide_band_page_aligned_writes_whole_pages(void) {
    assert_band(24u, 39u, 30u, +1);
    assert_band(24u, 39u, 70u, -1);
}

// A band of a single row, mid-page and at each edge of the screen.
void test_compose_hslide_band_of_one_row(void) {
    assert_band(56u, 56u, 30u, +1);   // y0 == y1, the first row of the last page
    assert_band(59u, 59u, 30u, -1);   // mid-page
    assert_band(0u,  0u,  30u, +1);   // the top row of the screen
    assert_band(63u, 63u, 30u, +1);   // and the bottom
}

// A band covering every row is the full-screen slide. The bytes below are the same
// hand-computed mapping the compose_hslide cases pin rather than a reading taken off the
// driver, so the banded path is held to the documented slide in its own right — and then
// to the full-screen form, byte for byte, over every page.
void test_compose_hslide_band_over_the_whole_screen_is_the_full_slide(void) {
    static uint8_t from[OLED_PAGES][OLED_WIDTH], to[OLED_PAGES][OLED_WIDTH];
    fill_slide_frames(from, to);
    const uint8_t last = (uint8_t)(OLED_HEIGHT - 1u);

    display::compose_hslide_band(from, to, 30u, +1, 0u, last);   // keep = 128 - 30 = 98
    TEST_ASSERT_EQUAL_UINT8(30,  s_fb[2][0]);      // from col 0+30
    TEST_ASSERT_EQUAL_UINT8(127, s_fb[2][97]);     // from col 97+30 (last `from`)
    TEST_ASSERT_EQUAL_UINT8(128, s_fb[2][98]);     // to col 98-98=0 (first `to`)
    TEST_ASSERT_EQUAL_UINT8(157, s_fb[2][127]);    // to col 127-98=29 -> 128+29

    display::compose_hslide_band(from, to, 30u, -1, 0u, last);
    TEST_ASSERT_EQUAL_UINT8(226, s_fb[4][0]);      // to col 98+0 -> 128+98
    TEST_ASSERT_EQUAL_UINT8(255, s_fb[4][29]);     // to col 98+29=127 (last `to`)
    TEST_ASSERT_EQUAL_UINT8(0,   s_fb[4][30]);     // from col 30-30=0 (first `from`)
    TEST_ASSERT_EQUAL_UINT8(97,  s_fb[4][127]);    // from col 127-30=97

    uint8_t full[OLED_PAGES][OLED_WIDTH];
    display::compose_hslide(from, to, 45u, +1);
    memcpy(full, s_fb, sizeof(full));

    display::clear();
    display::compose_hslide_band(from, to, 45u, +1, 0u, last);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&full[0][0], &s_fb[0][0], OLED_PAGES * OLED_WIDTH);
}

// A band past the bottom of the screen stops at the last row rather than indexing off
// the end of the framebuffer, and one that ends before it starts is no band at all.
void test_compose_hslide_band_clamps_and_ignores_an_empty_band(void) {
    assert_band(60u, 200u, 30u, +1);           // clamped to 60..63

    static uint8_t from[OLED_PAGES][OLED_WIDTH], to[OLED_PAGES][OLED_WIDTH];
    fill_slide_frames(from, to);
    uint8_t before[OLED_PAGES][OLED_WIDTH];
    for (uint8_t p = 0; p < OLED_PAGES; ++p)
        for (uint8_t c = 0; c < OLED_WIDTH; ++c) before[p][c] = (uint8_t)(p * 31u + c);
    display::draw_framebuffer(before);
    display::compose_hslide_band(from, to, 30u, +1, 40u, 39u);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&before[0][0], &s_fb[0][0], OLED_PAGES * OLED_WIDTH);
}

// draw_framebuffer copies a full page-format bitmap verbatim.
void test_draw_framebuffer_copies(void) {
    uint8_t src[OLED_PAGES][OLED_WIDTH];
    for (uint8_t p = 0; p < OLED_PAGES; ++p)
        for (uint8_t c = 0; c < OLED_WIDTH; ++c)
            src[p][c] = (uint8_t)(p * 31 + c);
    display::draw_framebuffer(src);
    TEST_ASSERT_EQUAL_UINT8(src[3][70], s_fb[3][70]);
    TEST_ASSERT_EQUAL_UINT8(src[7][127], s_fb[7][127]);
}

// The SPI flush is blocking, so the frame-pacing API is degenerate: the display
// is never "busy", update_async() always accepts a frame (doing a full flush),
// and pump() is a safe no-op. display_manager relies on exactly this contract.
void test_flush_api_is_non_blocking_noop(void) {
    TEST_ASSERT_FALSE(display::update_busy());
    TEST_ASSERT_TRUE(display::update_async());
    TEST_ASSERT_FALSE(display::update_busy());      // flush completed synchronously
    display::pump();                                // no-op, safe any time
    TEST_ASSERT_FALSE(display::update_busy());
    TEST_ASSERT_TRUE(display::update_async());      // always ready for the next frame
}

// fill_rect sets every pixel in [x,x+w) x [y,y+h); `on=false` clears them again.
void test_fill_rect_fills_region(void) {
    display::fill_rect(2, 3, 4, 2, true);            // x 2..5, y 3..4 (rows 3,4)
    for (uint8_t c = 2; c <= 5; ++c)
        TEST_ASSERT_EQUAL_UINT8(0x18, s_fb[0][c]);   // bits 3|4 = 0x08|0x10
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][1]);       // left of the rect
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][6]);       // right of the rect
    display::fill_rect(2, 3, 4, 2, false);           // clear the same region
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][3]);
}

// A rect that spills past both edges is clipped to the panel, not wrapped or OOB.
void test_fill_rect_clips_to_bounds(void) {
    display::fill_rect(OLED_WIDTH - 2, OLED_HEIGHT - 2, 10, 10, true);  // spills off both edges
    TEST_ASSERT_TRUE(s_fb[7][OLED_WIDTH - 1] & (1u << 7));  // clipped corner set, no crash
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][0]);              // far corner untouched
}

// draw_gauge with v == max fills the whole interior, so a border-to-border gauge
// exactly the height of one page reads back as all-0xFF columns.
void test_draw_gauge_full_fills_interior(void) {
    display::draw_gauge(0, 0, 10, 8, 127);           // v = max -> full 8px-tall bar
    for (uint8_t c = 0; c < 10; ++c)
        TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][c]);   // border + full fill = every row
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][10]);      // just past the right border
}

// A partial value fills only the left portion (fillw = v*inner/127); interior
// columns past the fill show just the top+bottom border bits (0x81).
void test_draw_gauge_partial_fill(void) {
    display::draw_gauge(0, 0, 10, 8, 63);            // fillw = 63*8/127 = 3 columns
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][0]);       // left border
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][3]);       // within the fill (border rows + fill)
    TEST_ASSERT_EQUAL_UINT8(0x81, s_fb[0][5]);       // past the fill: only top+bottom border
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][9]);       // right border
}

// v > 127 is clamped, so it renders identically to v == 127 (a full bar).
void test_draw_gauge_value_clamped(void) {
    display::draw_gauge(0, 0, 10, 8, 200);
    for (uint8_t c = 0; c < 10; ++c)
        TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][c]);
}

// Degenerate sizes (w < 2 or h < 2 leave no room for the outline) draw nothing.
void test_draw_gauge_degenerate_is_noop(void) {
    display::draw_gauge(0, 0, 1, 8, 127);            // w < 2
    display::draw_gauge(0, 0, 10, 1, 127);           // h < 2
    for (uint8_t c = 0; c < OLED_WIDTH; ++c)
        TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][c]);
}

// draw_icon blits a column-major bitmap: each column is (h+7)/8 bytes, bit 0 of
// the first byte = the top pixel. An h == 8 icon is one byte per column.
void test_draw_icon_blits_column_major_bitmap(void) {
    static const uint8_t icon[3] = {0xFF, 0x00, 0x81};  // 3 cols x 1 byte (h = 8)
    display::draw_icon(3, 0, 3, 8, icon);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fb[0][3]);       // col 0: all rows
    TEST_ASSERT_EQUAL_UINT8(0x00, s_fb[0][4]);       // col 1: blank
    TEST_ASSERT_EQUAL_UINT8(0x81, s_fb[0][5]);       // col 2: rows 0 and 7
}

// A taller icon (h = 10 -> 2 bytes per column) exercises the row >> 3 byte index:
// bit 0 of byte 0 is row 0, bit 1 of byte 1 is row 9.
void test_draw_icon_multibyte_column(void) {
    static const uint8_t icon[2] = {0x01, 0x02};     // 1 col x 2 bytes: rows 0 and 9
    display::draw_icon(0, 0, 1, 10, icon);
    TEST_ASSERT_TRUE(s_fb[0][0] & (1u << 0));        // row 0  (page 0, bit 0)
    TEST_ASSERT_TRUE(s_fb[1][0] & (1u << 1));        // row 9  (page 1, bit 1)
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_clear_zeroes_framebuffer);
    RUN_TEST(test_draw_char_glyph_gap_advance);
    RUN_TEST(test_draw_char_non_printable_maps_to_question);
    RUN_TEST(test_draw_char_page_guard);
    RUN_TEST(test_draw_text_lays_out_chars);
    RUN_TEST(test_set_pixel_sets_and_clears_bit);
    RUN_TEST(test_draw_hline_sets_row);
    RUN_TEST(test_invert_rect_xors_region);
    RUN_TEST(test_var_font_glyph_and_width);
    RUN_TEST(test_var_font_spans_the_whole_printable_range);
    RUN_TEST(test_text_width_agrees_with_the_glyph_range);
    RUN_TEST(test_draw_text_right_aligns);
    RUN_TEST(test_text_width_wide_string_no_truncation);
    RUN_TEST(test_draw_text_right_wide_string_left_aligns);
    RUN_TEST(test_compose_hslide_endpoints_show_single_frame);
    RUN_TEST(test_compose_hslide_dir_pos_slides_to_in_from_right);
    RUN_TEST(test_compose_hslide_dir_neg_slides_to_in_from_left);
    RUN_TEST(test_compose_hslide_offset_clamped_to_width);
    RUN_TEST(test_compose_hslide_band_keeps_the_rows_outside_it);
    RUN_TEST(test_compose_hslide_band_page_aligned_writes_whole_pages);
    RUN_TEST(test_compose_hslide_band_of_one_row);
    RUN_TEST(test_compose_hslide_band_over_the_whole_screen_is_the_full_slide);
    RUN_TEST(test_compose_hslide_band_clamps_and_ignores_an_empty_band);
    RUN_TEST(test_draw_framebuffer_copies);
    RUN_TEST(test_flush_api_is_non_blocking_noop);
    RUN_TEST(test_fill_rect_fills_region);
    RUN_TEST(test_fill_rect_clips_to_bounds);
    RUN_TEST(test_draw_gauge_full_fills_interior);
    RUN_TEST(test_draw_gauge_partial_fill);
    RUN_TEST(test_draw_gauge_value_clamped);
    RUN_TEST(test_draw_gauge_degenerate_is_noop);
    RUN_TEST(test_draw_icon_blits_column_major_bitmap);
    RUN_TEST(test_draw_icon_multibyte_column);
    return UNITY_END();
}
