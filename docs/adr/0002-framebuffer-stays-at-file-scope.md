# ADR-0002: The framebuffer stays at file scope

**Status**: Accepted — 2026-08-29

## Context

`display.cpp` holds its framebuffer as a file-static `s_fb`, and the drawing primitives operate
on it directly. A suite that wants to see pixels therefore has to `#include` the driver's `.cpp`
into its own translation unit, which is the idiom `test_display`, `test_eeprom` and
`test_dfu_session` all use.

An architecture review raised the obvious objection: two modules that both draw cannot be tested
together without defining the driver twice. It proposed a `Framebuffer` the driver holds and a
suite can borrow, turning the donor-include into an ordinary link. It was listed as speculative,
to be reopened only if the file-static framebuffer turned out to block a compositor suite.

## Decision

The framebuffer stays at file scope, and the donor-include stays the way host suites reach it.

## Consequences

**The predicted obstruction did not happen.** `test_compositor` donor-includes `display.cpp`
*and* `ui/compositor.cpp` into one translation unit behind no-op SPI and hal stubs, and asserts
against `display::capture()`. Two modules that both draw, tested together, with no change to the
driver. The concern was reasonable in advance and turned out not to bind.

**One adapter is a hypothetical seam.** There is one framebuffer in a pedal and one in a host
program. Nothing varies across the seam that would be introduced.

**The idiom is deliberate and documented.** `platformio.ini` explains why `display.cpp` and
`eeprom.cpp` are excluded from the native `build_src_filter` — they are compiled into their test
translation units instead, and building them twice would define every symbol twice. The README
names the same idiom as what makes the screenshot pipeline possible. This is a decision the repo
has already made coherently in several places.

**`display::capture()` is the real seam, and it already exists.** A host program does not need
the framebuffer's address; it needs a copy of the frame, which is what `capture()` returns and
what `frame_dump.hpp` writes out.

## When to reopen

If two modules that both draw ever need to be linked into one binary *without* one of them being
donor-included — a host program compiling the app's compositor and the bootloader's DFU screen
as ordinary objects, say — the duplicate-symbol problem becomes real rather than theoretical.
The multi-effect's screenshot generator currently sidesteps it by donor-including both, so this
has not arrived yet.
