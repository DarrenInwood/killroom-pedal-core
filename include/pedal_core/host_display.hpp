#pragma once
#include <cstdint>
#include "hal.hpp"

// The hardware seam, stubbed, for a host program that compiles the display stack.
//
// The driver, the fonts and the compositor are all portable C++; what stops them running
// on a desktop is the four hal contracts they reach hardware through. Satisfying those
// with no-ops leaves the real framebuffer in exactly the state a pedal's would be, which
// is what lets a screen be photographed for the documentation or asserted on in a suite —
// with no second implementation of the layout anywhere.
//
// Include this once, before the implementation files being compiled in:
//
//     #include <pedal_core/host_display.hpp>
//     #include "path/to/pedal-core/src/display.cpp"
//     #include "path/to/pedal-core/src/ui/compositor.cpp"
//
// THE CLOCK IS NOT A CONSTANT. The splash and the save animation timestamp themselves
// from systick, and display::init() spends 110 ms of it on the controller's reset pulse.
// A clock pinned at zero freezes both animations on their first frame — the least
// interesting frame either one has — and a producer stamped from a clock the caller never
// advances past times out on its first tick. So the clock is settable, and a host program
// is expected to move it: set_now_ms() after init(), then advance_ms() between frames.
//
// This header defines the symbols rather than declaring them, so a translation unit that
// includes it needs no other stub file. The library's own test/support fakes are linked
// as a static library, so a suite including this defines the clock itself and never pulls
// the archive member — the two do not collide.
namespace pedal_core::host_display {

// The millisecond clock the stubbed systick reads. Owned here rather than by the caller
// so that including this header is the whole of the wiring.
inline uint32_t& clock_ms()
{
    static uint32_t ms = 0u;
    return ms;
}

// Put the clock at an instant. A host program calls this after display::init(), which
// has spent time of its own on the reset pulse.
inline void set_now_ms(uint32_t ms) { clock_ms() = ms; }

// Move it on. Between frames of an animation, this is the frame interval.
inline uint32_t advance_ms(uint32_t ms)
{
    clock_ms() += ms;
    return clock_ms();
}

inline uint32_t now_ms() { return clock_ms(); }

}  // namespace pedal_core::host_display

// --- the contracts themselves ----------------------------------------------
// Defined inline, so every translation unit that includes this header agrees on one
// definition and the linker folds them.

namespace spi {
inline void write(const uint8_t*, uint16_t) {}
inline void transfer(const uint8_t*, uint8_t*, uint16_t) {}
}  // namespace spi

namespace systick {
inline uint32_t now_ms() { return pedal_core::host_display::clock_ms(); }
// A host has nothing to wait for, but the driver's reset sequence spends real time here
// and the frame pacing reads it, so the delay moves the clock rather than being ignored.
//
// Reading through now_ms() rather than clock_ms() keeps the two together in the object
// file, and that is load-bearing rather than tidy. An inline function nothing in the
// translation unit calls is not emitted at all; a host program that hands the compositor
// its instants never calls now_ms() itself, and the library modules linked beside it do --
// footswitch, tap_tempo and external_input ask the time on every pass. With now_ms() left
// out, the linker goes looking for it, finds test/support's systick fake, and the binary
// ends up with two clocks: the one this header settles and the one that member advances.
// display::init() spends its reset pulse here, so this call is what keeps both present.
inline void delay_ms(uint32_t ms)
{
    pedal_core::host_display::clock_ms() = now_ms() + ms;
}
}  // namespace systick

namespace pedal_core::hal {
inline void display_pins_init() {}
inline void display_cs(bool) {}
inline void display_dc_data(bool) {}
inline void display_reset(bool) {}
inline void display_power(bool) {}
}  // namespace pedal_core::hal
