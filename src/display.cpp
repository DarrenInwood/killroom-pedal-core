#include <pedal_core/display.hpp>
#include <pedal_core/hal.hpp>
#include <cstring>

// ===========================================================================
// Controller backend (compile-time selected)
// ---------------------------------------------------------------------------
// All three controllers are 128x64 page-format parts on the shared SPI bus, so
// the framebuffer, font, and primitives below are shared. Only two things vary:
//   1. The init command sequence.
//   2. The flush addressing: SSD1306/SSD1309 use horizontal addressing
//      (the controller auto-advances column/page across the whole frame);
//      ST7567 uses page addressing (set page + column before each page).
// Select with -D DISPLAY_SSD1306 / -D DISPLAY_SSD1309 / -D DISPLAY_ST7567.
// 4-wire SPI framing: commands are clocked with DC low, pixel data with DC high
// (no I2C control byte), so the init/addressing arrays hold raw command bytes.
// ===========================================================================
#if !defined(DISPLAY_SSD1306) && !defined(DISPLAY_SSD1309) && !defined(DISPLAY_ST7567)
#define DISPLAY_SSD1309 1   // default controller for an un-flagged build
#endif

// Panel orientation. Segment remap chooses which end of a row is column 0; COM scan direction
// chooses which end of the column stack is row 0. A 180° rotation needs both of them: flipping
// one alone mirrors the image instead of turning it. All three controllers encode the pair the
// same way, so one definition serves them all. A product whose panel is mounted inverted builds
// with -D DISPLAY_ROTATE_180.
#ifdef DISPLAY_ROTATE_180
static constexpr uint8_t SEG_REMAP = 0xA0;   // column 0   = SEG0
static constexpr uint8_t COM_SCAN  = 0xC0;   // COM0       = row 0
#else
static constexpr uint8_t SEG_REMAP = 0xA1;   // column 127 = SEG0
static constexpr uint8_t COM_SCAN  = 0xC8;   // COM[N-1]   = row 0
#endif

#if defined(DISPLAY_ST7567)
// ST7567 is a page-addressing LCD: its RAM is 132 columns wide, so the visible
// 128-pixel window is reached through a small column offset. Tune COL_OFFSET
// (0 or 4) and the electronic-volume/bias bytes in init_seq against the module.
#ifndef ST7567_COL_OFFSET
#define ST7567_COL_OFFSET 4
#endif
static constexpr bool    PAGE_ADDRESSING = true;
static constexpr uint8_t COL_OFFSET      = ST7567_COL_OFFSET;
static const uint8_t init_seq[] = {
    0xE2,        // soft reset
    0xA2,        // bias 1/9
    SEG_REMAP,   // ADC direction — the ST7567's segment remap, matching the SSD130x
    COM_SCAN,    // COM output scan direction — pairs with the remap
    0x40,        // display start line = 0
    0x2C,        // power control: booster on
    0x2E,        // power control: booster + voltage regulator on
    0x2F,        // power control: booster + regulator + follower on
    0x26,        // regulation resistor ratio (mid; raises/lowers contrast range)
    0x81, 0x20,  // electronic volume (contrast) — tune for the module
    0xA6,        // normal display (not inverted)
    0xAF,        // display on
    // Note: the ST7567 datasheet recommends a short delay between the 0x2C/0x2E/
    // 0x2F power-control steps; most small panels tolerate the back-to-back
    // sequence, but add per-step delays here if a module fails to power up.
};

