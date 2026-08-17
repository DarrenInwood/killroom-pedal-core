#pragma once
#include <cstdint>
#include "../display.hpp"
#include "pedal_core_config.hpp"   // OLED_*

// The family compositor: one full-frame redraw of the performance screen from
// current state, a pacing state machine (idle cap, animation cadence, slide
// transitions, splash and fault holds), and the transient overlays (the param
// focus card with its iris, the growing-box banner, the save animation).
//
// A product subclasses it with a LayoutSpec (how many knob columns, how wide)
// and a handful of hooks: its animated header icon, its param-name
// abbreviations, its splash art, and its bespoke full screens (settings pages,
// calibration wizards, the name editor's siblings). Everything the hooks draw
// composes with protected helpers over the same state the base renders from,
// so a product screen cannot show a value the performance grid would not.
//
// Public mutators are virtual so a suite can drive real machinery behind a
// recording subclass.
namespace pedal_core::ui {

class Compositor {
public:
    // Slots a producer can push without a gauge.
    static constexpr uint16_t NO_BAR = 0xFFFFu;

    // The performance screen. Every other screen id is the product's own,
    // rendered by its draw_screen() hook.
    static constexpr uint8_t SCREEN_NORMAL = 0u;

    // Which header widget is highlighted on the performance screen.
    enum class Focus : uint8_t { Pages = 0, Algo = 1, Preset = 2 };

    // Message placement: a centered animated box, or a quiet one-line strip
    // along the bottom edge.
    enum class MsgPos : uint8_t { Centre = 0, Bottom = 1 };

    struct LayoutSpec {
        uint8_t num_cols;   // knob columns on the performance grid (<= MAX_COLS)
        uint8_t col_w;      // column width in px
        uint8_t col_x0;     // x of the first column
    };

    static constexpr uint8_t MAX_COLS = 4u;

    explicit Compositor(const LayoutSpec& layout) : m_layout(layout) {}

    void init();
    void update(uint32_t now);

    // --- state the producers push (each marks the frame dirty) --------------
    virtual void set_context_name(const char* name);   // the algorithm's name
    virtual void set_preset(uint16_t slot);
    virtual void set_preset_name(const char* name);
    virtual void set_status(bool badge_on);            // header ♪ badge (tempo slaved)
    virtual void set_param(uint8_t slot, const char* name, const char* value_str,
                           uint16_t bar = NO_BAR);
    virtual void set_page(uint8_t page, uint8_t num_pages);
    virtual void set_focus(Focus f);
    virtual void set_save_prompt(bool show);
    virtual void set_screen(uint8_t screen_id);
    virtual void set_name_cursor(uint8_t cursor);

    uint8_t screen() const { return m_screen; }

    // --- transients ---------------------------------------------------------
    virtual void show_param_change(const char* name, const char* value, uint16_t bar);
    virtual void show_message(const char* msg, MsgPos pos = MsgPos::Centre);
    virtual void show_saved();
    virtual void show_splash();
    virtual void show_storage_fault();
    virtual void begin_slide(int8_t dir);

protected:
    virtual ~Compositor() = default;

    // --- the product hooks --------------------------------------------------
    // Draw the animated header icon for `now` and return the width it consumed
    // (the header name region starts past it). A product without one returns 0.
    virtual uint8_t draw_header_icon(uint32_t now) { (void)now; return 0u; }

    // Abbreviate a param name for the compact grid column; identity by default.
    virtual const char* shorten_param_name(const char* name) const { return name; }

    // One of the product's bespoke full screens (screen_id != SCREEN_NORMAL).
    virtual void draw_screen(uint8_t screen_id) = 0;

    // The product's splash art, drawn each animation frame from `elapsed` ms.
    virtual void draw_splash_art(uint32_t elapsed) = 0;

    // The storage-fault screen. The default names the EEPROM and the RAM
    // fallback in the fixed 6x8 page font; a product can redraw it wholesale.
    virtual void draw_storage_fault_screen();

    // --- helpers for the product's screens ----------------------------------
    uint8_t col_x(uint8_t col) const { return (uint8_t)(m_layout.col_x0 + col * m_layout.col_w); }

