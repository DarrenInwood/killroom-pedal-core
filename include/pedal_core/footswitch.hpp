#pragma once
#include <cstdint>

// Debounced two-footswitch gesture grammar: press vs hold, per switch.
//
// The two switches are wholly independent. Each reports its own press, its own
// hold, and the end of that hold; pressing both at once simply reports both, so
// there is no combination gesture to learn, to time, or to trigger by accident
// in the dark. What each gesture *does* (bypass, tap, freeze, rotary brake) is
// the product's policy; this module only names the gestures.
//
// A press is emitted on release and only when no hold fired, so a hold never
// also reads as a press. A hold fires once while the switch is down and is
// closed by FSn_HoldRelease when the foot comes off — the pair is what lets a
// product implement a momentary action (lean on it for a freeze, release to
// return) rather than only a latching one.
//
// Raw switch state arrives through pedal_core::hal::fs_pressed(); the product
// supplies FOOTSWITCH_DEBOUNCE_MS and FOOTSWITCH_HOLD_MS via its
// pedal_core_ui_config.hpp. Call update() every tick. Events are
// edge-triggered, queued, and cleared after one read.
namespace footswitch {

    enum class Event : uint8_t {
        None = 0,
        FS1_Press,        // short press, emitted on release
        FS1_Hold,         // held for FOOTSWITCH_HOLD_MS
        FS1_HoldRelease,  // that hold ended
        FS2_Press,
        FS2_Hold,
        FS2_HoldRelease,
    };

    void  init();
    void  update();
    Event get_event();     // returns and clears the oldest pending event
    bool  has_event();

    // How long switch `sw` (0 or 1) has been down, debounced; 0 while it is up. The
    // gesture grammar above is edge-triggered, so this is the one way to ask what a foot
    // is doing right now -- which is what a screen showing the hold actions while a
    // switch is under a foot needs, before the hold has fired and there is anything to
    // report as an event.
    uint32_t down_for_ms(uint8_t sw);
}
