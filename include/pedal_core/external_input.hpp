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
        MomentaryBypass, Freeze, RotarySpeed, RotaryBrake, Compare, Scene,
        MomFreeze, Count
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
            case Action::MomFreeze:       return "Mom. Freeze";
            case Action::RotarySpeed:     return "Rotary Speed";
            case Action::RotaryBrake:     return "Rotary Brake";
            case Action::Compare:         return "Compare";
            case Action::Scene:           return "Scene A/B";
            default:                      return "Off";   // None / out of range
        }
    }

    // Whether an action lasts only as long as the switch is held. A momentary action
    // engages on the hold and returns on the release, so a player can lean on a freeze,
    // play over it, and let go; everything else fires once and is done.
    //
    // This is what the hold/hold-release event pair exists for, and it is why the
    // footswitch hold threshold is a musical one rather than a menu one. Every action
    // here is hold-only, and every hold-only action is here: what a switch does no longer
    // depends on which gesture reached it.
    inline bool action_is_momentary(Action a)
    {
        return a == Action::MomentaryBypass
            || a == Action::MomFreeze
            || a == Action::RotaryBrake;
    }

    // Which gesture an action belongs on.
    //
    // A press is emitted on the release and has nothing left to hold, so a while-held
    // action needs a hold to follow. A hold gives one event per lean on the switch, so
    // tapping a tempo with it is not something anyone can do. Everything that fires once
    // and is done works either way.
    //
    // These are what the settings rows offer, and what a stored assignment is translated
    // against when a preset written before the split names the other one.
    inline bool action_allows_hold(Action a)
    {
        return a != Action::Tap        // one tap per lean is not tapping a tempo
            && a != Action::Freeze;    // the latching one; MomFreeze is the held one
    }

    inline bool action_allows_press(Action a)
    {
        return !action_is_momentary(a);   // nothing to release a press against
    }

    // The same action for the other gesture, or None where there is no counterpart. A
    // preset that named one before the split is read as the other, so what it does is
    // unchanged even though the code it stores now means something narrower.
    inline Action action_for_gesture(Action a, bool hold)
    {
        if (hold  && a == Action::Freeze)          return Action::MomFreeze;
        if (!hold && a == Action::MomFreeze)       return Action::Freeze;
        if (!hold && a == Action::MomentaryBypass) return Action::Bypass;
        return (hold ? action_allows_hold(a) : action_allows_press(a)) ? a : Action::None;
    }

    // The list a settings row sweeps for one gesture: only the actions that gesture can
    // carry, in code order, with Off always first because code 0 is legal everywhere.
    //
    // A row steps a position through these rather than the raw codes, because the menu
    // clamps a single contiguous range and a filtered vocabulary is not one. Keeping the
    // three in one place is what stops the count, the lookup and the inverse drifting.
    inline uint8_t action_count_for(bool hold)
    {
        uint8_t n = 0;
        for (uint8_t i = 0; i < (uint8_t)Action::Count; ++i)
            if (hold ? action_allows_hold((Action)i) : action_allows_press((Action)i)) ++n;
        return n;
    }

    inline Action action_at(bool hold, uint8_t pos)
    {
        for (uint8_t i = 0; i < (uint8_t)Action::Count; ++i) {
            if (!(hold ? action_allows_hold((Action)i) : action_allows_press((Action)i))) continue;
            if (pos == 0u) return (Action)i;
            --pos;
        }
        return Action::None;
    }

    // Where an action sits in that sweep. An action the gesture cannot carry has no
    // position, and answers 0 -- Off -- so a row is always on a value it can step from.
    // Callers translate through action_for_gesture() first, so what the row shows is what
    // the switch will actually do.
    inline uint8_t action_pos_of(Action a, bool hold)
    {
        uint8_t n = 0;
        for (uint8_t i = 0; i < (uint8_t)Action::Count; ++i) {
            if (!(hold ? action_allows_hold((Action)i) : action_allows_press((Action)i))) continue;
            if ((Action)i == a) return n;
            ++n;
        }
        return 0u;
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
