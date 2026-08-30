#pragma once
#include <cstdint>
#include "pedal_core_config.hpp"
#include "font.hpp"

// 128x64 monochrome page-format display driver over 4-wire SPI.
// The controller is selected at build time with one of the DISPLAY_* macros
// (DISPLAY_SSD1306, DISPLAY_SSD1309, DISPLAY_ST7567); an un-flagged build
// defaults to SSD1309. The framebuffer, font, and drawing primitives are
// identical for all three — only the init sequence and the page/column
// addressing of the flush path differ (see display.cpp).
//
// Uses a full 1 KB framebuffer; call update() to push it to hardware.
namespace display {
    void init();
    void clear();
    void update();         // flush framebuffer to display (blocking)

    // Flush the framebuffer to the display. The SPI burst is fast enough (~1.4 ms
    // for a full frame at 6 MHz) to complete in one call, so this is a full blocking
    // flush that always returns true; it keeps display_manager's frame-pacing API
    // uniform. (see display.cpp)
    bool update_async();

    // Frame-pacing hook, called every superloop wake. A no-op: update_async() flushes
    // synchronously, so there is no in-progress frame to advance between wakes.
    void pump();

    bool update_busy();    // always false — the flush completes within update_async()

    // Draw a character at column x (pixels), page row (0-7).
    // Returns x position after the character (for chaining).
    uint8_t draw_char(uint8_t x, uint8_t page, char c);

    // Draw a null-terminated string.
    void draw_text(uint8_t x, uint8_t page, const char* str);

    // Draw a string right-aligned within width pixels starting at x.
    void draw_text_right(uint8_t x, uint8_t page, const char* str, uint8_t width);

    // --- Variable-width font primitives (pixel-precise; any y, not page-aligned) ---
    // Blit one glyph with its top-left at (x, y); returns x + the glyph's pen advance.
    uint8_t draw_glyph(const Font& f, uint8_t x, uint8_t y, char c);
    // Draw a string; glyphs advance proportionally.
    void    draw_text(const Font& f, uint8_t x, uint8_t y, const char* str);
    // Like draw_text but cut off at x_max — including mid-glyph — to use every pixel.
    void    draw_text_clipped(const Font& f, uint8_t x, uint8_t y, const char* str, uint8_t x_max);
    // Pixel width (sum of pen advances) the string would occupy. Returned as
    // uint16_t so a string wider than 255 px reports its true width instead of
    // wrapping mod 256, which would fool the right-align / font-fit callers.
    uint16_t text_width(const Font& f, const char* str);
    // Draw a string ending its right edge at x_right (right-aligned).
    void    draw_text_right(const Font& f, uint8_t x_right, uint8_t y, const char* str);

    // Draw a string with its glyph pixels cleared (0) instead of set. Used to lay
    // inverted text over a filled background — e.g. the preset-number badge.
    void    draw_text_inv(const Font& f, uint8_t x, uint8_t y, const char* str);

    // Draw a horizontal line across the full width at a given pixel row.
    void draw_hline(uint8_t pixel_row);

    // Fill (on=true) or clear (on=false) a pixel-precise rectangle.
    void fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);

    // A value bar: a 1px outline track (w×h) with a left-anchored fill proportional
    // to v (0..127). For the param gauges and the editing focus panel.
    void draw_gauge(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t v);

    // Blit a small column-major bitmap: w columns, each (h+7)/8 bytes, bit0 = top
    // pixel (same layout as a font glyph). Sets ink pixels only.
    void draw_icon(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t* cols);

    // Invert a page-aligned region (for highlighting).
    void invert_rect(uint8_t x, uint8_t page, uint8_t w, uint8_t h_pages);

    // Invert (XOR) a pixel-precise rectangle — flips whatever is already drawn (text,
    // icons, gauges) so a highlighted widget reads as its photo-negative.
    void invert_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

    // Set a single pixel.
    void set_pixel(uint8_t x, uint8_t y, bool on);

    // Copy a full 8×128 page-format bitmap directly into the framebuffer.
    void draw_framebuffer(const uint8_t src[OLED_PAGES][OLED_WIDTH]);

    // Copy the current framebuffer out (for capturing a frame before a transition).
    void capture(uint8_t dst[OLED_PAGES][OLED_WIDTH]);

    // Compose a horizontal slide of two captured frames into the framebuffer: `offset`
    // (0..OLED_WIDTH) advances the transition. dir > 0 slides `to` in from the right
    // (old exits left); dir < 0 slides `to` in from the left (old exits right).
    void compose_hslide(const uint8_t from[OLED_PAGES][OLED_WIDTH],
                        const uint8_t to[OLED_PAGES][OLED_WIDTH], uint8_t offset, int8_t dir);

    // The same slide over a band of PIXEL rows y0..y1 (inclusive): every row outside the
    // band keeps whatever the framebuffer already holds, so a caller can slide one region
    // of the screen while the rest is drawn live. A band edge that falls mid-page is
    // masked row by row, which is the point of it — a page-granular band would strand the
    // descender rows of text that is sliding, or drag along a label that is not.
    // y1 past the last row is clamped to it; y0 > y1 draws nothing.
    void compose_hslide_band(const uint8_t from[OLED_PAGES][OLED_WIDTH],
                             const uint8_t to[OLED_PAGES][OLED_WIDTH], uint8_t offset,
                             int8_t dir, uint8_t y0, uint8_t y1);

#ifdef DISPLAY_SELFTEST
    // Bring-up diagnostic (build with -D DISPLAY_SELFTEST).
    // Assumes init() has run, then loops forever cycling three visually distinct stages so a
    // dead-panel fault can be localised by which stage appears — never returns. See the
    // implementation in display.cpp for the stage-by-stage interpretation.
    [[noreturn]] void selftest();
#endif
}
