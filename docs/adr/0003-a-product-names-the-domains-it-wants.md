# ADR-0003: A product names the domains it wants

**Status**: Accepted — 2026-08-31

## Context

The optional domains — the tempo layer, the external-input jack — were gated on whether the
product happened to have supplied that domain's config header:

```cpp
#if __has_include("pedal_core_tempo_config.hpp")
```

Four translation units did this. Twenty-three files depended on a product-supplied config
header, so the guard was not the rule, it was the exception; and the four it covered were not
the only files that reached for one. A product with no `pedal_core_tempo_config.hpp` therefore
got three different failures depending on what it touched:

| what the product does | what it got |
|---|---|
| calls `tap_tempo::tap()` | compiles, then a **link error** for a symbol, somewhere unrelated |
| includes `tempo_controller.hpp` | a compile error on the config include, with no reason given |
| builds `midi_clock_out.cpp` | a compile error — that file had no guard at all |
| includes `external_input.hpp` | a compile error on the config include |

CLAUDE.md documented the first of those as "the trap" and taught a workaround: *when a symbol
goes missing, check the guard before the linker flags.* A documented workaround for a
diagnosable condition is the definition of a shallow seam — the interface a product had to know
was which four of the eight files were guarded, which two headers would hard-fail anyway, and
that a missing symbol might mean a missing header.

The absent branch was also never compiled by anything. `-Itest/support` puts every config
header on the include path, so `pio test -e native` only ever built the present side of all
four guards: the trap was structurally invisible to the suite meant to protect against it.

## Decision

A product supplies `pedal_core_features.hpp` naming every optional domain:

```cpp
#define PEDAL_CORE_HAS_TEMPO    1
#define PEDAL_CORE_HAS_EXTINPUT 0
```

- The modules of a domain that is off compile to nothing, as before, but on the switch rather
  than on a file's presence.
- The headers of a domain that is off `#error` with the switch's name and what to do.
- A switch left **undefined** is an `#error`, not a zero. This is the part that closes the
  trap: the old guard could not tell "no tempo wanted" from "tempo wanted, header forgotten",
  and answered both by producing no symbols.
- Presence of a config header no longer gates anything. A domain switched on obliges its
  config header, and that failure is an ordinary "no such file" at the file that needs it.

## Consequences

**One failure mode instead of three, at the file that caused it.** Every error names the
switch and the header to supply.

**Both sides are built.** `native_minimal` compiles the library with every domain off and runs
one suite. The pre-push hook and CI run it beside `native`. That env is why this is a decision
rather than a hope: the branch is exercised.

**`midi_clock_out.cpp` joined the set**, having had no guard at all. Its absent side is built
by donor-include in `test_features_absent`, because it is deliberately outside the native src
filter — it needs `midi_handler` symbols that build does not carry.

**The library's own stub defers to the build.** `test/support/pedal_core_features.hpp` uses
`#ifndef` so `native_minimal` can override with `-D`. A product's own copy should simply
define each switch; the indirection exists because this repo compiles both sides, and a second
stub file of the same name would be resolved differently depending on which directory the
including translation unit sat in — a quoted include searches that directory first.

**It costs every product a file.** This is breaking, and deliberately part of the group of
breaking changes landed together so the family takes one co-ordinated re-pin.

## When to reopen

If the family ever grows a domain whose presence genuinely cannot be known at compile time,
the switch is the wrong shape for it. Nothing like that exists today: a pedal either has a
tempo or it does not, and it knows which when it is built.

`__has_include` is not the alternative to revisit. It conflates "not wanted" with "not yet
written", and the failure it produces is a missing symbol reported somewhere else.