#elif defined(DISPLAY_SSD1306)
// SSD1306 has an internal charge pump (enabled via 0x8D,0x14) and uses the
// SSD1306-typical pre-charge / VCOMH values; otherwise identical to SSD1309.
static constexpr bool    PAGE_ADDRESSING = false;
static constexpr uint8_t COL_OFFSET      = 0;
static const uint8_t init_seq[] = {
    0xAE,        // display off
    0xD5, 0x80,  // clock divide ratio / oscillator freq (ratio=1, freq=8)
    0xA8, 0x3F,  // multiplex ratio (63 = 64 rows)
    0xD3, 0x00,  // display offset = 0
    0x40,        // display start line = 0
    0x8D, 0x14,  // charge pump enable (internal VCC)
    0x20, 0x00,  // horizontal addressing mode
    SEG_REMAP,   // segment remap — see DISPLAY_ROTATE_180 above
    COM_SCAN,    // COM output scan direction — pairs with the remap
    0xDA, 0x12,  // COM pins hardware config (alt, no left-right remap)
    0x81, 0xCF,  // contrast (typical for internal-VCC operation)
    0xD9, 0xF1,  // pre-charge period (phase1=1, phase2=15 — charge-pump default)
    0xDB, 0x40,  // VCOMH deselect level
    0xA4,        // display from RAM (not all-on)
    0xA6,        // normal display (not inverted)
    0xAF,        // display on
};

#else  // DISPLAY_SSD1309
// SSD1309 has no internal charge pump — it requires an external VCC supply.
//   - No charge pump: omit 0x8D/0x14 entirely.
//   - Pre-charge period 0xD9: 0x22 (2+2 DCLKs) for external-VCC operation.
//   - VCOMH deselect 0xDB: 0x34 (0.78 × VCC) instead of 0x40.
static constexpr bool    PAGE_ADDRESSING = false;
static constexpr uint8_t COL_OFFSET      = 0;
static const uint8_t init_seq[] = {
    0xAE,        // display off
    0xD5, 0x80,  // clock divide ratio / oscillator freq (ratio=1, freq=8)
    0xA8, 0x3F,  // multiplex ratio (63 = 64 rows)
    0xD3, 0x00,  // display offset = 0
    0x40,        // display start line = 0
    0x20, 0x00,  // horizontal addressing mode
    SEG_REMAP,   // segment remap — see DISPLAY_ROTATE_180 above
    COM_SCAN,    // COM output scan direction — pairs with the remap
    0xDA, 0x12,  // COM pins hardware config (alt, no left-right remap)
    0x81, 0xFF,  // contrast (0xFF = maximum; SSD1309 external VCC supports full range)
    0xD9, 0x22,  // pre-charge period (phase1=2, phase2=2 DCLKs — no charge pump)
    0xDB, 0x34,  // VCOMH deselect level (0x34 = 0.78 × VCC for SSD1309)
    0xA4,        // display from RAM (not all-on)
    0xA6,        // normal display (not inverted)
    0xAF,        // display on
};
#endif

// --- 4-wire SPI transport ---------------------------------------------------
// CS/DC/RES reach the product's pins through hal::display_*; SCK/MOSI ride the
// shared SPI bus. Commands are clocked with DC low, pixel data with DC high; CS
// frames each transfer (other devices on the bus assert their own CS).
static inline void cs_low()   { pedal_core::hal::display_cs(true); }
static inline void cs_high()  { pedal_core::hal::display_cs(false); }

static void disp_cmd(const uint8_t* b, uint16_t n)
{
    cs_low();
    pedal_core::hal::display_dc_data(false);  // DC low = command
    spi::write(b, n);
    cs_high();
}

static void disp_data(const uint8_t* b, uint16_t n)
{
    cs_low();
    pedal_core::hal::display_dc_data(true);   // DC high = data
    spi::write(b, n);
    cs_high();
}

// Set the page/column address for one page (page-addressing controllers).
// The column pointer is parked at the visible-window origin (COL_OFFSET) so the
// 128 data bytes that follow land in columns COL_OFFSET..COL_OFFSET+127.
static void set_page_addr(uint8_t page)
{
    const uint8_t cmds[3] = {
        static_cast<uint8_t>(0xB0u | page),                         // page address
        static_cast<uint8_t>(0x10u | ((COL_OFFSET >> 4) & 0x0Fu)),  // column high nibble
        static_cast<uint8_t>(0x00u | (COL_OFFSET & 0x0Fu)),         // column low nibble
    };
    disp_cmd(cmds, sizeof(cmds));
}

