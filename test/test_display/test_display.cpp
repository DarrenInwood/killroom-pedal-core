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

// draw_text_right shifts the run so it ends at x+width.
void test_draw_text_right_aligns(void) {
    display::draw_text_right(0, 2, "Hi", 30);    // 2*6=12 px in a 30 px field
    TEST_ASSERT_EQUAL_UINT8(FONT_5X8['H' - 0x20][0], s_fb[2][30 - 12]);
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
    RUN_TEST(test_draw_text_right_aligns);
    RUN_TEST(test_compose_hslide_endpoints_show_single_frame);
    RUN_TEST(test_compose_hslide_dir_pos_slides_to_in_from_right);
    RUN_TEST(test_compose_hslide_dir_neg_slides_to_in_from_left);
    RUN_TEST(test_compose_hslide_offset_clamped_to_width);
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
