#if __has_include("pedal_core_extinput_config.hpp")

#include <pedal_core/external_input.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_ui_config.hpp"   // FOOTSWITCH_DEBOUNCE_MS

// Footswitch mode debounces the *combined* (tip, ring) contact state as a single 4-position
// switch — none / tip / ring / both — so a three-button footswitch (third switch diode-ORed
// onto both contacts) reports a distinct Both press. Mirrors the front-panel debounce timing
// (FOOTSWITCH_DEBOUNCE_MS) but emits one press per gesture, on release.

namespace {

external_input::Mode   s_mode = external_input::Mode::Expression;
external_input::Action s_action[3] = {
    (external_input::Action)EXT_DEFAULT_TIP_ACTION,
    (external_input::Action)EXT_DEFAULT_RING_ACTION,
    (external_input::Action)EXT_DEFAULT_BOTH_ACTION,
};

constexpr uint8_t EVENT_BUF = 8;  // power of two
external_input::Event s_events[EVENT_BUF];
uint8_t s_ev_head = 0;
uint8_t s_ev_tail = 0;

void push_event(external_input::Event e)
{
    const uint8_t next = (uint8_t)((s_ev_head + 1u) & (EVENT_BUF - 1u));
    if (next != s_ev_tail) { s_events[s_ev_head] = e; s_ev_head = next; }
}

// Combined contact position: bit0 = tip, bit1 = ring → 0 none, 1 tip, 2 ring, 3 both.
uint8_t s_raw_pos   = 0;  // last raw sample
uint8_t s_deb_pos   = 0;  // debounced current position
uint8_t s_armed_pos = 0;  // strongest position seen since leaving idle (Both sticks)
uint32_t s_change_ms = 0;

void reset_debounce()
{
    s_raw_pos = s_deb_pos = s_armed_pos = 0u;
    s_change_ms = 0u;
    s_ev_head = s_ev_tail = 0u;
}

uint8_t read_position()
{
    const bool tip  = pedal_core::hal::ext_tip_pressed();
    const bool ring = pedal_core::hal::ext_ring_pressed();
    return (uint8_t)((tip ? 1u : 0u) | (ring ? 2u : 0u));
}

}  // namespace

void external_input::set_mode(Mode mode)
{
    s_mode = mode;
    reset_debounce();
    // Pin directions, pulls and the expression reference are the product's;
    // the hal maps the logical mode onto them.
    pedal_core::hal::ext_input_pins_mode(mode == Mode::Footswitch);
}

void external_input::init()
{
    set_mode(Mode::Expression);
}

external_input::Mode external_input::mode()
{
    return s_mode;
}

void external_input::set_action(Switch sw, Action a)
{
    if ((uint8_t)sw < 3u && (uint8_t)a < (uint8_t)Action::Count)
        s_action[(uint8_t)sw] = a;
}

external_input::Action external_input::action(Switch sw)
{
    return ((uint8_t)sw < 3u) ? s_action[(uint8_t)sw] : Action::None;
}

void external_input::update()
{
    if (s_mode != Mode::Footswitch) return;

    const uint32_t now = systick::now_ms();
    const uint8_t  raw = read_position();

    if (raw != s_raw_pos) {
        s_raw_pos   = raw;
        s_change_ms = now;
    }

    if ((now - s_change_ms) >= FOOTSWITCH_DEBOUNCE_MS && raw != s_deb_pos) {
        s_deb_pos = raw;
        if (raw != 0u) {
            // Entering / escalating a press. 'Both' (3) is the strongest position and sticks once
            // seen, so a three-button press whose contacts settle a few ms apart still reports
            // Both rather than a stray Tip/Ring.
            if (raw == 3u)            s_armed_pos = 3u;
            else if (s_armed_pos != 3u) s_armed_pos = raw;
        } else {
            // Released: emit the press for the strongest position held during the gesture.
            switch (s_armed_pos) {
                case 1u: push_event(Event::TipPress);  break;
                case 2u: push_event(Event::RingPress); break;
                case 3u: push_event(Event::BothPress); break;
                default: break;
            }
            s_armed_pos = 0u;
        }
    }
}

bool external_input::has_event()
{
    return s_ev_head != s_ev_tail;
}

external_input::Event external_input::get_event()
{
    if (s_ev_head == s_ev_tail) return Event::None;
    const Event e = s_events[s_ev_tail];
    s_ev_tail = (uint8_t)((s_ev_tail + 1u) & (EVENT_BUF - 1u));
    return e;
}

#endif  // __has_include(pedal_core_extinput_config.hpp)
