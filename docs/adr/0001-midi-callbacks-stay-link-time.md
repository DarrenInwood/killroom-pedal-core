# ADR-0001: The MIDI callbacks stay link-time

**Status**: Accepted — 2026-08-29

## Context

`midi_handler` parses MIDI from both transports and hands each parsed event to the product
through seven `extern "C"` symbols the product's `main.cpp` resolves at link time:

```
on_midi_cc   on_midi_note_on   on_midi_note_off   on_midi_program_change
on_midi_sysex   on_midi_clock   on_midi_clock_reset
```

An architecture review proposed replacing them with a `MidiListener` interface set once through
`midi_handler::set_listener()`. The case for it is real:

- The seven symbols are untyped. Nothing checks a product implemented them with the right
  signatures beyond the linker's own name matching.
- Any host suite that touches MIDI must define all seven, whether or not it cares about them.
- Two pedals cannot exist in one binary, because the symbols are global.
- What a message means leaves the library through C symbols and comes straight back into
  `PedalBase` — a round trip out and back in, through a seam with no type.

## Decision

The callbacks stay as they are.

## Consequences

The reasons, so this is not re-proposed each time someone reads `midi_handler.cpp`:

**It fails the deletion test.** Delete a `MidiListener` and the seven symbols come back. The
complexity moves rather than concentrating, which is the signal that separates a deepening from
a rearrangement.

**One adapter is a hypothetical seam.** In a shipped pedal there is exactly one implementation —
the product's. A recording listener in a test suite would be the second, but the suites already
define the seven symbols locally without difficulty, so the seam would be introduced for a
variation that is not actually wanted.

**The weak-symbol default already solves the awkward case.** `on_midi_note_off` is weak, defined
empty in `midi_note_off_default.cpp`, so a product that tracks note lifetimes overrides it and
one that does not links without naming it. That is the mechanism working exactly as designed,
and it generalises to any callback that later needs a default.

**A virtual call per message is small but not free.** These pedals run on STM32 M0 and M4 parts,
and the MIDI path is on the superloop. The cost is not the objection on its own — it is that
there is nothing being bought with it.

## When to reopen

If a second adapter becomes genuinely wanted — two MIDI-consuming objects in one binary, or a
product that needs to swap what consumes MIDI at runtime — the balance changes and this should
be revisited. Testability alone is not that reason: the suites manage today.
