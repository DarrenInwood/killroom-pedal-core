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

    // Pass 1: update all debounce states so both pressed flags are current
    // before the hold-interaction check in pass 2.
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
                if (sw.pressed && !sw.hold_fired) {
                    // Short press: emit only if the other switch isn't held —
                    // both-held belongs to pass 2.
                    const bool other_pressed = s_sw[1 - i].pressed;
                    if (!other_pressed) {
                        push_event(i == 0 ? Event::FS1_Press : Event::FS2_Press);
                    }
                }
                sw.pressed = false;
            }
        }
    }

    // Pass 2: hold detection. Both pressed flags reflect this tick's state, so
    // the Both_Hold check is accurate even when both switches debounce on the
    // same tick.
    //
    // Both_Hold needs both switches held *simultaneously* for
    // FOOTSWITCH_HOLD_MS, timed from the later of the two presses — the start
    // of the overlap. A save is a deliberate two-foot hold: a switch that
    // joins late must itself be held for the full window, so a brief overlap
    // never saves.
    if (s_sw[0].pressed && s_sw[1].pressed &&
        !s_sw[0].hold_fired && !s_sw[1].hold_fired)
    {
        const uint32_t overlap_start = (s_sw[0].press_ms > s_sw[1].press_ms)
                                     ? s_sw[0].press_ms : s_sw[1].press_ms;
        if ((now - overlap_start) >= FOOTSWITCH_HOLD_MS) {
            s_sw[0].hold_fired = true;
            s_sw[1].hold_fired = true;
            push_event(Event::Both_Hold);
        }
    }

    // An individual hold fires only while its switch is held alone — with the
    // other down, the gesture belongs to the Both_Hold check above.
    for (uint8_t i = 0; i < 2; ++i) {
        SwState& sw = s_sw[i];
        if (sw.pressed && !sw.hold_fired && !s_sw[1 - i].pressed &&
            (now - sw.press_ms) >= FOOTSWITCH_HOLD_MS)
        {
            sw.hold_fired = true;
            push_event(i == 0 ? Event::FS1_Hold : Event::FS2_Hold);
        }
    }
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
