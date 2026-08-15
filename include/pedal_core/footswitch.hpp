#pragma once
#include <cstdint>

// Debounced two-footswitch gesture grammar: press vs hold per switch, plus
// Both_Hold — both switches held simultaneously for the full hold window,
// timed from the later press, so a brief overlap of two singles never fires
// it. What each gesture *does* (bypass, tap, boost, preset step, save) is the
// product's policy; this module only names the gestures.
//
// Raw switch state arrives through pedal_core::hal::fs_pressed(); the product
// supplies FOOTSWITCH_DEBOUNCE_MS and FOOTSWITCH_HOLD_MS via its
// pedal_core_ui_config.hpp. Call update() every tick. Events are
// edge-triggered, queued, and cleared after one read.
namespace footswitch {

    enum class Event : uint8_t {
        None = 0,
        FS1_Press,      // short press released
        FS1_Hold,       // held for FOOTSWITCH_HOLD_MS
        FS2_Press,
        FS2_Hold,
        Both_Hold,      // both held simultaneously for FOOTSWITCH_HOLD_MS
    };

    void  init();
    void  update();
    Event get_event();     // returns and clears the oldest pending event
    bool  has_event();
}