// ===========================================================================
// Shared framebuffer, font, and primitives
// ===========================================================================

// ---------------------------------------------------------------------------
// 6x8 font: 96 printable characters (space=0x20 .. tilde=0x7E)
// Each character is 5 columns of pixel data (1 byte = 8 rows, LSB=top).
// Column 6 is always 0 (spacing).
// ---------------------------------------------------------------------------
static const uint8_t FONT_5X8[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x14,0x08,0x3E,0x08,0x14}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x00,0x41,0x22,0x14,0x08}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
    {0x00,0x01,0x02,0x04,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'
    {0x00,0x08,0x36,0x41,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00}, // '}'
    {0x10,0x08,0x08,0x10,0x08}, // '~'
    {0x00,0x00,0x00,0x00,0x00}, // DEL (unused, padding)
};

// Glyph advance: 5 pixels + 1 spacing gap. (Named CHAR_ADVANCE, not CHAR_WIDTH,
// to avoid colliding with the C23 <limits.h> width macro on host/native builds.)
static constexpr uint8_t CHAR_ADVANCE = 6;

// Framebuffer: 128 columns x 8 pages (each page = 8 rows of pixels)
static uint8_t s_fb[OLED_PAGES][OLED_WIDTH];

void display::init()
{
    // Pins as outputs, CS idle high, panel supply off.
    pedal_core::hal::display_pins_init();
    cs_high();

    // Power-up sequence. The SSD1309 has no internal charge pump, so its ~13 V VCC comes from
    // a boost supply gated by hal::display_power. Hold the controller in reset, enable the
    // supply and let VCC reach regulation, then release reset and let the controller settle
    // before the init commands — sending commands before VCC is stable leaves the panel in an
    // undefined state.
    pedal_core::hal::display_reset(true);         // hold in reset
    pedal_core::hal::display_power(true);         // supply on -> VCC ramps
    systick::delay_ms(100);                       // let VCC reach regulation
    pedal_core::hal::display_reset(false);        // release reset
    systick::delay_ms(10);                        // controller settle

    disp_cmd(init_seq, sizeof(init_seq));
    clear();
    update();
}

void display::clear()
{
    memset(s_fb, 0, sizeof(s_fb));
}

void display::update()
{
    if constexpr (PAGE_ADDRESSING) {
        // Page-addressing controller (ST7567): set each page's origin, then send
        // its 128 pixel bytes.
        for (uint8_t page = 0; page < OLED_PAGES; ++page) {
            set_page_addr(page);
            disp_data(s_fb[page], OLED_WIDTH);
        }
    } else {
        // Horizontal-addressing controller (SSD130x): set the column/page window
        // once; the controller auto-advances across the whole frame, so the
        // 1 KB framebuffer streams as one SPI data burst.
        const uint8_t addr_cmds[6] = {
            0x21, 0, 127,   // column address 0..127
            0x22, 0, 7,     // page address 0..7
        };
        disp_cmd(addr_cmds, sizeof(addr_cmds));
        disp_data(&s_fb[0][0], OLED_PAGES * OLED_WIDTH);
    }
}


uint8_t display::draw_char(uint8_t x, uint8_t page, char c)
{
    if (page >= OLED_PAGES) return x;
    if (c < 0x20 || c > 0x7E) c = '?';

    const uint8_t* glyph = FONT_5X8[c - 0x20];
    for (uint8_t col = 0; col < 5 && (x + col) < OLED_WIDTH; ++col) {
        s_fb[page][x + col] = glyph[col];
    }
    if (x + 5 < OLED_WIDTH) s_fb[page][x + 5] = 0x00;
    return x + CHAR_ADVANCE;
}

void display::draw_text(uint8_t x, uint8_t page, const char* str)
{
    while (*str && x < OLED_WIDTH) {
        x = draw_char(x, page, *str++);
    }
}

