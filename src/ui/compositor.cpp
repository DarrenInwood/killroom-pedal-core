#include <pedal_core/ui/compositor.hpp>
#include <pedal_core/font_data.hpp>
#include <pedal_core/hal.hpp>            // systick::now_ms
#include "pedal_core_ui_config.hpp"      // DISPLAY_PARAM_SHOW_MS
#include <cstring>
#include <cstdio>

// The compositor draws the whole frame from current state every time something
// is dirty; nothing is incrementally erased, so stale pixels cannot survive a
// state change. Input is polled independently of redraw, and the flush is the
// display driver's asynchronous path — never a blocking wait here.

namespace pedal_core::ui {

// Eighth-note glyph for the header status badge: 5 wide x 8 tall, column-major, LSB=top.
static const uint8_t ICON_NOTE[5] = { 0xE0u, 0xE0u, 0xE0u, 0x7Fu, 0x07u };

// The letter B, same 5x8 cell: the context-line badge for Scene B sounding. Scene A
// draws nothing, because A is the ordinary state and most presets have no scene at all.
static const uint8_t ICON_SCENE_B[5] = { 0x7Fu, 0x49u, 0x49u, 0x36u, 0x00u };

// Room for the "Page X/Y" indicator, and the conversion that bounds it.
//
// A compiler sizes this from the conversion's own range, not from what the values happen to
// be: promoted to unsigned, each field could be ten digits, and it reports a truncation the
// code cannot reach. %hu says the fields are shorts, which is both true and narrow enough
// to fit — and unlike %hhu it still holds page 256, which is one past what a byte carries
// and exactly the value "the page after the last" reaches.
static constexpr uint8_t PAGE_TEXT_BYTES = 5u + 5u + 1u + 5u + 1u;   // "Page " %hu "/" %hu NUL

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

// Copy src into dst, truncated so it fits within max_px when drawn in font f.
void Compositor::fit(const display::Font& f, const char* src, uint8_t max_px,
                     char* dst, uint8_t dstsz)
{
    uint8_t w = 0, j = 0;
    for (uint8_t i = 0; src[i] && j < dstsz - 1u; ++i) {
        char c = src[i];
        if (c < (char)f.first || c >= (char)(f.first + f.count)) c = '?';
        const uint8_t adv = f.advances[(uint8_t)c - f.first];
        if ((uint16_t)w + adv > max_px) break;
        w = (uint8_t)(w + adv);
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

// Copy src into dst, collapsing the literal "+/-" into the single ± glyph and (when
// strip_spaces) dropping spaces. Capped at maxlen output chars.
// Copy src into dst, truncated to its capacity, and say whether the stored text moved.
// The comparison is against the truncated form: two names that differ only past the end of
// the buffer draw the same pixels, so they are the same screen and cost no redraw.
bool Compositor::assign(char* dst, uint16_t cap, const char* src)
{
    const char* v = (src != nullptr) ? src : "";
    const size_t room = (size_t)(cap - 1u);
    if (strncmp(dst, v, room) == 0) return false;

    // The copy is spelled out rather than left to strncpy: truncating is the intent here and
    // the terminator belongs to the same step, but strncpy's contract does not say so and a
    // compiler warns on every consumer's build. The bytes are what strncpy leaves — the
    // text, then zeroes to the end of the buffer — and the terminator is guaranteed.
    size_t n = 0;
    while (n < room && v[n] != '\0') ++n;
    memcpy(dst, v, n);
    memset(dst + n, 0, (size_t)cap - n);
    return true;
}

void Compositor::to_display(char* dst, const char* src, uint8_t maxlen, bool strip_spaces)
{
    uint8_t j = 0;
    for (uint8_t i = 0; src[i] && j < maxlen; ) {
        if (src[i] == '+' && src[i + 1] == '/' && src[i + 2] == '-') {
            dst[j++] = GLYPH_PM;
            i += 3;
        } else if (strip_spaces && src[i] == ' ') {
            ++i;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// Draw a string centered within the [x0, x0+w) span at pixel row y.
void Compositor::draw_centered(const display::Font& f, uint8_t x0, uint8_t w,
                               uint8_t y, const char* s)
{
    const uint8_t tw = display::text_width(f, s);
    const uint8_t x  = (tw < w) ? (uint8_t)(x0 + (w - tw) / 2u) : x0;
    display::draw_text(f, x, y, s);
}

// 1px rectangle outline.
void Compositor::rect_outline(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (w == 0u || h == 0u) return;
    for (uint8_t xx = x; xx < (uint16_t)x + w; ++xx) {
        display::set_pixel(xx, y, true);
        display::set_pixel(xx, (uint8_t)(y + h - 1u), true);
    }
    for (uint8_t yy = y; yy < (uint16_t)y + h; ++yy) {
        display::set_pixel(x, yy, true);
        display::set_pixel((uint8_t)(x + w - 1u), yy, true);
    }
}

// Small filled triangle: 5 wide, 3 tall, centered on cx; apex up or down.
// One statement of where the tick goes, for both the 28px grid column and the 120px
// panel: the pot's position across the track, inset by the tick's own width so full scale
// stops at the track's right end instead of overhanging it.
void Compositor::draw_pickup_tick(uint8_t x0, uint8_t track_w, uint8_t y, uint16_t pot)
{
    const uint8_t span = (uint8_t)(track_w > PICKUP_W ? track_w - PICKUP_W : 0u);
    const uint8_t off  = (uint8_t)(((uint32_t)gauge_of(pot) * span) / 127u);
    display::fill_rect((uint8_t)(x0 + off), y, PICKUP_W, PICKUP_H, true);
}

void Compositor::draw_tri(uint8_t cx, uint8_t y, bool up)
{
    for (uint8_t r = 0; r < 3u; ++r) {
        const uint8_t w  = up ? (uint8_t)(2u * r + 1u) : (uint8_t)(2u * (2u - r) + 1u);
        const uint8_t x0 = (uint8_t)(cx - w / 2u);
        for (uint8_t i = 0; i < w; ++i)
            display::set_pixel((uint8_t)(x0 + i), (uint8_t)(y + r), true);
    }
}

// Pixels reserved at the right of the context line for the "Page X/Y" widget: the widest it
// can render (its font is variable-width, so measure every digit) plus a 1px buffer each
// side. Constant for a given font — computed once and cached.
uint8_t Compositor::page_widget_width()
{
    static uint8_t cached = 0u;
    if (cached == 0u) {
        uint8_t w = 0u;
        for (char d = '0'; d <= '9'; ++d) {
            char s[12];
            snprintf(s, sizeof(s), "Page %c/%c", d, d);
            const uint8_t tw = display::text_width(display::FONT_SMALL, s);
            if (tw > w) w = tw;
        }
        cached = (uint8_t)(w + 2u);  // 1px buffer each side
    }
    return cached;
}

// ---------------------------------------------------------------------------
// The performance screen
// ---------------------------------------------------------------------------

// Header: product icon (left, via the hook) + preset name + preset number (far
// right, big font). When the preset widget is selected the whole row from just
// past the icon photo-negates.
void Compositor::draw_header(uint32_t now)
{
    const uint8_t inset = draw_header_icon(now);

    // Preset number, right-aligned in the largest header font — plain digits, no badge.
    //
    // Zero-based, and that is the whole point: this number is the MIDI Program Change
    // number. A player reading 012 off the display can send bank 0, program 12 and land
    // here. Counting from 1 on screen was friendlier to read and forced everyone
    // addressing the pedal to subtract one, which is the error nobody catches on stage.
    char num[8];
    snprintf(num, sizeof(num), "%03u", (unsigned)m_preset);
    const uint8_t num_w = display::text_width(display::FONT_NAME, num);
    const uint8_t num_x = (uint8_t)(OLED_WIDTH - num_w);
    display::draw_text_right(display::FONT_NAME, OLED_WIDTH,
                             (uint8_t)((RULE_Y - display::FONT_NAME.height) / 2u), num);

    // Unsaved edits, as a dot just left of the number -- the mark a text editor puts on a
    // modified file. It is a state rather than an instruction, so it costs a few pixels
    // beside the thing it is about instead of a whole row; the bottom edge is the
    // footswitch labels'. The name region shrinks to leave it room, and the dot sits
    // inside the header highlight so it photo-negates with the rest of the row.
    constexpr uint8_t DOT = 3u, DOT_GAP = 3u;
    const uint8_t dot_w = m_save_prompt ? (uint8_t)(DOT + DOT_GAP) : 0u;
    if (m_save_prompt)
        display::fill_rect((uint8_t)(num_x - dot_w), (uint8_t)((RULE_Y - DOT) / 2u),
                           DOT, DOT, true);

    // Preset name (or context-name fallback) between the icon and the number,
    // truncated at a char boundary so it ends cleanly.
    char name_disp[sizeof(m_preset_name)];
    strncpy(name_disp, m_preset_name, sizeof(name_disp));
    name_disp[sizeof(name_disp) - 1u] = '\0';
    uint8_t n = (uint8_t)strlen(name_disp);
    while (n > 0 && name_disp[n - 1] == ' ') name_disp[--n] = '\0';
    const char* big = (name_disp[0] != '\0') ? name_disp : m_context_name;

    // Graduated header fallback: render the name in the largest bold size that fits whole
    // — 12pt, else 10pt, else 8pt — each vertically centred in the header bar. If even 8pt
    // overflows, clip mid-character (never dropping whole characters) at a 2px gap before
    // the number.
    const uint8_t name_x   = (uint8_t)(inset + 3u);
    const uint8_t x_max    = (uint8_t)(num_x - 2u - dot_w);
    const uint8_t region_w = (uint8_t)(x_max - name_x);
    const display::Font* nf = nullptr;
    if      (display::text_width(display::FONT_NAME,    big) <= region_w) nf = &display::FONT_NAME;
    else if (display::text_width(display::FONT_NAME_MD, big) <= region_w) nf = &display::FONT_NAME_MD;
    else if (display::text_width(display::FONT_NAME_SM, big) <= region_w) nf = &display::FONT_NAME_SM;
    if (nf) {
        display::draw_text(*nf, name_x, (uint8_t)((RULE_Y - nf->height) / 2u), big);
    } else {
        const display::Font& f = display::FONT_NAME_SM;
        display::draw_text_clipped(f, name_x, (uint8_t)((RULE_Y - f.height) / 2u), big, x_max);
    }

    // Selected: photo-negate the header row from 2px past the icon to the right edge.
    if (m_focus == Focus::Preset)
        display::invert_region((uint8_t)(inset + 2u), 0u,
                               (uint8_t)(OLED_WIDTH - (inset + 2u)), RULE_Y);
}

// Context-line highlight band: the row the context name and page widget share, plus a
// 1px margin above and below (FONT_SMALL is 6px). A selected widget photo-negates its half.
static constexpr uint8_t CTX_ROW_Y = 17u;   // ALGO_Y - 1
static constexpr uint8_t CTX_ROW_H = 8u;

// Context name (left) + "Page X/Y" (right) sharing one row, split so the page widget
// always has room for its widest text. The ♪ status badge tucks in at the right of the
// name half. Selecting a widget photo-negates its half of the row. (Performance screen
// only; every product screen is a bespoke full-screen layout that never reaches here.)
void Compositor::draw_context_line()
{
    // The right of the row carries one of two things. Where the product has named what its
    // second footswitch does, that wins: the switch is labelled FUNCTION on the panel
    // precisely because its job changes with the algorithm, so the screen is the only place
    // that can say which job it has right now. Otherwise the page indicator has the space.
    const bool    fn      = m_function[0] != '\0';
    const uint8_t fn_w    = fn ? (uint8_t)(display::text_width(display::FONT_SMALL, m_function) + 2u) : 0u;
    const bool    multi   = !fn && m_num_pages > 1u;
    const uint8_t split_x = fn    ? (uint8_t)(OLED_WIDTH - fn_w)
                          : multi ? (uint8_t)(OLED_WIDTH - page_widget_width())
                                  : OLED_WIDTH;

    // The name fills the region left of the split (the whole row when no page widget).
    // Each badge takes the next 5px off the right of that region, plus a 1px gap, so
    // they stack leftwards from the split and the name simply gets less room.
    uint8_t name_right = split_x;   // exclusive right boundary of the name region
    const auto badge = [&](const uint8_t* icon) {
        if (name_right < 6u) return;   // no room left; the name wins the row
        name_right = (uint8_t)(name_right - 6u);
        display::draw_icon(name_right, (uint8_t)(ALGO_Y - 1u), 5u, 8u, icon);
    };
    if (m_status_badge) badge(ICON_NOTE);
    if (m_scene_badge)  badge(ICON_SCENE_B);
    display::draw_text_clipped(display::FONT_SMALL, 1, ALGO_Y, m_context_name,
                               (uint8_t)(name_right > 0u ? name_right - 1u : 0u));

    if (fn) {
        display::draw_text(display::FONT_SMALL, (uint8_t)(split_x + 1u), ALGO_Y, m_function);
    }
    // Page widget, left-aligned in its region with a 1px margin (only when >1 page).
    else if (multi) {
        char pg[PAGE_TEXT_BYTES];
        snprintf(pg, sizeof(pg), "Page %hu/%hu",
                 (unsigned short)(m_page + 1u), (unsigned short)m_num_pages);
        display::draw_text(display::FONT_SMALL, (uint8_t)(split_x + 1u), ALGO_Y, pg);
    }

    // Selected widget: photo-negate its half of the row.
    if (m_focus == Focus::Algo)
        display::invert_region(0, CTX_ROW_Y, split_x, CTX_ROW_H);
    else if (m_focus == Focus::Pages && multi)
        display::invert_region(split_x, CTX_ROW_Y, (uint8_t)(OLED_WIDTH - split_x), CTX_ROW_H);
}

// Knob-aligned columns: name (centered) / gauge / value (centered, single font).
void Compositor::draw_param_grid()
{
    for (uint8_t i = 0; i < m_layout.num_cols; ++i) {
        const uint8_t x0 = col_x(i);
        const uint8_t w  = m_layout.col_w;

        char nm[16];
        fit(display::FONT_TEXT, shorten_param_name(m_param_name[i]),
            (uint8_t)(w - 2u), nm, sizeof(nm));
        draw_centered(display::FONT_TEXT, x0, w, PNAME_Y, nm);

        if (m_param_bar[i] != NO_BAR)
            display::draw_gauge((uint8_t)(x0 + 2u), GAUGE_Y, (uint8_t)(w - 4u),
                                GAUGE_H, gauge_of(m_param_bar[i]));

        // A knob waiting to be picked up: a tick above the gauge marking where the pot
        // is pointing. The value stays where it is until the pot crosses it, so the gap
        // between tick and bar end is exactly how much further there is to turn.
        if (m_param_pickup[i] != NO_BAR)
            draw_pickup_tick((uint8_t)(x0 + 2u), (uint8_t)(w - 4u), PICKUP_Y,
                             m_param_pickup[i]);

        char val[16];
        to_display(val, m_param_val[i], sizeof(val) - 1u, true);
        if (val[0]) {
            if (display::text_width(display::FONT_TEXT, val) <= w - 2u) {
                draw_centered(display::FONT_TEXT, x0, w, PVAL_Y, val);
            } else {
                const uint8_t y = (uint8_t)(PVAL_Y + display::FONT_TEXT.ascent
                                                   - display::FONT_SMALL.ascent);
                draw_centered(display::FONT_SMALL, x0, w, y, val);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Titles and the shared name-editor screen
// ---------------------------------------------------------------------------

// Right-aligned "Page X/Y" chip: a fixed-width highlighted box (sized for the widest text)
// with the text left-aligned inside, so "Page " stays put as the numbers change.
void Compositor::draw_title_page_chip()
{
    const uint8_t w = page_widget_width();
    const uint8_t x = (uint8_t)(OLED_WIDTH - w);
    char pg[PAGE_TEXT_BYTES];
    snprintf(pg, sizeof(pg), "Page %hu/%hu",
             (unsigned short)(m_page + 1u), (unsigned short)m_num_pages);
    display::draw_text(display::FONT_SMALL, (uint8_t)(x + 1u), 4u, pg);        // left-aligned in the box
    display::invert_region(x, TITLE_Y, w, (uint8_t)(TITLE_RULE_Y - TITLE_Y));  // highlight the chip
}

void Compositor::draw_title(const char* title, bool with_page)
{
    uint8_t x_max = OLED_WIDTH;
    if (with_page) {
        draw_title_page_chip();
        x_max = (uint8_t)(OLED_WIDTH - page_widget_width() - 2u);  // keep the title clear of the chip
    }
    display::draw_text_clipped(display::FONT_NAME_SM, 2, TITLE_Y, title, x_max);
    display::draw_hline(TITLE_RULE_Y);
}

// The family name editor: the name centered with the cursor cell inverted and flanked
// by up/down spinners; knob P1 moves the cursor, P2 sets the character under it. The
// page chip is left off: the editor is one screen, and a chip reading "Page 1/1" is a
// widget that can only ever say the same thing.
void Compositor::draw_name_page()
{
    draw_title("Edit Name");

    char name_disp[sizeof(m_preset_name)];
    strncpy(name_disp, m_preset_name, sizeof(name_disp));
    name_disp[sizeof(name_disp) - 1u] = '\0';
    const uint8_t nw = display::text_width(display::FONT_NAME, name_disp);
    const uint8_t nx = (nw < OLED_WIDTH) ? (uint8_t)((OLED_WIDTH - nw) / 2u) : 0u;
    // Centre the name + spinners vertically between the header rule and the bottom hint.
    const uint8_t ny = 28u;
    display::draw_text(display::FONT_NAME, nx, ny, name_disp);

    char prefix[sizeof(m_preset_name)];
    strncpy(prefix, name_disp, m_name_cursor);
    prefix[m_name_cursor] = '\0';
    const uint8_t cx = (uint8_t)(nx + display::text_width(display::FONT_NAME, prefix));
    const char cc[2] = { name_disp[m_name_cursor], '\0' };
    const uint8_t cw = display::text_width(display::FONT_NAME, cc);
    display::fill_rect(cx, ny, cw ? cw : 4u, display::FONT_NAME.height, true);
    display::draw_text_inv(display::FONT_NAME, cx, ny, cc);

    const uint8_t arrow_cx = (uint8_t)(cx + (cw ? cw : 4u) / 2u);
    draw_tri(arrow_cx, (uint8_t)(ny - 5u), true);
    draw_tri(arrow_cx, (uint8_t)(ny + display::FONT_NAME.height + 1u), false);

    display::draw_text(display::FONT_SMALL, 0, HINT_Y, "P1=move  P2=char");
}

void Compositor::draw_normal(bool transient_owns_bottom_row)
{
    display::clear();
    if (m_screen != SCREEN_NORMAL) {
        draw_screen(m_screen);
        return;
    }
    draw_header(m_icon_now);
    draw_context_line();
    draw_param_grid();
    // What the two footswitches do, along the bottom edge: the first at the left and the
    // second at the right, so each label sits over the switch it names. Suppressed while a
    // transient banner or card owns this row; it returns once that clears. The two longest
    // names in the action vocabulary still fit side by side.
    if (!transient_owns_bottom_row) {
        if (m_switch_label[0][0])
            display::draw_text(display::FONT_SMALL, 0, HINT_Y, m_switch_label[0]);
        if (m_switch_label[1][0])
            display::draw_text_right(display::FONT_SMALL, OLED_WIDTH, HINT_Y,
                                     m_switch_label[1]);
    }
}

// ---------------------------------------------------------------------------
// Transient overlays
// ---------------------------------------------------------------------------

// The parameter focus panel a knob edit pops: a blind that unrolls downward from
// PANEL_Y over whatever draw_frame() has already rendered, giving the parameter its full
// name and a 17px value. Everything above PANEL_Y is left alone, so an edit reads as a
// zoom into one parameter rather than a change of screen: on the performance screen the
// player keeps the preset, the algorithm and the page in view throughout, and over a
// product screen the panel leaves that page's title bar showing.
//
// `prog` (0..256) is the shared overlay ramp. Only the rows the blind has reached are
// cleared, so the screen below the edge keeps showing through, and a text element is
// drawn only once the blind has passed its last row: there is no vertical clip
// primitive, so a half-revealed glyph would render whole and overhang the edge. The
// gauge takes an explicit height, so it grows out from under the blind instead.
void Compositor::draw_focus_panel(uint16_t prog)
{
    constexpr uint8_t BAND_H = (uint8_t)(OLED_HEIGHT - PANEL_Y);
    const uint8_t h      = (uint8_t)(1u + ((BAND_H - 1u) * prog) / 256u);  // 1 .. BAND_H
    const uint8_t bottom = (uint8_t)(PANEL_Y + h);                         // exclusive

    display::fill_rect(0, PANEL_Y, OLED_WIDTH, h, false);
    display::draw_hline(PANEL_Y);

    // The panel spans the full width, so the name needs none of the grid's abbreviations.
    if (bottom >= PANEL_NAME_Y + display::FONT_TEXT.height) {
        char nm[sizeof(m_panel_name)];
        fit(display::FONT_TEXT, m_panel_name, (uint8_t)(OLED_WIDTH - 6u), nm, sizeof(nm));
        display::draw_text(display::FONT_TEXT, 3, PANEL_NAME_Y, nm);
    }

    if (bottom >= PANEL_VAL_Y + display::FONT_NAME.height) {
        char cv[16];
        to_display(cv, m_panel_val, sizeof(cv) - 1u, false);
        draw_centered(display::FONT_NAME, 0, OLED_WIDTH, PANEL_VAL_Y, cv);
    }

    // The knob that popped this panel is still waiting to be picked up: the same tick the
    // grid draws, over the panel's wider track. It waits on the gauge's own condition, not
    // on its own two rows: the gauge grows out from under the blind and so reaches the
    // screen later than the rows above it, and a mark that means "how much further to
    // turn" says nothing without the bar it is measured against.
    if (m_panel_pickup != NO_BAR && bottom >= PANEL_GAUGE_Y + 3u)
        draw_pickup_tick(4, (uint8_t)(OLED_WIDTH - 8u), PANEL_PICKUP_Y, m_panel_pickup);

    // Below 3px the outline swallows the fill and the gauge reads as a plain line.
    if (m_panel_bar != NO_BAR && bottom >= PANEL_GAUGE_Y + 3u) {
        const uint8_t avail = (uint8_t)(bottom - PANEL_GAUGE_Y);
        const uint8_t gh    = (avail < PANEL_GAUGE_H) ? avail : PANEL_GAUGE_H;
        display::draw_gauge(4, PANEL_GAUGE_Y, (uint8_t)(OLED_WIDTH - 8u), gh,
                            gauge_of(m_panel_bar));
    }
}

// A system message over the normal screen. The default is a centered box that grows from a
// thin line (prog 0) to full height (prog 256), the text appearing once nearly open. The
// Bottom variant is a quiet one-line message along the bottom edge (FONT_SMALL at HINT_Y),
// clearing only that strip so it covers neither the param names nor the value row.
void Compositor::draw_banner(uint16_t prog)
{
    if (m_banner_bottom) {
        display::fill_rect(0, (uint8_t)(HINT_Y - 1u), OLED_WIDTH,
                           (uint8_t)(OLED_HEIGHT - (HINT_Y - 1u)), false);
        draw_centered(display::FONT_SMALL, 0, OLED_WIDTH, HINT_Y, m_banner);
        return;
    }
    const uint8_t tw = display::text_width(display::FONT_TEXT, m_banner);
    const uint8_t w  = (uint8_t)(tw + 10u);
    const uint8_t x  = (w < OLED_WIDTH) ? (uint8_t)((OLED_WIDTH - w) / 2u) : 0u;
    const uint8_t h  = (uint8_t)(3u + (13u * prog) / 256u);   // 3 .. 16
    const uint8_t y  = (uint8_t)(32u - h / 2u);
    display::fill_rect(x, y, w, h, false);
    rect_outline(x, y, w, h);
    if (prog >= 200u)
        display::draw_text(display::FONT_TEXT, (uint8_t)(x + 5u), 29u, m_banner);
}

// A 2px-thick checkmark with its corner near (x, y).
void Compositor::draw_check(uint8_t x, uint8_t y)
{
    for (uint8_t i = 0; i < 4u; ++i) {
        display::set_pixel((uint8_t)(x + i),      (uint8_t)(y + 4u + i), true);
        display::set_pixel((uint8_t)(x + i + 1u), (uint8_t)(y + 4u + i), true);
    }
    for (uint8_t i = 0; i < 8u; ++i) {
        display::set_pixel((uint8_t)(x + 3u + i), (uint8_t)(y + 7u - i), true);
        display::set_pixel((uint8_t)(x + 4u + i), (uint8_t)(y + 7u - i), true);
    }
}

// Save animation: a filling progress bar under "SAVING", then a checkmark + "SAVED".
void Compositor::draw_save(uint32_t elapsed)
{
    display::clear();
    if (elapsed < SAVE_SAVING_MS) {
        draw_centered(display::FONT_TEXT, 0, OLED_WIDTH, 14, "SAVING");
        const uint8_t v = (uint8_t)(elapsed * 127u / SAVE_SAVING_MS);
        display::draw_gauge(14, 36, (uint8_t)(OLED_WIDTH - 28u), 10u, v);
    } else {
        const uint8_t tw = display::text_width(display::FONT_NAME, "SAVED");
        const uint8_t x  = (uint8_t)((OLED_WIDTH - (tw + 18u)) / 2u);
        draw_check(x, 24u);
        display::draw_text(display::FONT_NAME, (uint8_t)(x + 18u), 22u, "SAVED");
    }
}

// ---------------------------------------------------------------------------
// Lifecycle and pacing
// ---------------------------------------------------------------------------

void Compositor::init()
{
    memset(m_param_name, 0, sizeof(m_param_name));
    memset(m_param_val,  0, sizeof(m_param_val));
    for (uint8_t i = 0; i < MAX_COLS; ++i) {
        snprintf(m_param_name[i], sizeof(m_param_name[i]), "P%d", i + 1);
        snprintf(m_param_val[i],  sizeof(m_param_val[i]),  "--");
        m_param_bar[i] = NO_BAR;
    }
    m_pacer = FramePacer{};
    display::init();
}

void Compositor::update(uint32_t now)
{
    const FramePacer::Decision d = m_pacer.decide(now, display::update_busy());

    // A slide needs the screen it is going to before it can composite towards it: render
    // the destination into the framebuffer and keep a copy.
    if (d.capture_slide_target) {
        m_icon_now = now;
        draw_normal();
        display::capture(m_slide_to);
    }

    switch (d.what) {
        case FramePacer::What::Nothing:
            return;

        case FramePacer::What::SplashFrame:
            display::clear();
            draw_splash_art(d.arg);
            break;

        case FramePacer::What::SplashRestart:
            // The storage-fault hold just ended, so the normal splash follows it: a
            // faulted boot runs Storage Fault -> Splash -> UI rather than jumping
            // straight to the UI. show_splash() puts its first frame up itself.
            show_splash();
            return;

        case FramePacer::What::SlideStep:
            display::compose_hslide(m_slide_from, m_slide_to,
                                    (uint8_t)(d.arg * OLED_WIDTH / FramePacer::SLIDE_MS),
                                    m_slide_dir);
            break;

        case FramePacer::What::SlideSettle:
            display::draw_framebuffer(m_slide_to);   // settle on the destination
            break;

        // The save animation owns the whole screen; the focus panel and the banner are
        // drawn over the normal screen (or over whichever product screen draw_normal()
        // dispatched to), so what they do not cover stays on view.
        case FramePacer::What::FrameSave:
            m_icon_now = now;
            draw_save(d.arg);
            break;

        case FramePacer::What::Frame:
        case FramePacer::What::FramePanel:
        case FramePacer::What::FrameBanner:
            m_icon_now = now;   // drives the header icon animation
            draw_normal(d.what != FramePacer::What::Frame);
            if      (d.what == FramePacer::What::FramePanel)  draw_focus_panel((uint16_t)d.arg);
            else if (d.what == FramePacer::What::FrameBanner) draw_banner((uint16_t)d.arg);
            break;
    }

    display::update_async();
}

// ---------------------------------------------------------------------------
// Producers
// ---------------------------------------------------------------------------

void Compositor::apply(const ScreenState& s)
{
    set_screen(s.screen);
    set_focus(s.focus);
    set_preset(s.preset);
    set_preset_name(s.preset_name);
    set_context_name(s.context_name);
    for (uint8_t i = 0; i < MAX_COLS; ++i) {
        set_param(i, s.param_name[i], s.param_val[i], s.param_bar[i]);
        set_param_pickup(i, s.param_pickup[i]);
    }
    set_page(s.page, s.num_pages);
    set_function_label(s.function);
    set_switch_labels(s.switch_label[0], s.switch_label[1]);
    set_name_cursor(s.name_cursor);
    set_save_prompt(s.save_prompt);
    set_status(s.status_badge);
    set_scene(s.scene_badge);
}

void Compositor::set_context_name(const char* name)
{
    if (assign(m_context_name, sizeof(m_context_name), name)) m_pacer.changed();
}

void Compositor::set_preset(uint16_t slot)
{
    if (m_preset != slot) { m_preset = slot; m_pacer.changed(); }
}

void Compositor::set_preset_name(const char* name)
{
    if (assign(m_preset_name, sizeof(m_preset_name), name)) m_pacer.changed();
}

void Compositor::set_status(bool badge_on)
{
    if (m_status_badge != badge_on) {
        m_status_badge = badge_on;
        m_pacer.changed();
    }
}

void Compositor::set_scene(bool badge_on)
{
    if (m_scene_badge != badge_on) {
        m_scene_badge = badge_on;
        m_pacer.changed();
    }
}

void Compositor::set_param(uint8_t slot, const char* name, const char* value_str, uint16_t bar)
{
    if (slot >= MAX_COLS) return;
    // A knob pushes its column every tick while it is being turned, so the redraw is
    // worth spending only when the label, the reading or the gauge actually moves.
    bool moved = assign(m_param_name[slot], sizeof(m_param_name[slot]), name);
    if (assign(m_param_val[slot], sizeof(m_param_val[slot]), value_str)) moved = true;
    if (m_param_bar[slot] != bar) { m_param_bar[slot] = bar; moved = true; }
    if (moved) m_pacer.changed();
}

void Compositor::set_param_pickup(uint8_t slot, uint16_t pot)
{
    if (slot >= MAX_COLS) return;
    if (m_param_pickup[slot] == pot) return;   // held every tick while armed; redraw once
    m_param_pickup[slot] = pot;
    m_pacer.changed();
}

void Compositor::set_switch_labels(const char* fs1, const char* fs2)
{
    const char* v[2] = { fs1, fs2 };
    bool moved = false;
    for (uint8_t i = 0; i < 2u; ++i)
        if (assign(m_switch_label[i], sizeof(m_switch_label[i]), v[i])) moved = true;
    // Pushed every tick, so the redraw is worth spending only when the words move.
    if (moved) m_pacer.changed();
}

void Compositor::set_function_label(const char* label)
{
    if (assign(m_function, sizeof(m_function), label)) m_pacer.changed();
}

void Compositor::set_page(uint8_t page, uint8_t num_pages)
{
    if (m_page == page && m_num_pages == num_pages) return;
    m_page      = page;
    m_num_pages = num_pages;
    m_pacer.changed();
}

void Compositor::set_focus(Focus f)
{
    if (m_focus != f) { m_focus = f; m_pacer.changed(); }
}

void Compositor::set_save_prompt(bool show)
{
    if (m_save_prompt != show) { m_save_prompt = show; m_pacer.changed(); }
}

void Compositor::set_screen(uint8_t screen_id)
{
    if (m_screen != screen_id) { m_screen = screen_id; m_pacer.changed(); }
}

void Compositor::set_name_cursor(uint8_t cursor)
{
    if (m_name_cursor != cursor) { m_name_cursor = cursor; m_pacer.changed(); }
}

void Compositor::show_message(const char* msg, MsgPos pos)
{
    strncpy(m_banner, msg, sizeof(m_banner) - 1);
    m_banner[sizeof(m_banner) - 1] = '\0';
    m_banner_bottom = (pos == MsgPos::Bottom);
    m_pacer.overlay(FramePacer::Overlay::Banner, systick::now_ms());
}

void Compositor::show_param_change(const char* name, const char* value, uint16_t bar,
                                   uint16_t pickup)
{
    strncpy(m_panel_name, name, sizeof(m_panel_name) - 1);
    m_panel_name[sizeof(m_panel_name) - 1] = '\0';
    strncpy(m_panel_val, value, sizeof(m_panel_val) - 1);
    m_panel_val[sizeof(m_panel_val) - 1] = '\0';
    m_panel_bar    = bar;
    m_panel_pickup = pickup;
    m_pacer.overlay(FramePacer::Overlay::Panel, systick::now_ms());
}

void Compositor::show_saved()
{
    m_pacer.overlay(FramePacer::Overlay::Save, systick::now_ms());
}

void Compositor::begin_slide(int8_t dir)
{
    // A splash or another slide in flight keeps the screen; otherwise capture the current
    // frame as the "from" and let the next update() render and slide in the "to".
    const FramePacer::SlideStart start = m_pacer.slide();
    if (start == FramePacer::SlideStart::Refused) return;
    // A transient overlay (the param focus panel / a banner) has been closed, so re-render
    // the underlying screen for the slide to capture. The state behind it is still current
    // here — begin_slide runs before the algorithm/preset/page actually changes — so this
    // captures the correct "from" frame and the player sees a clean slide to the new screen.
    if (start == FramePacer::SlideStart::RedrawFirst) draw_normal();
    display::capture(m_slide_from);
    m_slide_dir = dir;
}

void Compositor::show_splash()
{
    const uint32_t now = systick::now_ms();
    display::clear();
    draw_splash_art(0);       // first frame up immediately; the superloop animates the rest
    display::update();
    m_pacer.splash(now);
}

// Drawn with the 6x8 page font (like a bootloader's DFU screen), not the proportional
// fonts — the message is fixed and every line fits the 128 px width.
void Compositor::draw_storage_fault_screen()
{
    display::clear();
    display::draw_text(0, 0, "Storage Fault");
    display::draw_hline(8);                       // rule under the title
    display::draw_text(0, 2, "EEPROM not responding");
    display::draw_text(0, 4, "Presets loaded to RAM");
    display::draw_text(0, 5, "Saves are temporary");
    display::draw_text(0, 7, "Starting anyway...");
}

void Compositor::show_storage_fault()
{
    // A static hold on the same timer the splash uses, with the splash queued behind it,
    // so the superloop reveals the normal screen only after both have had their turn.
    draw_storage_fault_screen();
    display::update();
    m_pacer.fault_hold(systick::now_ms());
}

}  // namespace pedal_core::ui
