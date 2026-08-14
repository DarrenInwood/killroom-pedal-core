# pedal-core

Shared firmware core for a family of guitar pedals built on the same control
surface: a 128×64 monochrome OLED, a 25xx256-class SPI EEPROM preset store,
10-bit parameters at every layer, and MIDI over USB and DIN.

Consumers pull this repo in as a git submodule under `firmware/lib/pedal-core`;
PlatformIO's library dependency finder picks it up from there via
[library.json](library.json).

## What lives here

| Module | Kind | What it is |
|---|---|---|
| `pedal_core/crc16.hpp` | pure header | CRC-16/CCITT-FALSE, the one integrity check every EEPROM block uses |
| `pedal_core/adc_filter.hpp` | pure header | Fixed-point EMA (α = 1/64) for pot smoothing |
| `pedal_core/adc_map.hpp` | pure header | 12-bit ADC code → parameter value, raw and calibrated |
| `pedal_core/encoder_decode.hpp` | pure header | Quadrature decode table, quarter-steps → detents |
| `pedal_core/usb_midi_cin.hpp` | pure header | USB-MIDI 1.0 CIN classification, both directions |
| `pedal_core/vbus_debounce.hpp` | pure header | VBUS presence debouncer for self-powered devices |
| `pedal_core/font.hpp` + `font_data.*` | header + data | The family's variable-width OLED fonts; regenerate with [tools/gen_fonts.py](tools/gen_fonts.py) |
| `pedal_core/eeprom.*` | protocol driver | 25xx256: page-bounded writes, boot health probe, RAM-mirror fallback |
| `pedal_core/display.*` | protocol driver | SSD1309 / SSD1306 / ST7567 framebuffer driver with pixel-precise text, gauges, `invert_region`, slide composition |

## The porting contract

Nothing in `src/` touches a register. Hardware is reached only through
[include/pedal_core/hal.hpp](include/pedal_core/hal.hpp): `spi::write/transfer`,
`systick::now_ms/delay_ms`, `watchdog::kick`, and the `pedal_core::hal::*` pin
functions. A product implements those over its own MCU drivers and the library
compiles unchanged — the consumers run on different STM32 families.

Product-specific constants (parameter scale, EEPROM geometry, display size)
come from a `pedal_core_config.hpp` the product places on the include path.
[test/support/pedal_core_config.hpp](test/support/pedal_core_config.hpp) is the
reference for what it must define.

## Tests

```
pio test -e native
```

Unit tests for library modules live here, not in the consumers. The two
protocol drivers are tested by compiling the real `.cpp` into the test TU
behind recording stubs — see `test/test_eeprom` for the pattern.