// ---------------------------------------------------------------------------
// Variable-width font primitives (see font.hpp). Glyphs are blit one pixel column
// at a time at an arbitrary y, so text can sit on any pixel row rather than the
// 8-pixel page grid.
// ---------------------------------------------------------------------------
// A character's place in the font, with anything outside it substituted by '?'.
//
// The comparison is deliberately unsigned and widened. `first + count` reaches
// 128 and beyond, and `char` is signed on any host and unsigned on ARM — so
// comparing a char against `(char)(first + count)` compares it against a
// negative number on the host, which is true for every character and turns all
// text into question marks. The library builds for both, so neither may depend
// on the sign of a plain char.
static uint8_t glyph_index(const display::Font& f, char c)
{
    uint16_t code = (uint8_t)c;
    if (code < f.first || code >= (uint16_t)f.first + f.count) code = (uint8_t)'?';
    return (uint8_t)(code - f.first);
}

// Blit one glyph, clipping any column at or beyond x_max (so a string can be cut
// mid-letter at an exact pixel). `ink` sets the glyph's pixels; !ink clears them
// (inverted text over a filled background). Returns x + the glyph's pen advance.
static uint8_t blit_glyph(const display::Font& f, uint8_t x, uint8_t y, char c,
                          uint8_t x_max, bool ink)
{
    const uint8_t  idx = glyph_index(f, c);
    const uint8_t  w   = f.widths[idx];
    const uint8_t  bpc = (uint8_t)((f.height + 7u) / 8u);  // bytes per column
    const uint8_t* col = f.bitmap + f.offsets[idx];
    for (uint8_t cx = 0; cx < w; ++cx, col += bpc) {
        const uint8_t px = (uint8_t)(x + cx);
        if (px >= x_max || px >= OLED_WIDTH) break;
        for (uint8_t row = 0; row < f.height; ++row) {
            const uint8_t yy = (uint8_t)(y + row);
            if (yy >= OLED_HEIGHT) break;
            if (col[row >> 3] & (1u << (row & 7u))) {
                if (ink) s_fb[yy >> 3][px] |=  (1u << (yy & 7u));
                else     s_fb[yy >> 3][px] &= ~(1u << (yy & 7u));
            }
        }
    }
    return (uint8_t)(x + f.advances[idx]);
}

uint8_t display::draw_glyph(const Font& f, uint8_t x, uint8_t y, char c)
{
    return blit_glyph(f, x, y, c, OLED_WIDTH, true);
}

void display::draw_text(const Font& f, uint8_t x, uint8_t y, const char* str)
{
    while (*str && x < OLED_WIDTH) x = blit_glyph(f, x, y, *str++, OLED_WIDTH, true);
}

void display::draw_text_inv(const Font& f, uint8_t x, uint8_t y, const char* str)
{
    while (*str && x < OLED_WIDTH) x = blit_glyph(f, x, y, *str++, OLED_WIDTH, false);
}

void display::draw_text_clipped(const Font& f, uint8_t x, uint8_t y, const char* str, uint8_t x_max)
{
    while (*str && x < x_max) x = blit_glyph(f, x, y, *str++, x_max, true);
}

uint16_t display::text_width(const Font& f, const char* str)
{
    uint16_t w = 0;
    for (const char* p = str; *p; ++p) w += f.advances[glyph_index(f, *p)];
    return w;
}

void display::draw_text_right(const Font& f, uint8_t x_right, uint8_t y, const char* str)
{
    const uint16_t w = text_width(f, str);
    draw_text(f, (uint8_t)(w <= x_right ? x_right - w : 0u), y, str);
}

void display::draw_text_right(uint8_t x, uint8_t page, const char* str, uint8_t width)
{
    uint16_t len = 0;
    for (const char* p = str; *p; ++p) ++len;
    const uint16_t text_px = (uint16_t)(len * CHAR_ADVANCE);
    if (text_px < width) x += (uint8_t)(width - text_px);
    draw_text(x, page, str);
}

