#pragma once
#include <cstdint>

// The relay (true bypass) and its status LED, over the hal panel seam. The LED
// mirrors the active state and flashes on a preset save — count on/off pairs at
// the family cadence, starting from the current LED state so the first toggle
// is immediately visible. What triggers a flash, and any second switch the
// product hangs beside the relay (a boost, a tap), is the product's business.
//
// Three things can want that LED, so they are ranked here rather than raced for: a
// flash beats a claim, a claim beats the relay. A product that never claims sees only
// the last two, which is the behaviour this module has always had.
namespace bypass {
    void init();
    void set_active(bool active);
    void toggle();
    bool is_active();
    // Flash the LED N times (non-blocking; driven by update()).
    void flash(uint8_t count);
    void update();  // call every main loop tick

    // Put something other than the relay under the status LED — a state the footswitch
    // beside it is holding, on a product whose switches can carry one. Re-stated every
    // tick by whoever owns it; release_led() hands it back to the relay.
    void claim_led(bool on);
    void release_led();
}
