# -*- coding: utf-8 -*-
"""Pin netlist.py against a fixture.

The rest of this directory needs pcbnew and a board, so none of it runs in CI.
netlist.py is the exception -- pure standard library, no KiCad -- and it is also
the piece three tools now share, so a change to it is a change to all of them.
That makes it worth the only real coverage this directory can have.

The fixture exercises what the three parsers it replaced each handled separately:
a component with a sheet path, one without a footprint, pin types, pin functions,
and a net whose name is KiCad's generated form.

    python tools/kicad/test_netlist.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata", "sample.net")

failures = []


def check(what, got, want):
    if got != want:
        failures.append("%s\n     got: %r\n    want: %r" % (what, got, want))


nl = netlist.parse(FIXTURE)

check("comps", nl.comps, {
    "R1":   ("10k", "Resistor_SMD:R_0603_1608Metric"),
    "C1":   ("100nF", "Capacitor_SMD:C_0603_1608Metric"),
    "U1":   ("OPA1678", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    # No footprint of its own -- a board-only part still has to be seen, or the
    # sync that reads this would decide it was deleted from the schematic.
    "FID1": ("Fiducial", ""),
})

check("pins", nl.pins, {
    ("R1", "2"): "GND", ("C1", "2"): "GND", ("U1", "4"): "GND",
    ("C1", "1"): "VCC", ("U1", "8"): "VCC",
    ("R1", "1"): "Net-(R1-Pad1)", ("U1", "3"): "Net-(R1-Pad1)",
})

check("sheets", nl.sheets, {"R1": "/", "C1": "/Power/", "U1": "/Audio/", "FID1": "/"})

check("types", nl.types, {
    ("R1", "2"): ("passive", ""), ("C1", "2"): ("passive", ""),
    ("U1", "4"): ("power_in", "V-"),
    ("C1", "1"): ("passive", ""), ("U1", "8"): ("power_in", "V+"),
    ("R1", "1"): ("passive", ""), ("U1", "3"): ("input", "+"),
})

# Every pin has a types entry, even when the netlist gave neither field: the
# callers index types by the same key as pins and would KeyError otherwise.
check("types covers pins", sorted(nl.types), sorted(nl.pins))

if failures:
    print("test_netlist: %d failure(s)\n" % len(failures))
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("test_netlist: PASS (%d comps, %d pins, %d sheets, %d types)"
      % (len(nl.comps), len(nl.pins), len(nl.sheets), len(nl.types)))
