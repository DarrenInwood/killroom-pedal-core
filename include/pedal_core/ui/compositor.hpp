#pragma once
#include <cstdint>
#include "../action.hpp"
#include "../display.hpp"
#include "frame_pacer.hpp"
#include "pedal_core_config.hpp"   // OLED_*

// The family compositor: one full-frame redraw of the performance screen from
// current state, a pacing state machine (idle cap, animation cadence, slide
// transitions, splash and fault holds), and the transient overlays (the param
// focus panel that unrolls over the grid, the growing-box banner, the save
// animation).
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

namespace action = pedal_core::action;

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

    // Longest label the context row and the switch row will hold, in characters: the
    // longest name in the family's action vocabulary with a character to spare. Derived
    // rather than written down, so an action whose name outgrows the row widens the row
    // instead of being truncated silently — truncation reads on the pedal as a
    // misspelling rather than as a layout problem.
    static constexpr uint8_t FUNCTION_LABEL_MAX = (uint8_t)(action::LONGEST_NAME + 1u);

    explicit Compositor(const LayoutSpec& layout) : m_layout(layout) {}

    void init();
    void update(uint32_t now);

    // The performance screen as one value.
    //
    // Everything the fourteen setters below write, in one place, so a producer can say
    // what the screen shows in a single call instead of learning an interface point per
    // field — and so a field added here costs a caller nothing.
    //
    // This is what a producer hands over, not a second copy the compositor keeps: apply()
    // diffs it against the state already held and the struct is the caller's, so the
    // pedal spends no extra RAM on it.
    struct ScreenState {
        uint8_t  screen = SCREEN_NORMAL;
        Focus    focus  = Focus::Pages;
        uint16_t preset = 0;
        char     preset_name[17]  = "";
        char     context_name[20] = "";
        char     param_name[MAX_COLS][20] = {};
        char     param_val [MAX_COLS][16] = {};
        uint16_t param_bar [MAX_COLS] = { NO_BAR, NO_BAR, NO_BAR, NO_BAR };
        uint16_t param_pickup[MAX_COLS] = { NO_BAR, NO_BAR, NO_BAR, NO_BAR };
        uint8_t  page = 0, num_pages = 1;
        char     function[FUNCTION_LABEL_MAX + 1u] = "";
        char     switch_label[2][FUNCTION_LABEL_MAX + 1u] = {};
        uint8_t  name_cursor = 0;
        bool     save_prompt = false;
        bool     status_badge = false;
        bool     scene_badge  = false;
    };

    // Say what the screen shows, whole. Each field is compared against what is already
    // held and the frame is marked dirty only where something a pixel depends on actually
    // moved, so a producer can push the same state every tick and pay for a redraw only
    // when the screen has something new to say.
    //
    // It runs through the setters below rather than beside them, so there is one place
    // per field that decides what a change is and the two ways in cannot drift.
    virtual void apply(const ScreenState& s);

    // --- state the producers push (each marks the frame dirty where it moves) ---
    virtual void set_context_name(const char* name);   // the algorithm's name
    virtual void set_preset(uint16_t slot);
    virtual void set_preset_name(const char* name);
    virtual void set_status(bool badge_on);            // header ♪ badge (tempo slaved)
    virtual void set_scene(bool badge_on);              // context-line B badge (Scene B sounding)
    virtual void set_param(uint8_t slot, const char* name, const char* value_str,
                           uint16_t bar = NO_BAR);
    // Where the pot itself is pointing while a knob waits to be picked up, on the
    // parameter scale. The gauge shows the value; this shows the pot, so the gap between
    // the two is the distance left to turn. NO_BAR while the knob is live and the gauge
    // already tells the whole story.
    virtual void set_param_pickup(uint8_t slot, uint16_t pot);
    virtual void set_page(uint8_t page, uint8_t num_pages);
    // What the product's second footswitch does on the running algorithm, for the right of
    // the context row. Empty gives the row back to the page indicator — the two share the
    // space because they are never both worth saying: a page number means nothing where the
    // knobs are not walking pages, and the switch's job is the thing a foot needs to know.
    virtual void set_function_label(const char* label);
    // What the two footswitches do right now, along the bottom edge: the first at the
    // left and the second at the right, each over the switch it names. A product that
    // pushes the hold actions while a foot is down turns the row into a preview of what
    // keeping it down will do. Either label empty simply leaves that side blank.
    virtual void set_switch_labels(const char* fs1, const char* fs2);
    virtual void set_focus(Focus f);
    virtual void set_save_prompt(bool show);
    virtual void set_screen(uint8_t screen_id);
    virtual void set_name_cursor(uint8_t cursor);

    uint8_t screen() const { return m_screen; }

    // --- transients ---------------------------------------------------------
    // `pickup` is where the pot is pointing when the panel answers a turn of a knob still
    // waiting to be picked up, drawn as the tick above the gauge. NO_BAR everywhere else:
    // the mark says which way to keep turning *this* knob, so it belongs to the gesture of
    // turning it rather than to every way a value can move.
    virtual void show_param_change(const char* name, const char* value, uint16_t bar,
                                   uint16_t pickup = NO_BAR);
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
    // Where a pot is pointing while its knob waits to be picked up: a tick on row `y`,
    // over a gauge track `track_w` wide starting at `x0`. The travel is inset by the
    // tick's own width, so a pot at full scale lands inside the track's right end rather
    // than half off it. The grid column and the panel differ only in those two numbers.
    static void draw_pickup_tick(uint8_t x0, uint8_t track_w, uint8_t y, uint16_t pot);
    static void draw_tri(uint8_t cx, uint8_t y, bool up);
    static void fit(const display::Font& f, const char* src, uint8_t max_px,
                    char* dst, uint8_t dstsz);
    static void to_display(char* dst, const char* src, uint8_t maxlen, bool strip_spaces);

    // Copy `src` into `dst`, truncated to its capacity, and say whether the stored text
    // moved. The comparison is against the truncated form, so two names that differ only
    // past the end of the buffer are the same screen and cost no redraw.
    static bool assign(char* dst, uint16_t cap, const char* src);
    static uint8_t page_widget_width();
    void draw_title(const char* title, bool with_page = false);
    void draw_name_page();               // the family name editor (Edit Name screen)

    // Mark the frame dirty after a change to device-side state the base
    // cannot see (a product screen's own readouts).
    void mark_dirty() { m_pacer.changed(); }

    // Layout rows shared by the performance grid and the product screens.
    static constexpr uint8_t RULE_Y  = 17u;
    static constexpr uint8_t ALGO_Y  = 18u;
    static constexpr uint8_t PNAME_Y = 27u;
    static constexpr uint8_t GAUGE_Y = 40u;
    static constexpr uint8_t GAUGE_H = 5u;
    // The pickup tick sits in the gap between the parameter name and its gauge, so it
    // never disappears into the filled part of the bar it is being compared against.
    static constexpr uint8_t PICKUP_Y = 38u;
    static constexpr uint8_t PICKUP_H = 2u;
    static constexpr uint8_t PICKUP_W = 2u;
    static constexpr uint8_t PVAL_Y  = 46u;
    static constexpr uint8_t HINT_Y  = 58u;
    static constexpr uint8_t TITLE_Y = 1u, TITLE_RULE_Y = 13u;

    // The param focus panel: a blind that unrolls over the grid, leaving the header
    // and context row in place so an edit never costs the player their bearings.
    //
    // PANEL_Y clears the deepest row any screen's first text line reaches: a product page
    // drawing FONT_TEXT at y=16 fills rows 16..26, so a rule one row higher would shave the
    // descenders off its top line. The performance grid's own name row starts painting at
    // y=28, so nothing above the rule is lost there either.
    static constexpr uint8_t PANEL_Y        = 27u;  // separator rule; the panel owns 27..63
    static constexpr uint8_t PANEL_NAME_Y   = 28u;  // FONT_TEXT (11px) -> 28..38
    static constexpr uint8_t PANEL_VAL_Y    = 40u;  // FONT_NAME (17px) -> 40..56
    // The panel carries the pickup tick in the grid's arrangement -- the mark sitting on
    // the bar it is read against -- so a player who has read one gauge can read the other
    // without learning a second kind of mark. Row 56 is the value's last, so the tick
    // takes 57..58 and the gauge gives up the pixel to make room for it.
    static constexpr uint8_t PANEL_PICKUP_Y = 57u;  // -> 57..58, PICKUP_H tall
    static constexpr uint8_t PANEL_GAUGE_Y  = 59u;
    static constexpr uint8_t PANEL_GAUGE_H  = 5u;   // -> 59..63, flush to the bottom edge

    // The ± glyph occupies the slot just past '~' in every font.
    static constexpr char GLYPH_PM = (char)0x7F;

    // --- state the hooks read ------------------------------------------------
    LayoutSpec m_layout;
    uint8_t    m_screen = SCREEN_NORMAL;
    Focus      m_focus  = Focus::Pages;
    uint16_t   m_preset = 0;
    char       m_preset_name[17]  = "";
    char       m_context_name[20] = "";
    // A slot label is a knob's parameter on the grid, a settings row in the tree and a whole
    // algorithm name in a browser list, so it is sized for the longest of those.
    char       m_param_name[MAX_COLS][20] = {};
    char       m_param_val [MAX_COLS][16] = {};
    uint16_t   m_param_bar [MAX_COLS] = { NO_BAR, NO_BAR, NO_BAR, NO_BAR };
    uint16_t   m_param_pickup[MAX_COLS] = { NO_BAR, NO_BAR, NO_BAR, NO_BAR };
    uint8_t    m_page = 0, m_num_pages = 1;
    char       m_function[FUNCTION_LABEL_MAX + 1u] = "";
    char       m_switch_label[2][FUNCTION_LABEL_MAX + 1u] = {};
    uint8_t    m_name_cursor = 0;
    bool       m_save_prompt = false;
    bool       m_status_badge = false;
    bool       m_scene_badge  = false;

private:
    void draw_normal(bool transient_owns_bottom_row = false);
    void draw_header(uint32_t now);
    void draw_context_line();
    void draw_param_grid();
    void draw_focus_panel(uint16_t prog);
    void draw_banner(uint16_t prog);
    void draw_save();
    void draw_title_page_chip();
    static void draw_check(uint8_t x, uint8_t y);

    // When a frame is due and what belongs on it. Every timing constant is the pacer's;
    // this class paints what it is told to.
    FramePacer m_pacer;

    // What the transients say, held for the frame that draws them.
    char     m_panel_name[16] = {};
    char     m_panel_val [16] = {};
    uint16_t m_panel_bar = NO_BAR;
    uint16_t m_panel_pickup = NO_BAR;
    char     m_banner[32] = {};
    bool     m_banner_bottom = false;

    // The two frames a transition composites, and which way it runs.
    int8_t   m_slide_dir = 1;
    uint8_t  m_slide_from[OLED_PAGES][OLED_WIDTH];
    uint8_t  m_slide_to  [OLED_PAGES][OLED_WIDTH];

    uint32_t m_icon_now = 0;
};

}  // namespace pedal_core::ui
