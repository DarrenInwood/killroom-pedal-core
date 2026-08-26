#include <pedal_core/footswitch.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_ui_config.hpp"   // FOOTSWITCH_DEBOUNCE_MS / FOOTSWITCH_HOLD_MS

static constexpr uint8_t EVENT_BUF = 8;

static footswitch::Event s_events[EVENT_BUF];
static uint8_t s_ev_head = 0;
static uint8_t s_ev_tail = 0;

static void push_event(footswitch::Event e)
{
    const uint8_t next = (s_ev_head + 1u) & (EVENT_BUF - 1u);
    if (next != s_ev_tail) { s_events[s_ev_head] = e; s_ev_head = next; }
}

// Per-switch event triples, indexed by switch. Keeping them in one table is what
// lets update() treat the two switches as the same code path rather than two.
static constexpr footswitch::Event k_press[2] = {
    footswitch::Event::FS1_Press, footswitch::Event::FS2_Press };
static constexpr footswitch::Event k_hold[2] = {
    footswitch::Event::FS1_Hold, footswitch::Event::FS2_Hold };
static constexpr footswitch::Event k_hold_release[2] = {
    footswitch::Event::FS1_HoldRelease, footswitch::Event::FS2_HoldRelease };

struct SwState {
    bool     raw        = false;
    bool     debounced  = false;
    bool     pressed    = false;   // currently held after debounce
    bool     hold_fired = false;
    uint32_t change_ms  = 0;
    uint32_t press_ms   = 0;
};

static SwState s_sw[2];

void footswitch::init()
{
    pedal_core::hal::footswitch_pins_init();
}

void footswitch::update()
{
    const uint32_t now = systick::now_ms();

    // Each switch is decided entirely on its own state. Neither the debounce nor
    // the hold consults the other switch, so both feet down is simply both
    // switches reporting, in whatever order they settle.
    for (uint8_t i = 0; i < 2; ++i) {
        SwState& sw = s_sw[i];
        const bool raw = pedal_core::hal::fs_pressed(i);

        if (raw != sw.raw) {
            sw.raw       = raw;
            sw.change_ms = now;
        }

        if ((now - sw.change_ms) >= FOOTSWITCH_DEBOUNCE_MS && raw != sw.debounced) {
            sw.debounced = raw;
            if (raw) {
                sw.pressed    = true;
                sw.hold_fired = false;
                sw.press_ms   = now;
            } else {
                // A release closes whichever gesture was open: the hold it
                // started, or — if the foot came off before the hold window —
                // a short press. Never both.
                if (sw.pressed)
                    push_event(sw.hold_fired ? k_hold_release[i] : k_press[i]);
                sw.pressed = false;
            }
        }

        // The hold fires once, mid-press, so a momentary action engages while the
        // foot is still down rather than waiting for it to lift.
        if (sw.pressed && !sw.hold_fired &&
            (now - sw.press_ms) >= FOOTSWITCH_HOLD_MS)
        {
            sw.hold_fired = true;
            push_event(k_hold[i]);
        }
    }
}

uint32_t footswitch::down_for_ms(uint8_t sw)
{
    if (sw >= 2u || !s_sw[sw].pressed) return 0u;
    return systick::now_ms() - s_sw[sw].press_ms;
}

bool footswitch::has_event()
{
    return s_ev_head != s_ev_tail;
}

footswitch::Event footswitch::get_event()
{
    if (s_ev_head == s_ev_tail) return Event::None;
    const Event e = s_events[s_ev_tail];
    s_ev_tail = (s_ev_tail + 1u) & (EVENT_BUF - 1u);
    return e;
}
