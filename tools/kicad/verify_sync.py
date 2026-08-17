"""Check a synced board against the schematic netlist: package, pad set, value, and every pad's net.

A sync tool reporting "success, 216 pads assigned" is not evidence. Three separate things have to
agree with the schematic, and each has been wrong on this project at some point:

  - the NET on every pad. The MCP's sync created only named nets and left 85 control-board pads with
    no net at all, including the buck's whole switching loop.
  - the PACKAGE. Refdes get reused: U101 was the buck (SOT-583-8) and came back as an SGM2210
    (SOT-23-5), U102/U103 were MIC29302s in TO-263-5. The board kept the old 8- and 10-pad packages
    under the new refdes. A pad-net check cannot see this -- the surplus pads simply carry no net,
    which looks exactly like an intentionally unconnected pin. Compare the package itself.
  - the VALUE. It is what reaches the silkscreen and the BOM. RLY1 sat on the board as a TQ2SA-3V-Z
    long after the schematic had moved to the 9V coil.

    python verify_sync.py <board.kicad_pcb> <netlist.net>
"""
import os
import re
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config
import netlist as netlist_mod

# board-only by design: fiducials and mounting holes have no schematic symbol
# Refs that live only on the board -- fiducials and mounting holes have no
# schematic symbol, and KiCad's own sync offers to delete them, which is the
# wrong answer. The prefixes are the product's, so they come from its config.
_cfg_board = board_config.load().board_for_path(sys.argv[1])
BOARD_ONLY = tuple(_cfg_board.get("board_only_prefixes") or ("FID", "H"))




def fpid_of(fp):
    """'lib:name' for a footprint. GetFPID() intermittently returns a bare SwigPyObject."""
    try:
        return fp.GetFPIDAsString()
    except AttributeError:
        i = fp.GetFPID()
        return f"{i.GetLibNickname().wx_str()}:{i.GetLibItemName().wx_str()}"


_nl = netlist_mod.parse(sys.argv[2])
pins, comps = _nl.pins, _nl.comps
b = pcbnew.LoadBoard(sys.argv[1])

nonet, wrong, extra, pkg, val, missing = [], [], [], [], [], []
ok = 0
seen = set()

for fp in b.GetFootprints():
    ref = fp.GetReference()
    if ref.startswith(BOARD_ONLY) and ref not in comps:
        continue
    if ref not in comps:
        extra.append(f"{ref} is on the board but not in the schematic")
        continue
    seen.add(ref)
    want_val, want_fpid = comps[ref]

    if fpid_of(fp) != want_fpid:
        pkg.append(f"{ref}: board {fpid_of(fp)}, schematic {want_fpid}")
    if fp.GetValue() != want_val:
        val.append(f"{ref}: board {fp.GetValue()!r}, schematic {want_val!r}")

    for pad in fp.Pads():
        num = pad.GetNumber()
        if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
            continue
        exp = pins.get((ref, num))
        got = pad.GetNetname()
        if exp is None:
            # a pad the schematic has no pin for: either a genuinely unconnected pin, or -- the case
            # that matters -- a leftover pad from the wrong package. The package check above decides.
            if got:
                wrong.append(f"{ref}.{num} on {got!r} but the schematic has no such pin")
            continue
        if not got:
            nonet.append(f"{ref}.{num} has NO net (schematic says {exp})")
        elif got != exp:
            wrong.append(f"{ref}.{num} on {got!r}, schematic says {exp!r}")
        else:
            ok += 1

missing = sorted(set(comps) - seen)

print(f"pads matching schematic  : {ok}")
print(f"pads with NO net         : {len(nonet)}")
print(f"pads on the WRONG net    : {len(wrong)}")
print(f"WRONG package            : {len(pkg)}")
print(f"WRONG value              : {len(val)}")
print(f"in schematic, not on board: {len(missing)}")
print(f"on board, not in schematic: {len(extra)}")
for group in (nonet, wrong, pkg, val):
    for l in group[:20]:
        print("   XX", l)
for l in missing[:20]:
    print("   XX", l, "is in the schematic but not on the board")
for l in extra[:20]:
    print("   XX", l)

fails = len(nonet) + len(wrong) + len(pkg) + len(val) + len(missing) + len(extra)
print("SYNC VERIFY: PASS" if not fails else f"SYNC VERIFY: {fails} finding(s)")
sys.exit(1 if fails else 0)
