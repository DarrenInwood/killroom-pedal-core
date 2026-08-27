#if __has_include("pedal_core_extinput_config.hpp")

#include <pedal_core/external_input.hpp>
#include <pedal_core/hal.hpp>
#include "pedal_core_ui_config.hpp"   // FOOTSWITCH_DEBOUNCE_MS, FOOTSWITCH_HOLD_MS

// Footswitch mode debounces the *combined* (tip, ring) contact state as a single 4-position
// switch — none / tip / ring / both — so a three-button footswitch (third switch diode-ORed
// onto both contacts) reports a distinct Both press. Timing is the front panel's
// (FOOTSWITCH_DEBOUNCE_MS, FOOTSWITCH_HOLD_MS), and so is the gesture grammar: a hold fires
// mid-press, a press is emitted on the release and only where no hold fired.

namespace {

external_input::Mode   s_mode = external_input::Mode::Expression;
// [switch][gesture], gesture 0 = press, 1 = hold. A hold defaults to nothing: the jack is
// whatever its owner wires to it, and inventing a gesture they did not ask for is how a
// footswitch surprises somebody mid-set.
external_input::Action s_action[3][2] = {
    { (external_input::Action)EXT_DEFAULT_TIP_ACTION,  external_input::Action::None },
    { (external_input::Action)EXT_DEFAULT_RING_ACTION, external_input::Action::None },
    { (external_input::Action)EXT_DEFAULT_BOTH_ACTION, external_input::Action::None },
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
uint8_t  s_raw_pos    = 0;  // last raw sample
uint8_t  s_deb_pos    = 0;  // debounced current position
uint8_t  s_armed_pos  = 0;  // strongest position seen since leaving idle (Both sticks)
uint32_t s_change_ms  = 0;
uint32_t s_press_ms   = 0;  // when the current gesture started, for the hold threshold
bool     s_hold_fired = false;
// Which contacts have been observed open since the mode was entered. A contact that never
// has is not a switch somebody is standing on -- it is a TS plug shorting the ring to
// sleeve, or a switch wired closed -- so it is masked out rather than read as a press.
// Without this a TS plug would engage the ring's hold action and never let go of it.
uint8_t  s_seen_open  = 0;

void reset_debounce()
{
    s_raw_pos = s_deb_pos = s_armed_pos = 0u;
    s_change_ms = s_press_ms = 0u;
    s_hold_fired = false;
    s_seen_open = 0u;
    s_ev_head = s_ev_tail = 0u;
}

// Sample both contacts, learning which of them this cable can actually open. Mutating on a
// read is deliberate: a contact proves itself a switch by being open once, and there is no
// other moment to notice.
uint8_t sample_position()
{
    const bool tip  = pedal_core::hal::ext_tip_pressed();
    const bool ring = pedal_core::hal::ext_ring_pressed();
    const uint8_t raw = (uint8_t)((tip ? 1u : 0u) | (ring ? 2u : 0u));
    s_seen_open |= (uint8_t)(~raw & 0x03u);
    return (uint8_t)(raw & s_seen_open);
}

// The event for a position and a gesture: three per switch, press / hold / hold-release.
external_input::Event event_for(uint8_t pos, external_input::Gesture g)
{
    // Position 1/2/3 maps to switch 0/1/2 -- the codes are the same order as the enum.
    return (external_input::Event)(1u + (pos - 1u) * 3u + (uint8_t)g);
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

void external_input::set_action(Switch sw, bool hold, Action a)
{
    if ((uint8_t)sw < 3u && (uint8_t)a < (uint8_t)Action::Count)
        s_action[(uint8_t)sw][hold ? 1u : 0u] = a;
}

external_input::Action external_input::action(Switch sw, bool hold)
{
    return ((uint8_t)sw < 3u) ? s_action[(uint8_t)sw][hold ? 1u : 0u] : Action::None;
}

void external_input::update()
{
    if (s_mode != Mode::Footswitch) return;

    const uint32_t now = systick::now_ms();
    const uint8_t  raw = sample_position();

    if (raw != s_raw_pos) {
        s_raw_pos   = raw;
        s_change_ms = now;
    }

    if ((now - s_change_ms) >= FOOTSWITCH_DEBOUNCE_MS && raw != s_deb_pos) {
        s_deb_pos = raw;
        if (raw != 0u) {
            // Entering or escalating a press. 'Both' (3) is the strongest position and sticks
            // once seen, so a three-button press whose contacts settle a few ms apart still
            // reports Both rather than a stray Tip/Ring.
            //
            // Escalation stops once a hold has fired, because that hold named a switch and
            // is already engaged: promoting it to Both would have to release one momentary
            // action and engage another mid-gesture, which is an audible glitch nobody asked
            // for. Adding the second contact after the threshold therefore keeps the hold
            // that is running -- and the two contacts of a real three-button press close
            // milliseconds apart, far inside it.
            if (s_armed_pos == 0u) s_press_ms = now;   // a new gesture starts here
            if (!s_hold_fired) {
                if (raw == 3u)              s_armed_pos = 3u;
                else if (s_armed_pos != 3u) s_armed_pos = raw;
            }
        } else if (s_armed_pos != 0u) {
            // Released: close whichever gesture was open -- the hold it started, or, where the
            // foot came off before the threshold, a short press. Never both.
            push_event(event_for(s_armed_pos, s_hold_fired ? Gesture::HoldRelease
                                                           : Gesture::Press));
            s_armed_pos  = 0u;
            s_hold_fired = false;
        }
    }

    // The hold fires once, mid-press, so a momentary action engages while the foot is still
    // down rather than waiting for it to lift.
    //
    // Only where the contact has a hold to run. A hold that fired regardless would swallow
    // the press -- the press is emitted on the release and only where no hold fired -- so a
    // contact with nothing on its hold would go dead to any stomp somebody leant on, which
    // is most of them on a switch under a boot. An unassigned hold means the contact has one
    // gesture, and it behaves exactly as it did before it had two.
    if (!s_hold_fired && s_armed_pos != 0u && (now - s_press_ms) >= FOOTSWITCH_HOLD_MS
        && s_action[s_armed_pos - 1u][1] != Action::None) {
        s_hold_fired = true;
        push_event(event_for(s_armed_pos, Gesture::Hold));
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