void display::draw_hline(uint8_t pixel_row)
{
    if (pixel_row >= OLED_HEIGHT) return;
    const uint8_t page = pixel_row >> 3;
    const uint8_t bit  = 1u << (pixel_row & 7u);
    for (uint8_t col = 0; col < OLED_WIDTH; ++col) {
        s_fb[page][col] |= bit;
    }
}

void display::fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on)
{
    for (uint8_t yy = y; yy < (uint16_t)y + h && yy < OLED_HEIGHT; ++yy)
        for (uint8_t xx = x; xx < (uint16_t)x + w && xx < OLED_WIDTH; ++xx)
            set_pixel(xx, yy, on);
}

void display::draw_gauge(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t v)
{
    if (w < 2u || h < 2u) return;
    if (v > 127u) v = 127u;
    // 1px outline track.
    for (uint8_t xx = x; xx < (uint16_t)x + w; ++xx) {
        set_pixel(xx, y, true);
        set_pixel(xx, (uint8_t)(y + h - 1u), true);
    }
    for (uint8_t yy = y; yy < (uint16_t)y + h; ++yy) {
        set_pixel(x, yy, true);
        set_pixel((uint8_t)(x + w - 1u), yy, true);
    }
    // Left-anchored fill inside the border, proportional to v / 127.
    const uint8_t inner = (uint8_t)(w - 2u);
    const uint8_t fillw = (uint8_t)(((uint16_t)v * inner) / 127u);
    fill_rect((uint8_t)(x + 1u), (uint8_t)(y + 1u), fillw, (uint8_t)(h - 2u), true);
}

void display::draw_icon(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t* cols)
{
    const uint8_t bpc = (uint8_t)((h + 7u) / 8u);
    for (uint8_t cx = 0; cx < w; ++cx) {
        const uint8_t* c = cols + (uint16_t)cx * bpc;
        for (uint8_t row = 0; row < h; ++row) {
            if (c[row >> 3] & (1u << (row & 7u)))
                set_pixel((uint8_t)(x + cx), (uint8_t)(y + row), true);
        }
    }
}

void display::invert_rect(uint8_t x, uint8_t page, uint8_t w, uint8_t h_pages)
{
    for (uint8_t p = page; p < page + h_pages && p < OLED_PAGES; ++p) {
        for (uint8_t col = x; col < x + w && col < OLED_WIDTH; ++col) {
            s_fb[p][col] ^= 0xFF;
        }
    }
}

void display::invert_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    for (uint8_t yy = y; yy < (uint16_t)y + h && yy < OLED_HEIGHT; ++yy) {
        const uint8_t page = yy >> 3;
        const uint8_t bit  = (uint8_t)(1u << (yy & 7u));
        for (uint8_t xx = x; xx < (uint16_t)x + w && xx < OLED_WIDTH; ++xx)
            s_fb[page][xx] ^= bit;
    }
}

void display::set_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) return;
    const uint8_t page = y >> 3;
    const uint8_t bit  = 1u << (y & 7u);
    if (on) s_fb[page][x] |=  bit;
    else    s_fb[page][x] &= ~bit;
}

void display::draw_framebuffer(const uint8_t src[OLED_PAGES][OLED_WIDTH])
{
    memcpy(s_fb, src, sizeof(s_fb));
}

void display::capture(uint8_t dst[OLED_PAGES][OLED_WIDTH])
{
    memcpy(dst, s_fb, sizeof(s_fb));
}

