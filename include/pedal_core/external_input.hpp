#pragma once
#include <cstdint>
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
// Each of the three switches is assigned an Action (persisted in the settings block). set_mode() and
// set_action() are called from Pedal::boot() with the stored values and whenever the user
// changes them on the global External Input page. update() runs once per control tick from
// main.cpp (inert in Expression mode); Pedal drains the events and runs the assigned action.
namespace external_input {

    enum class Mode : uint8_t { Expression = 0, Footswitch = 1 };

    // Which physical contact a footswitch event came from.
    enum class Switch : uint8_t { Tip = 0, Ring = 1, Both = 2, Count = 3 };

    // Action a switch is assigned to. Values mirror the product's EXT_* codes / the persisted byte / the
    // SysEx payload; the static_assert keeps the count in lock-step with EXT_ACTION_COUNT.
    // Append-only: the value is the persisted byte and the SysEx payload, so an existing
    // assignment has to keep meaning what it meant.
    enum class Action : uint8_t {
        None = 0, Bypass, Tap, PresetUp, PresetDown, AlgoUp, AlgoDown,
        MomentaryBypass, Freeze, RotarySpeed, RotaryBrake, Compare, Count
    };
    static_assert((uint8_t)Action::Count == EXT_ACTION_COUNT, "Action enum vs EXT_ACTION_COUNT");

    enum class Event : uint8_t { None = 0, TipPress, RingPress, BothPress };

    // Full action name for the (full-screen) settings display. Matches the preset editor's
    // option labels; the longest, "Algorithm Down", is 14 chars.
    inline const char* action_name(Action a)
    {
        switch (a) {
            case Action::Bypass:          return "Bypass";
            case Action::Tap:             return "Tap Tempo";
            case Action::PresetUp:        return "Preset Up";
            case Action::PresetDown:      return "Preset Down";
            case Action::AlgoUp:          return "Algorithm Up";
            case Action::AlgoDown:        return "Algorithm Down";
            case Action::MomentaryBypass: return "Mom. Bypass";
            case Action::Freeze:          return "Freeze";
            case Action::RotarySpeed:     return "Rotary Speed";
            case Action::RotaryBrake:     return "Rotary Brake";
            case Action::Compare:         return "Compare";
            default:                      return "Off";   // None / out of range
        }
    }

    // Whether an action lasts only as long as the switch is held. A momentary action
    // engages on the hold and returns on the release, so a player can lean on a freeze,
    // play over it, and let go; everything else fires once and is done.
    //
    // This is what the hold/hold-release event pair exists for, and it is why the
    // footswitch hold threshold is a musical one rather than a menu one.
    inline bool action_is_momentary(Action a)
    {
        return a == Action::MomentaryBypass
            || a == Action::Freeze
            || a == Action::RotaryBrake;
    }

    void   init();                          // safe default config (Expression); mode applied in boot()
    void   set_mode(Mode mode);             // (re)configure the jack's pins via the product's hal
    Mode   mode();
    void   set_action(Switch sw, Action a); // assign a switch's action (Footswitch mode)
    Action action(Switch sw);
    void   update();                        // debounce in Footswitch mode; inert otherwise
    Event  get_event();                     // returns and clears the oldest pending event
    bool   has_event();
}