    // A parameter value as the 0-127 fraction display::draw_gauge() expects. The
    // driver primitive is deliberately scale-free, so the conversion lives here
    // rather than spreading the parameter scale into the display layer.
    static uint8_t gauge_of(uint16_t v)
    {
        if (v > PARAM_MAX) v = PARAM_MAX;
        return (uint8_t)(((uint32_t)v * 127u) / PARAM_MAX);
    }
    static void draw_centered(const display::Font& f, uint8_t x0, uint8_t w,
                              uint8_t y, const char* s);
    static void rect_outline(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
    static void draw_tri(uint8_t cx, uint8_t y, bool up);
    static void fit(const display::Font& f, const char* src, uint8_t max_px,
                    char* dst, uint8_t dstsz);
    static void to_display(char* dst, const char* src, uint8_t maxlen, bool strip_spaces);
    static uint8_t page_widget_width();
    void draw_title(const char* title, bool with_page = false);
    void draw_name_page();               // the family name editor (Edit Name screen)

    // Mark the frame dirty after a change to device-side state the base
    // cannot see (a product screen's own readouts).
    void mark_dirty() { m_dirty = true; }

    // Layout rows shared by the performance grid and the product screens.
    static constexpr uint8_t RULE_Y  = 17u;
    static constexpr uint8_t ALGO_Y  = 18u;
    static constexpr uint8_t PNAME_Y = 27u;
    static constexpr uint8_t GAUGE_Y = 40u;
    static constexpr uint8_t GAUGE_H = 5u;
    static constexpr uint8_t PVAL_Y  = 46u;
    static constexpr uint8_t HINT_Y  = 58u;
    static constexpr uint8_t TITLE_Y = 1u, TITLE_RULE_Y = 13u;

    // The ± glyph occupies the slot just past '~' in every font.
    static constexpr char GLYPH_PM = (char)0x7F;

    // --- state the hooks read ------------------------------------------------
    LayoutSpec m_layout;
    uint8_t    m_screen = SCREEN_NORMAL;
    Focus      m_focus  = Focus::Pages;
    uint16_t   m_preset = 0;
    char       m_preset_name[17]  = "";
    char       m_context_name[20] = "";
    char       m_param_name[MAX_COLS][14] = {};
    char       m_param_val [MAX_COLS][16] = {};
    uint16_t   m_param_bar [MAX_COLS] = { NO_BAR, NO_BAR, NO_BAR, NO_BAR };
    uint8_t    m_page = 0, m_num_pages = 1;
    uint8_t    m_name_cursor = 0;
    bool       m_save_prompt = false;
    bool       m_status_badge = false;

private:
    void draw_normal();
    void draw_header(uint32_t now);
    void draw_context_line();
    void draw_param_grid();
    void draw_focus_card();
    void draw_banner(uint16_t prog);
    void draw_save(uint32_t elapsed);
    void draw_frame(uint32_t now);
    void draw_title_page_chip();
    uint16_t overlay_prog(uint32_t elapsed) const;
    uint32_t overlay_total() const;
    bool overlay_animating(uint32_t elapsed) const;
    static void band_clip(uint8_t half);
    static void draw_check(uint8_t x, uint8_t y);

    // Pacing.
    static constexpr uint32_t REFRESH_MS     = 50u;
    static constexpr uint32_t ANIM_FRAME_MS  = 16u;
    static constexpr uint32_t ANIM_MS        = 140u;
    static constexpr uint32_t SAVE_SAVING_MS = 500u;
    static constexpr uint32_t SAVE_TOTAL_MS  = 1200u;
    static constexpr uint32_t SLIDE_MS       = 150u;
    static constexpr uint32_t SPLASH_MS      = 2000u;
    static constexpr uint32_t FAULT_HOLD_MS  = 2500u;

    enum class Overlay : uint8_t { None, Card, Banner, Save };
    Overlay  m_overlay = Overlay::None;
    uint32_t m_overlay_start_ms = 0;
    char     m_card_name[16] = {};
    char     m_card_val [16] = {};
    uint16_t m_card_bar = NO_BAR;
    char     m_banner[32] = {};
    bool     m_banner_bottom = false;

    bool     m_slide_pending = false;
    bool     m_slide_active  = false;
    int8_t   m_slide_dir     = 1;
    uint32_t m_slide_start_ms = 0;
    uint8_t  m_slide_from[OLED_PAGES][OLED_WIDTH];
    uint8_t  m_slide_to  [OLED_PAGES][OLED_WIDTH];

    bool     m_dirty = true;
    uint32_t m_last_draw_ms = 0;
    uint32_t m_icon_now = 0;

    uint32_t m_splash_expiry_ms = 0;
    uint32_t m_splash_start_ms  = 0;
    bool     m_splash_active    = false;
    bool     m_splash_after_hold = false;
};

}  // namespace pedal_core::ui
