#pragma once
#include <cstdint>
#include "action.hpp"
#include "pedal_core_features.hpp"

#if !PEDAL_CORE_HAS_EXTINPUT
#  error "external_input.hpp needs PEDAL_CORE_HAS_EXTINPUT. Set it in pedal_core_features.hpp and supply pedal_core_extinput_config.hpp."
#endif

#include "pedal_core_extinput_config.hpp"   // EXT_ACTION_COUNT, EXT_DEFAULT_* defaults

// The external-input jack — dual-function, menu-selected, for products that
// have one (compiled only where pedal_core_extinput_config.hpp exists).
//
//   Expression mode: the jack carries an expression pedal; the product's hal
//   wires the reference out and the analog wiper return.
//
//   Footswitch mode: both contacts are momentary switch inputs, and the module
//   debounces the *combined* contact state into three mutually-exclusive
//   switches: tip alone, ring alone, and both together. A common three-button
//   footswitch wires its third switch to both contacts through diodes, so
//   pressing it closes tip and ring simultaneously — reported here as Both,
//   distinct from Tip and Ring.
//
// Each switch carries two gestures — a press and a hold — assigned an Action apiece and
// persisted in the settings block. set_mode() and set_action() are called from Pedal::boot()
// with the stored values and whenever the user changes them on the global External Input
// page. update() runs once per control tick from main.cpp (inert in Expression mode); Pedal
// drains the events and runs the assigned action.
namespace external_input {

    enum class Mode : uint8_t { Expression = 0, Footswitch = 1 };

    // Which physical contact a footswitch event came from.
    enum class Switch : uint8_t { Tip = 0, Ring = 1, Both = 2, Count = 3 };

    // What a switch is assigned to. The vocabulary is pedal_core::action's, shared with the
    // front-panel switches, and named here so a jack assignment reads in the jack's own
    // namespace. The static_assert holds the product's copy of the count to the library's.
    using Action = pedal_core::action::Action;
    static_assert((uint8_t)Action::Count == EXT_ACTION_COUNT, "Action enum vs EXT_ACTION_COUNT");

    // Three events per switch, in press / hold / hold-release order, so the switch and the
    // gesture are both arithmetic on the value rather than another table to keep in step.
    //
    // A press is emitted on the release and only where no hold fired; a hold fires mid-press
    // so a momentary action engages while the foot is still down, and is closed by its
    // release. The same grammar the front-panel switches use, for the same reason.
    enum class Event : uint8_t {
        None = 0,
        TipPress,  TipHold,  TipHoldRelease,
        RingPress, RingHold, RingHoldRelease,
        BothPress, BothHold, BothHoldRelease,
    };

    enum class Gesture : uint8_t { Press = 0, Hold = 1, HoldRelease = 2 };

    // Which switch an event came from, and which of its gestures. Undefined for Event::None,
    // which every caller drops before asking.
    inline Switch  event_switch(Event e)  { return (Switch)(((uint8_t)e - 1u) / 3u); }
    inline Gesture event_gesture(Event e) { return (Gesture)(((uint8_t)e - 1u) % 3u); }

    // The vocabulary's own rules — which gesture an action belongs on, what it is called,
    // and the sweep a settings row steps through — reachable in this namespace so a jack
    // assignment and a panel one are written the same way.
    using pedal_core::action::action_name;
    using pedal_core::action::action_is_momentary;
    using pedal_core::action::action_allows_hold;
    using pedal_core::action::action_allows_press;
    using pedal_core::action::action_for_gesture;
    using pedal_core::action::action_count_for;
    using pedal_core::action::action_at;
    using pedal_core::action::action_pos_of;

    void   init();                          // safe default config (Expression); mode applied in boot()
    void   set_mode(Mode mode);             // (re)configure the jack's pins via the product's hal
    Mode   mode();
    // A switch's action for one of its two gestures (Footswitch mode). Which gesture is a
    // bool rather than the Gesture enum: a hold and its release are one assignment, and
    // splitting them would let a switch engage one action and release a different one.
    void   set_action(Switch sw, bool hold, Action a);
    Action action(Switch sw, bool hold);
    void   update();                        // debounce in Footswitch mode; inert otherwise
    Event  get_event();                     // returns and clears the oldest pending event
    bool   has_event();
}
