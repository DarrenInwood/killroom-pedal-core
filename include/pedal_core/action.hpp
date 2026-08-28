#pragma once
#include <cstdint>

// What a switch does. The one vocabulary every switch on the pedal is assigned from —
// the two on the front panel, and both contacts of the external jack — so a label, a
// stored byte and a SysEx payload all mean the same thing wherever they came from.
//
// Nothing here reaches hardware or needs a product config, so a product with no external
// jack still has the vocabulary its panel switches are assigned from.
namespace pedal_core::action {

// Values are the persisted byte and the SysEx payload, so this is APPEND-ONLY: an
// existing assignment has to keep meaning what it meant.
enum class Action : uint8_t {
    None = 0, Bypass, Tap, PresetUp, PresetDown, AlgoUp, AlgoDown,
    MomentaryBypass, Freeze, RotarySpeed, RotaryBrake, Compare, Scene,
    MomFreeze, Count
};

// Full action name for the (full-screen) settings display. Matches the preset editor's
// option labels; the longest, "Algorithm Down", is 14 chars.
constexpr const char* action_name(Action a)
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

// The longest name in the vocabulary, in characters. A screen row sizes itself from this
// rather than from a number someone has to remember to raise, so an action whose name
// outgrows the row is a layout that widens rather than a label that silently truncates —
// truncation reads as a misspelling rather than as a layout problem.
constexpr uint8_t name_length(const char* s)
{
    uint8_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

constexpr uint8_t longest_name()
{
    uint8_t longest = 0;
    for (uint8_t i = 0; i < (uint8_t)Action::Count; ++i) {
        const uint8_t n = name_length(action_name((Action)i));
        if (n > longest) longest = n;
    }
    return longest;
}

inline constexpr uint8_t LONGEST_NAME = longest_name();

// Whether an action lasts only as long as the switch is held. A momentary action
// engages on the hold and returns on the release, so a player can lean on a freeze,
// play over it, and let go; everything else fires once and is done.
//
// This is what the hold/hold-release event pair exists for, and it is why the
// footswitch hold threshold is a musical one rather than a menu one. Every action
// here is hold-only, and every hold-only action is here: what a switch does does not
// depend on which gesture reached it.
constexpr bool action_is_momentary(Action a)
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
// against when a preset names the counterpart gesture's action.
constexpr bool action_allows_hold(Action a)
{
    return a != Action::Tap        // one tap per lean is not tapping a tempo
        && a != Action::Freeze;    // the latching one; MomFreeze is the held one
}

constexpr bool action_allows_press(Action a)
{
    return !action_is_momentary(a);   // nothing to release a press against
}

// The same action for the other gesture, or None where there is no counterpart. A
// preset naming one is read as the other, so what it does is unchanged even though the
// code it stores means something narrower.
constexpr Action action_for_gesture(Action a, bool hold)
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
constexpr uint8_t action_count_for(bool hold)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < (uint8_t)Action::Count; ++i)
        if (hold ? action_allows_hold((Action)i) : action_allows_press((Action)i)) ++n;
    return n;
}

constexpr Action action_at(bool hold, uint8_t pos)
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
constexpr uint8_t action_pos_of(Action a, bool hold)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < (uint8_t)Action::Count; ++i) {
        if (!(hold ? action_allows_hold((Action)i) : action_allows_press((Action)i))) continue;
        if ((Action)i == a) return n;
        ++n;
    }
    return 0u;
}

}  // namespace pedal_core::action
