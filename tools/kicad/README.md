# The family's KiCad gates

The checks that decide whether a board can be ordered, and the generators that
build the package it is ordered with. They are shared because they encode the fab
house's rules and the pipeline's hard-won lessons, neither of which is a property
of any one board.

| File | What it is |
|---|---|
| [dfm_gate.py](dfm_gate.py) | Every rule that costs money: via drill and diameter, mask tenting, NPTH size, fiducial-to-edge, banned passive packages, the size price break, single-side placement |
| [gen_cpl_bom.py](gen_cpl_bom.py) | The JLCPCB CPL and machine BOM from the board, plus `HAND_ASSEMBLY.md` for everything self-fitted |
| [export_gerbers.py](export_gerbers.py) | A gerber + drill zip, from the board that is actually on disk |
| [fab_audit.py](fab_audit.py) | Audits a finished package: zip freshness, outline size, empty copper layers, drill limits, BOM/CPL agreement, CPL positions against the board |
| [jlcpcb.py](jlcpcb.py) | The vendor's own figures — thresholds, the LCSC rotation table, the centroid exceptions |
| [board_config.py](board_config.py) | Reads a product's `kicad/board_config.json` |
| [via_clearance.py](via_clearance.py) | How much room a via laid *after* routing leaves around other copper, per netclass |
| [kicad_env.py](kicad_env.py) | Finds KiCad on this machine |

## The three-way split

Everything these tools need falls into exactly one of three places, and which one
is decided by what makes the value change:

| Changes when… | Lives in | Committed |
|---|---|---|
| JLCPCB changes its rules | [jlcpcb.py](jlcpcb.py) | yes, here |
| the product's boards change | `<repo>/kicad/board_config.json` | yes, in the product |
| you sit at a different computer | env vars → `<repo>/kicad/tools/local.env` | **no** |

The third row is the one worth insisting on. A committed
`C:/Program Files/KiCad/10.0` is wrong on the second machine and wrong in CI, and
whoever fixes it breaks the first one.

## How a product uses them

A shim per tool in `<repo>/kicad/tools/`, so the command line is the same in every
repository and `route_board.sh` needs no knowledge of where anything lives:

```python
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CORE = os.path.join(ROOT, "firmware", "lib", "pedal-core", "tools", "kicad")
sys.path.insert(0, CORE)
from dfm_gate import main
sys.exit(main())
```

`kicad/board_config.json` describes the boards. Any key beginning with `_` is
ignored, which is where the reason for a value goes:

```jsonc
{
  "schema": 1,
  "vendor": "jlcpcb",
  "boards": {
    "main": {
      "_why": "4-layer; B.Cu carries the hand-laid clocks, so In1 stays a solid plane.",
      "dir": "kicad/main", "board": "main.kicad_pcb",
      "gerber_zip": "fab/main_gerbers_jlcpcb.zip",
      "bom_jlcpcb": "fab/main-bom-jlcpcb.csv",
      "cpl_jlcpcb": "fab/main-cpl-jlcpcb.csv",
      "layers": 4, "smd_side": "F", "max_size": [100.0, 100.0],
      "_stitch_clearance": "protect_hiz_nodes wants 0.35mm around the CV nodes and
                            clock_away_from_audio 0.4mm around the clocks. Both are KiCad
                            custom rules, which the SWIG API cannot read, so the numbers
                            are repeated here for the tools that place copper after the
                            router has finished.",
      "stitch_clearance": { "HiZ_Audio": 0.35, "Clock": 0.4 },
      "hand_solder": [], "global_source": [], "mixed_smd": [], "rotate_270": []
    }
  }
}
```

**A missing key is not always an error.** A board with no outline yet has no size
to check against, so `max_size` is left out and `dfm_gate` says it skipped the
price-break check. A config that guesses reads exactly like a config that knows,
which is worse than one that admits what it does not.

## Two rules for editing these files

**Locate the product from `sys.argv[0]`, never `__file__`.** This directory is a
submodule. A path resolved from `__file__` points *inside the library*, so a tool
that does it finds the wrong repository or none — and `fab_audit` did exactly that
before it moved here. The same trap catches a subprocess re-exec: re-exec
`sys.argv[0]`, or the child runs the library copy with none of the shim's setup.

**Stay in the conservative dialect.** These run under KiCad's bundled Python
(3.11 today, older on an older KiCad), and they are read beside the product's own
`kicad/tools/` scripts, which use `%`-formatting and no annotations. The tools
directly above this one run on system Python and use f-strings freely. Two
dialects, one repository — the boundary is this directory.

## What is not here

`pcbnew` cannot be imported on a GitHub runner, so **nothing in this directory is
exercised by CI**; the repo's workflow only checks that it parses. These tools are
proved by the consumers' golden diffs — a package regenerated in place must leave
`git diff` clean — which is the check to run after touching any of them.

Placement, routing, GND integrity and schematic sync are still product-local.
They are the natural next things to move, and they move when a second board
exists to prove them generic.
