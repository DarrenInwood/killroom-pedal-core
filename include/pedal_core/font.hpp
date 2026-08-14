#pragma once
#include <cstdint>

// Variable-width bitmap font for the OLED. Glyphs cover printable ASCII
// (0x20 space .. 0x7E tilde). Each glyph is normalised into a fixed-height box so
// the renderer can blit it at any pixel y with no per-glyph offset maths:
//   - `bitmap` is column-major: every glyph column is (height+7)/8 bytes, bit 0 of
//     the first byte = the top pixel of the box (matching the framebuffer's LSB-top
//     page format), so a glyph column maps straight onto vertical pixels.
//   - `widths[i]` = number of ink columns to blit for glyph i (= char first+i).
//   - `advances[i]` = pen advance after the glyph (proportional spacing).
//   - `offsets[i]` = byte offset of glyph i's columns within `bitmap`.
//   - `ascent` is the baseline position within the box (rows above the baseline),
//     used to vertically align text drawn in different fonts on a shared baseline.
//
// Generated from BDF/TTF sources by tools/gen_fonts.py — see src/drivers/font_data.*.
namespace display {

struct Font {
    uint8_t         height;     // box height in pixels
    uint8_t         ascent;     // rows from box top down to the baseline
    uint8_t         first;      // first character code (0x20)
    uint8_t         count;      // number of glyphs
    const uint8_t*  widths;     // [count] ink columns per glyph
    const uint8_t*  advances;   // [count] pen advance per glyph
    const uint16_t* offsets;    // [count] byte offset into bitmap
    const uint8_t*  bitmap;     // column-major glyph data, LSB = top
};

}  // namespace display