#ifdef DISPLAY_SELFTEST
// Bring-up diagnostic. Loops forever cycling three stages ~1.5 s each so a dead panel can be
// localised by which stage appears (watch it live while wiggling wires / checking jumpers):
//
//   Stage 1 — Entire display ON (command 0xA5, lights every pixel regardless of RAM):
//             all pixels lit  → commands reach the controller AND the panel can emit.
//   Stage 2 — Resume-from-RAM (0xA4) with a cleared framebuffer:
//             all pixels dark  → pixel-data writes land (the DC line and data path work).
//   Stage 3 — A checkerboard frame:
//             pattern shown    → column/page addressing and orientation are correct.
//
// Reading the result:
//   * Stage 1 never lights            → commands aren't landing: interface jumpers (BS0/1 set
//                                        to I2C, not 4-wire SPI), CS, SCK, RES, or power.
//   * Stage 1 lights, 2/3 don't       → the DC/data path is at fault (data sent as commands).
//   * All three cycle cleanly         → the panel is fine; flash the normal firmware.
void display::selftest()
{
    // The superloop that normally kicks the ~2 s watchdog is never reached in this mode, so
    // feed it through each hold or the diagnostic would reset itself mid-stage.
    const auto hold = [](uint16_t ms) {
        for (uint16_t i = 0; i < ms; ++i) { watchdog::kick(); systick::delay_ms(1); }
    };
    const uint8_t all_on[]   = { 0xA5u };          // entire display ON (ignore RAM)
    const uint8_t from_ram[] = { 0xA4u, 0xA6u };   // resume RAM display, non-inverted

    while (true) {
        disp_cmd(all_on, sizeof(all_on));          // stage 1
        hold(1500);

        disp_cmd(from_ram, sizeof(from_ram));      // stage 2
        clear();
        update();
        hold(1500);

        for (uint8_t page = 0; page < OLED_PAGES; ++page)   // stage 3
            for (uint8_t col = 0; col < OLED_WIDTH; ++col)
                s_fb[page][col] = (col & 1u) ? 0xAAu : 0x55u;
        update();
        hold(1500);
    }
}
#endif  // DISPLAY_SELFTEST

void display::compose_hslide(const uint8_t from[OLED_PAGES][OLED_WIDTH],
                             const uint8_t to[OLED_PAGES][OLED_WIDTH], uint8_t offset, int8_t dir)
{
    compose_hslide_band(from, to, offset, dir, 0u, (uint8_t)(OLED_HEIGHT - 1u));
}

void display::compose_hslide_band(const uint8_t from[OLED_PAGES][OLED_WIDTH],
                                  const uint8_t to[OLED_PAGES][OLED_WIDTH], uint8_t offset,
                                  int8_t dir, uint8_t y0, uint8_t y1)
{
    if (offset > OLED_WIDTH) offset = OLED_WIDTH;
    if (y1 >= OLED_HEIGHT) y1 = (uint8_t)(OLED_HEIGHT - 1u);
    if (y0 > y1) return;
    const uint8_t keep = (uint8_t)(OLED_WIDTH - offset);  // columns of `from` still visible
    for (uint8_t page = (uint8_t)(y0 >> 3); page <= (uint8_t)(y1 >> 3); ++page) {
        // Which rows of this page the band covers: all eight through its interior, and a
        // mask on the page an edge falls inside.
        const uint8_t top  = (uint8_t)(page << 3);
        const uint8_t lo   = (y0 > top) ? (uint8_t)(y0 - top) : 0u;
        const uint8_t hi   = (y1 < (uint8_t)(top + 7u)) ? (uint8_t)(y1 - top) : 7u;
        const uint8_t mask = (uint8_t)(((1u << (hi - lo + 1u)) - 1u) << lo);
        for (uint8_t c = 0; c < OLED_WIDTH; ++c) {
            uint8_t v;
            if (dir >= 0) {
                // `from` exits left; `to` enters from the right.
                v = (c < keep) ? from[page][c + offset] : to[page][(uint8_t)(c - keep)];
            } else {
                // `from` exits right; `to` enters from the left.
                v = (c >= offset) ? from[page][(uint8_t)(c - offset)] : to[page][(uint8_t)(keep + c)];
            }
            // Whole bytes where the band owns the page; read-modify-write at its edges.
            if (mask == 0xFFu) s_fb[page][c] = v;
            else               s_fb[page][c] = (uint8_t)((s_fb[page][c] & (uint8_t)~mask) | (v & mask));
        }
    }
}
