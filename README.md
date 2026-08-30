# pedal-core

Shared firmware core for a family of guitar pedals built on the same control
surface: a 128×64 monochrome OLED, a 25xx-class SPI EEPROM preset store,
10-bit parameters at every layer, and MIDI over USB and a MIDI jack.

Consumers pull this repo in as a git submodule under `firmware/lib/pedal-core`;
PlatformIO's library dependency finder picks it up from there via
[library.json](library.json).

## What lives here

| Module | Kind | What it is |
|---|---|---|
| `pedal_core/crc16.hpp` | pure header | CRC-16/CCITT-FALSE, the one integrity check every EEPROM block uses |
| `pedal_core/action.hpp` | pure header | What a switch does: the one vocabulary the panel switches and the external jack are both assigned from, and the row width its names imply |
| `pedal_core/adc_filter.hpp` | pure header | Fixed-point EMA (α = 1/64) for pot smoothing |
| `pedal_core/adc_map.hpp` | pure header | 12-bit ADC code → parameter value, raw and calibrated |
| `pedal_core/encoder_decode.hpp` | pure header | Quadrature decode table, quarter-steps → detents |
| `pedal_core/usb_midi_cin.hpp` | pure header | USB-MIDI 1.0 CIN classification, both directions |
| `pedal_core/midi_routing.hpp` | pure header | Where the wire's MIDI routing block and the pedal's MIDI configuration meet — all twelve fields named, and the one place both spellings of the follow-the-receive-channel sentinel are reconciled |
| `pedal_core/vbus_debounce.hpp` | pure header | VBUS presence debouncer for self-powered devices |
| `pedal_core/font.hpp` + `font_data.*` | header + data | The family's variable-width OLED fonts; regenerate with [tools/gen_fonts.py](tools/gen_fonts.py) |
| `pedal_core/eeprom.*` | protocol driver | 25xx, 16-bit address (25LC256 / 25LC512): page-bounded writes at the product's page size, boot health probe, and a RAM-mirror fallback sized by the product — whole-map where it fits, header + system blocks + one record window where it does not |
| `pedal_core/display.*` | protocol driver | SSD1309 / SSD1306 / ST7567 framebuffer driver with pixel-precise text, gauges, `invert_region`, slide composition |
| `pedal_core/midi_out_queue.hpp` | pure header | The MIDI Out jack transmit queue: whole messages, coalescing a controller sweep to its current value, and the rules deciding what is lost when there is more to send than 31.25 kbaud carries |
| `pedal_core/jack_router.hpp` | pure header | The MIDI Out router: the lock a streaming frame takes, the queue behind it, the running-status hold, the stall timeout, and the policy saying which source reaches which port |
| `pedal_core/sysex_codec.hpp` | pure header | Roland-style 7-bit SysEx packing and CRC32 — the firmware-update codec |
| `pedal_core/dfu_protocol.hpp` | pure header | The DFU-over-SysEx wire contract: command bytes and status codes |
| `pedal_core/dfu_session.hpp` | pure header | The DFU write session — chunk decode, bounds checks, erase/write sequencing, image verify — with flash behind a three-function seam |
| `pedal_core/app_image.hpp` | pure header | Boot-time application-image validation: descriptor magic, size, CRC32 trailer |
| `pedal_core/dfu_progress.hpp` | pure header | Upload percentage and its display string |
| `pedal_core/host_display.hpp` | pure header | The hardware seam, stubbed, for a host program compiling the display stack: SPI and pin no-ops, and a settable clock, so the shoot and the suites stand the same stack up the same way |
| `pedal_core/frame_dump.hpp` | pure header | The screenshot container — captured display frames on their way to [tools/oled_png.py](tools/oled_png.py) |

## Screenshots of the UI

A product documents its screens by generating them, not by drawing them. Build a
host program that includes
[include/pedal_core/host_display.hpp](include/pedal_core/host_display.hpp) — the
hardware seam, stubbed, with the settable clock the splash and save animations
timestamp themselves from — then compiles the real `display.cpp` and the
product's own compositor behind it, drives it through the states worth
picturing, and writes each `display::capture()` out with
[include/pedal_core/frame_dump.hpp](include/pedal_core/frame_dump.hpp). Then:

```
python tools/oled_png.py path/to/frames.bin --out docs/images/screens --sheet all_screens
```

[tools/oled_png.py](tools/oled_png.py) is standard library only, so the same
check that proves a product's docs are current runs in CI without a pip step.

The point of the split is that no second implementation of the layout exists.
The pictures are the firmware's own framebuffer, scaled — a screen the pedal
cannot draw is a screen that cannot appear in the docs, and a layout change that
was not regenerated shows up as a diff.

## The user manual

Every pedal in the family ships a Markdown manual rendered to PDF the same way,
so the template, the LaTeX preamble and the engine choice live here rather than
once per product:

