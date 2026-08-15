# Migrating a pedal onto pedal-core

> The library also carries the app/UI layer: `ui::Compositor` (subclass with a
> LayoutSpec + your icons/screens/splash), `PedalBase` (the family MIDI
> meanings — NRPN latch, bank/PC arithmetic, SysEx dispatch — behind hooks for
> your write path and command bodies), `MidiResponderBase`, and the
> footswitch / bypass / midi_handler / tap_tempo / tempo_controller /
> tempo_led / external_input modules plus the storage machinery
> (`blocks::seal`, sealed blocks, `SlotRing`). Consumers provide
> `pedal_core_ui_config.hpp` (and `pedal_core_tempo_config.hpp` /
> `pedal_core_extinput_config.hpp` where those domains exist); the reference
> stubs in `test/support/` name every constant. The sections below describe
> the original leaf-module migration and still apply to it.

How to move an existing firmware onto this library. Written for the pedals this
library was extracted *from*, where most of the code is already here and the
work is re-pointing rather than rewriting.

**Expect the library to look like your code, because it is your code.** The
files below were lifted from a working firmware. Do not "improve" them on the
way past — a behavioural change smuggled in during a migration is invisible in
the diff, because the diff is supposed to be full of moved code.

---

## 1. What the library needs from you

### The hardware seam — [`include/pedal_core/hal.hpp`](include/pedal_core/hal.hpp)

Nothing in `src/` touches a register. Hardware arrives through exactly three
namespaces plus one:

| Contract | Signature | Who provides it |
|---|---|---|
| `spi::write` | `void(const uint8_t*, uint16_t)` | your existing SPI driver |
| `spi::transfer` | `void(const uint8_t*, uint8_t*, uint16_t)` | your existing SPI driver |
| `systick::now_ms` | `uint32_t()` | your existing tick |
| `systick::delay_ms` | `void(uint32_t)` | your existing tick |
| `watchdog::kick` | `void()` | your existing watchdog |
| `pedal_core::hal::*` | seven pin functions | **new — you write these** |

The first five are almost certainly already declared exactly like this; a
donor-derived firmware links them with no adapter at all. Declaring extra
functions in those namespaces (`spi::init`, `systick::isr_tick`, …) is fine —
the library only needs the ones above.

### The polarity trap

`pedal_core::hal` names **logical** states. Mapping them to electrical levels is
your job, and three of the seven invert:

| Function | `true` means | Typical level |
|---|---|---|
| `eeprom_cs(bool select)` | selected | **low** |
| `display_cs(bool select)` | selected | **low** |
| `display_reset(bool in_reset)` | held in reset | **low** |
| `display_dc_data(bool data)` | pixel data (not command) | high |
| `display_power(bool on)` | panel supply on | high |

`*_pins_init()` must leave every device **deselected and unpowered**. Carry over
any detail your old `init()` had: if the display enable was configured at a
lower slew rate, or driven low explicitly to back up an external pulldown, both
belong in `display_pins_init()` — the panel's boost sequencing depends on them.

### The config header

The library includes `"pedal_core_config.hpp"`. Put one on the include path. It
must define, at namespace scope:

```
PARAM_VALUE_BITS  PARAM_MAX  PARAM_MID  PARAM_CENTRE
EEPROM_STORE_SIZE  EEPROM_PROBE_ADDR
OLED_WIDTH  OLED_HEIGHT  OLED_PAGES
```

[`test/support/pedal_core_config.hpp`](test/support/pedal_core_config.hpp) is the
reference. If your firmware already defines these across several headers, a
three-line shim that includes them is the whole job — but keep register-level
headers (a pinmap) out of it, or CMSIS leaks into the library's translation
units and the host build breaks.

---

## 2. The file inventory

For a firmware this library was extracted from, files fall into three groups.

**Byte-identical — delete yours, include ours.** No review needed beyond the
include path:

`crc16.hpp` · `encoder_decode.hpp` · `usb_midi_cin.hpp` · `vbus_debounce.hpp` ·
`font.hpp` · `font_data.hpp` · `sysex_codec.hpp` · `app_image.hpp` ·
`dfu_progress.hpp`

**Include path only** — the code is identical, the `#include` line differs:

| File | Change |
|---|---|
| `adc_filter.hpp` | comment only |
| `eeprom.hpp` | comment only |
| `adc_map.hpp` | `"../config/product.hpp"` → `"pedal_core_config.hpp"` |
| `display.hpp` | `"../config.hpp"` → `"pedal_core_config.hpp"` |
| `font_data.cpp` | `"font_data.hpp"` → `<pedal_core/font_data.hpp>` |

**Re-seamed** — same logic, hardware calls replaced. Review these two:

- `eeprom.cpp` — `gpio::` calls on a CS pin became `hal::eeprom_cs` /
  `hal::eeprom_pins_init`. Everything else, including the page-split writes, the
  boot probe and the RAM-mirror fallback, is unchanged.
- `display.cpp` — the same for CS, DC, RES and the panel supply. The
  framebuffer, fonts, primitives and flush path are unchanged.

**New here** — no counterpart to delete: `hal.hpp`, `dfu_protocol.hpp`,
`dfu_session.hpp`.

---

## 3. Tests move with the code

The library owns its modules' tests. Delete the consumer's copies of anything
here — keeping both means two places to update and one of them will rot.

The two driver suites need one edit: where they stubbed `gpio::` through a CMSIS
host shim, they now define a `namespace pedal_core::hal` block of no-ops.
[`test/test_eeprom`](test/test_eeprom/) and [`test/test_display`](test/test_display/)
show the pattern.

`native_test_setup.py` and `unity_config.h` also exist here; the consumer keeps
its own, since it still has suites of its own to run.

---

## 4. Build wiring

PlatformIO auto-builds anything under a project's `lib/`, so a submodule at
`firmware/lib/pedal-core` needs no `lib_deps` for the app. Two things that
arrangement does not solve:

- **The native test env** may be running LDF in `chain` mode. It needs
  `lib_ldf_mode = deep+` to resolve the library's headers, and `lib_ignore =
  pedal-core` if you want the fakes — not the library's `.cpp` files — to supply
  `eeprom::` and `display::`.
- **A sibling bootloader project** has no `lib/` of its own and needs an explicit
  `lib_deps = symlink://../lib/pedal-core`.

**CI: every `actions/checkout` needs `submodules: true`.** An uninitialised
submodule leaves an empty mount point, so a link to the directory resolves while
a link to a file inside it does not — the failure looks like a broken doc link
rather than a missing library.

---

## 5. Order of work

1. Add the submodule; add the config header and the `hal::` implementation.
   Nothing consumes them yet.
2. Re-point one module — `crc16.hpp` is the smallest — and get the build and the
   full test suite green. This proves the wiring before anything depends on it.
3. Re-point the rest of the headers, then the two seamed drivers.
4. Delete the duplicated tests.
5. The bootloader last, and as its own commit: it is the only part that touches
   a shipped update path.

**Do not regenerate a golden to make a test pass.** If a golden table, a golden
byte image or a pixel test moves during a migration, something changed that was
not supposed to. That is the signal the whole exercise depends on.

---

## 6. If you are migrating a bootloader

[`dfu_session.hpp`](include/pedal_core/dfu_session.hpp) replaces the decode,
bounds-check and erase/write sequencing that a bootloader `main.cpp` usually
does inline. It is stricter than hand-written versions tend to be: it rejects a
zero-length chunk, a declared length the payload does not supply, an
unaligned address, and a range that would wrap.

Check your host updater against these, because they are the parts a bootloader
gets to define:

- **Chunk framing** is a 4×7-bit address, then the **encoded** payload length as
  2×7-bit, then the packed data. Seven-bit groups throughout: a raw byte above
  `0x7F` inside a SysEx frame is a status byte and a compliant parser abandons
  the message.
- **`CHUNK_MAX` defaults to 256** binary bytes. Define
  `PEDAL_CORE_DFU_CHUNK_MAX` if your host sends more.
- **`FlashOps::begin`** fires on `FW_BEGIN`. Wire it to whatever tracks which
  pages you have erased, or a retry programs over un-erased flash.
- **The end-of-image CRC covers what was written**, not what the host declared.
- **The status address is a high-water mark**, so a resent chunk does not rewind
  a host's resume point.
