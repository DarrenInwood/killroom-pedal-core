# Triage Labels

The skills speak in terms of five canonical triage roles. This file maps those roles to the
actual label strings used in this repo's issue tracker.

| Label in mattpocock/skills | Label in our tracker | Meaning                                  |
| -------------------------- | -------------------- | ---------------------------------------- |
| `needs-triage`             | `needs-triage`       | Maintainer needs to evaluate this issue  |
| `needs-info`               | `needs-info`         | Waiting on reporter for more information |
| `ready-for-agent`          | `ready-for-agent`    | Fully specified, ready for an AFK agent  |
| `ready-for-human`          | `ready-for-human`    | Requires human implementation            |
| `wontfix`                  | `wontfix`            | Will not be actioned                     |

When a skill mentions a role (e.g. "apply the AFK-ready triage label"), use the corresponding
label string from this table.

Every label string here equals its role name, so nothing has to be translated. The five are the
same words the multi-effect uses, which is deliberate: an issue that moves between the two
trackers keeps its state.

## `area:` — which part of the library

One per issue. The library is a set of modules that products compose, so the areas are the
groupings a reader would use to find related work, not directories:

| Label            | Covers                                                                        |
| ---------------- | ----------------------------------------------------------------------------- |
| `area:tempo`     | `tap_tempo`, `tempo_controller`, `tempo_led` — the tempo engine and its state machine |
| `area:midi`      | `midi_handler`, `midi_clock_out`, `midi_responder_base`, `sysex_codec`, `wire_protocol`, `usb_midi_cin` |
| `area:storage`   | `eeprom`, `block_store` — the persistence layer and its wear handling          |
| `area:ui`        | `display`, `font`/`font_data`, `ui/`, `encoder_decode`, `encoder_rotate`       |
| `area:controls`  | `footswitch`, `external_input`, `expr_map`, `adc_filter`, `adc_map`, `bypass`  |
| `area:dfu`       | `app_image`, `dfu_protocol`, `dfu_session`, `dfu_progress`, `crc16`            |
| `area:platform`  | `hal`, `pedal_base`, `ialgorithm`, `mcu_uid`, `vbus_debounce`, `param_scale`, `frame_dump` |

`area:platform` is the one to watch: it holds the interfaces every product implements, so a
change there is the most likely to be a breaking one.

## No `kind:` axis here

The multi-effect carries a second axis (`kind:defect` … `kind:doc`) because its autonomous loop
reads a priority order off it. Nothing here consumes such an ordering, so the labels would be
written and never read. Add them the day something does.