| File | What it is |
|---|---|
| [tools/build_manual.py](tools/build_manual.py) | The renderer: Pandoc + Eisvogel + Tectonic, tool discovery, image resolution |
| [tools/manual/eisvogel.latex](tools/manual/eisvogel.latex) | The vendored Eisvogel template — title page, headers, code styling |
| [tools/manual/manual-header.tex](tools/manual/manual-header.tex) | Shared verbatim preamble: the `linknavy` link colour, and `fvextra` so long code lines wrap |
| [tools/diagram_kit.py](tools/diagram_kit.py) | The line-art toolkit the manuals' block diagrams are drawn with — supersampled strokes, labelled stage boxes, jacks, summing nodes, and a 200 dpi pHYs chunk so Pandoc sizes a figure to the text block |

Tectonic pulls the LaTeX packages it needs on demand and caches them, so no
system-wide TeX install is required — only the first run needs network access.

What stays with the product is the part that differs: `docs/manual/MANUAL.md`
(the content) and `tools/manual-meta.yaml` (title, subtitle, page geometry). A
product drives the renderer from a shim at its own `tools/build_pdf.py`, so the
command is `python tools/build_pdf.py` in every repo:

```python
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "firmware/lib/pedal-core/tools"))
from build_manual import main
sys.exit(main(["--manual", str(ROOT / "docs/manual/MANUAL.md"),
               "--meta",   str(ROOT / "tools/manual-meta.yaml")]))
```

Images resolve from the manual's own directory, so `../images/x.png` renders the
same in the PDF as it does on GitHub. A product needing extra preamble passes
`--extra-header`; one keeping figures elsewhere passes `--resource-path`.

## Cutting a release

The bootloader's descriptor is defined here, in
[`pedal_core/app_image.hpp`](include/pedal_core/app_image.hpp), so the host-side
tools that write and check that descriptor belong here too. A product that kept
its own copies would be maintaining a second definition of a contract it does not
own — and the failure mode is silent, because an image the bootloader refuses
does not crash, it leaves the pedal in DFU.

| File | What it is |
|---|---|
| [tools/appimage_seal.py](tools/appimage_seal.py) | Patches the descriptor's `size` field and appends the CRC32 trailer. **Imported, never executed by SCons** — a product keeps a real post-build script that does `Import("env")`; only the algorithm is here |
| [tools/app_image.py](tools/app_image.py) | The gate: re-implements `app_image::validate()` against the linked `.bin`, so a build that could not boot fails CI instead of a pedal |
| [tools/release_manifest.py](tools/release_manifest.py) | Checks the release tag against `product.hpp`, then writes the per-product manifest entry the editor fetches |
| [tools/setup_build_env.sh](tools/setup_build_env.sh) | Idempotent PlatformIO bootstrap; pre-fetches each named env's packages and prints `PIO=<path>` |

All four are standard library only, so the gates run in CI without a pip step.

What differs between products arrives as arguments rather than as an edited copy:
the app region's size (`region_bytes`), the product's name, its `device_id` and
`slug`, the macros its version lives in, and the list of PlatformIO envs. A
product drives each from a shim, so the command is the same in every repo —
`python tools/check_app_image.py <bin>`, `python tools/release_manifest.py …`,
`bash tools/setup-build-env.sh`:

```python
# tools/check_app_image.py
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "firmware/lib/pedal-core/tools"))
from app_image import main
sys.exit(main(sys.argv, region_bytes=..., usage=__doc__))
```

`device_id` and `slug` are wire and URL contracts the editor depends on, not
labels — see [tools/release_manifest.py](tools/release_manifest.py).

The shell engine takes `PROJECT_DIR` and `PIO_ENVS` from the environment for the
same reason every Python shim passes paths in: **this library is a submodule, so
anything a tool resolves from its own location points inside the library rather
than at the product using it.**

## Migrating an existing pedal onto this

[MIGRATION.md](MIGRATION.md) — the per-file inventory, the polarity traps in the
hardware seam, which tests to delete, and the order of work.

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

`bash tools/install_hooks.sh`, once per clone, points git at [.githooks/](.githooks/): its
`pre-push` runs that suite, every `tools/**/test_*.py` and the doc-link check before a push
leaves the machine. The board tools need `pcbnew`, which lives in a KiCad install rather than
on a CI runner, so those tests run here and are skipped with a message where KiCad is absent.

Unit tests for library modules live here, not in the consumers. The two
protocol drivers are tested by compiling the real `.cpp` into the test TU
behind recording stubs — see `test/test_eeprom` for the pattern.

`test/test_dfu_session` is worth singling out: the decode-and-bounds logic it
covers is what bricks a pedal when it is wrong, and in the firmware it was
extracted from it lived inside a bootloader `main.cpp` bound to CMSIS, TinyUSB
and a real flash controller, where none of it could run on a host. The
`FlashOps` seam — including `mapped()`, so an address is never assumed to be a
pointer — is what makes every rejection path testable.
