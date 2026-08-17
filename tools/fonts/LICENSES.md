# Vendored font sources

These BDF bitmap fonts are converted to the firmware's variable-width format by
`tools/gen_fonts.py` (→ `include/pedal_core/font_data.hpp` and `src/font_data.cpp`). Only the
printable-ASCII glyphs (0x20–0x7E) are used. Sources are from the
[u8g2](https://github.com/olikraus/u8g2/tree/master/tools/font/bdf) BDF collection.

| File          | Role (font_data) | Description            | License |
|---------------|------------------|------------------------|---------|
| `helvB12.bdf` | `FONT_NAME`      | Helvetica Bold 12pt ~17px (preset name)   | Adobe X11 bitmap (redistributable) |
| `helvB10.bdf` | `FONT_NAME_MD`   | Helvetica Bold 10pt ~15px (name fallback) | Adobe X11 bitmap (redistributable) |
| `helvB08.bdf` | `FONT_NAME_SM`   | Helvetica Bold 8pt ~11px (name fallback)  | Adobe X11 bitmap (redistributable) |
| `helvR08.bdf` | `FONT_TEXT`      | Helvetica ~9px (algo / params / values)   | Adobe X11 bitmap (redistributable) |
| `4x6.bdf`     | `FONT_SMALL`     | 4×6 fixed (page indicator / units / msg)  | Public domain (X11 "misc") |

To change a font: drop a new BDF here, point the relevant `CHOSEN` entry in
`tools/gen_fonts.py` at it, run `python tools/gen_fonts.py`, then rebuild the firmware and
regenerate each product's screenshots (they compile these tables, so a font change moves
them). `gen_fonts.py --preview` renders the in-use fonts on the simulated OLED.
