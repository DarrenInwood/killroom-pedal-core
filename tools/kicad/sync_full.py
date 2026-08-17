"""Sync a board to its schematic netlist: footprints, packages, values, and every pad's net.

This replaces KiCad's "Update PCB from Schematic" because the MCP's sync_schematic_to_board is
actively unsafe -- it creates only NAMED nets (skipping every auto-named local net), it merges pads
onto a bogus net called "PWR_FLAG", and it writes the file while reporting that it refused to.

    python sync_full.py <board.kicad_pcb> <netlist.net> <fp-lib-table-dir>

Footprints whose refdes is not in the schematic are removed UNLESS they are board-only by design
(fiducials and mounting holes) -- KiCad's own sync offers to delete those and it is the wrong answer.

SWIG discipline: pcbnew invalidates the board's footprint container the moment you Remove()/Add(),
and reloading the board while Python still holds footprints from the old one segfaults. So the board
container is walked EXACTLY ONCE, into `fps`, and every later pass iterates that dict instead.
Always verify the result with verify_sync.py -- a tool reporting a count is not evidence.
"""
import os
import re
import subprocess
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config
import kicad_env
import netlist as netlist_mod

STOCK = kicad_env.stock_footprints()
KEEP_PREFIX = ("FID", "H")   # fiducials + mounting holes live only on the board




def lib_path(nick, proj_dir):
    tbl = os.path.join(proj_dir, "fp-lib-table")
    if os.path.exists(tbl):
        for m in re.finditer(r'\(name "([^"]+)"\).*?\(uri "([^"]+)"\)',
                             open(tbl, encoding="utf-8").read()):
            if m.group(1) == nick:
                return m.group(2).replace("${KIPRJMOD}", proj_dir)
    return os.path.join(STOCK, nick + ".pretty")


def load(ref, fpid, proj_dir):
    """Load a footprint from the library, with its LIB_ID nickname set.

    FootprintLoad() leaves the nickname empty, which makes the board read `:SOT-23-5` and silently
    breaks any later comparison against the schematic's `Package_TO_SOT_SMD:SOT-23-5`.
    """
    if ":" not in fpid:
        sys.exit(f"{ref}: no footprint assigned in the schematic")
    nick, name = fpid.split(":", 1)
    # pcbnew.FootprintLoad's internal IO plugin INTERMITTENTLY comes back as a bare SwigPyObject
    # with no methods -- the same SWIG flakiness fpid_of() guards against -- so the call raises
    # AttributeError on a bad draw. It succeeds on a retry; loop rather than die on one unlucky call.
    fp = None
    for _ in range(20):
        try:
            fp = pcbnew.FootprintLoad(lib_path(nick, proj_dir), name)
        except AttributeError:
            fp = None
        if fp is not None:
            break
    if fp is None:
        sys.exit(f"{ref}: could not load {fpid} from {lib_path(nick, proj_dir)}")
    fp.SetFPID(pcbnew.LIB_ID(nick, name))
    return fp


def fpid_of(fp):
    """'lib:name' for a footprint.

    GetFPID() intermittently comes back as a raw SwigPyObject with no methods on it, so go through
    the string accessor and only fall back to the object when that is unavailable.
    """
    try:
        return fp.GetFPIDAsString()
    except AttributeError:
        i = fp.GetFPID()
        return f"{i.GetLibNickname().wx_str()}:{i.GetLibItemName().wx_str()}"


board_path, net_path, proj_dir = sys.argv[1], sys.argv[2], sys.argv[3]
_nl = netlist_mod.parse(net_path)
comps, pins, ptypes = _nl.comps, _nl.pins, _nl.types
b = board_config.load_board(pcbnew, board_path)

# The one and only walk of the board's footprint container.
fps = {fp.GetReference(): fp for fp in b.GetFootprints()}

# --- 1. remove footprints the schematic no longer has ---------------------------------------------
stale = sorted(r for r in fps if r not in comps and not r.startswith(KEEP_PREFIX))
for r in stale:
    b.Remove(fps.pop(r))
print(f"removed {len(stale)} stale footprint(s): {' '.join(stale)}")

# --- 2. add footprints the board does not have yet -------------------------------------------------
gx, gy, col = 55.0, 55.0, 0
added = []
for ref in sorted(set(comps) - set(fps)):
    value, fpid = comps[ref]
    fp = load(ref, fpid, proj_dir)
    fp.SetReference(ref)
    fp.SetValue(value)
    # position is scratch -- every footprint is about to be re-placed to the floorplan
    fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(gx + 10.0 * (col % 4)),
                                   pcbnew.FromMM(gy + 10.0 * (col // 4))))
    col += 1
    b.Add(fp)
    fps[ref] = fp
    added.append(f"{ref}({fpid.split(':')[1]})")
print(f"added {len(added)} footprint(s): {' '.join(added)}")

# --- 3. replace footprints whose package no longer matches the schematic ---------------------------
# Refdes get reused. U101 was the buck (SOT-583-8) and came back as an SGM2210 (SOT-23-5); U102/U103
# were MIC29302s in TO-263-5. The board kept the old 8- and 10-pad packages under the new refdes, and
# a pad-net check cannot see it -- the surplus pads simply carry no net, which looks exactly like an
# unconnected pin. Compare the package itself.
#
# ONE SWAP PER PROCESS. b.Remove() poisons more than the footprint container: the very next
# pcbnew.FootprintLoad() returns None forever after, so the SECOND swap in a run can never fetch
# its replacement. Converting the three inter-board connectors to box headers is what surfaced it
# -- J606 swapped, then J607 died with "could not load Connector_IDC:...", a library that loads
# perfectly well in any process that has not called Remove().
swapped = []
for ref in sorted(r for r in fps if r in comps):
    old = fps[ref]
    value, fpid = comps[ref]
    if fpid_of(old) == fpid:
        continue
    if swapped:
        break
    new = load(ref, fpid, proj_dir)
    new.SetReference(ref)
    new.SetValue(value)
    new.SetPosition(old.GetPosition())
    new.SetOrientation(old.GetOrientation())
    was_flipped = old.IsFlipped()
    swapped.append(f"{ref}: {fpid_of(old)} -> {fpid}")
    b.Remove(old)
    b.Add(new)
    # Flip only AFTER Add(). A footprint the board does not own yet has no layer mapping to flip
    # through, and pcbnew segfaults. Every control-board part is flipped (its SMD side is Bottom),
    # so this is the common path there, not a corner case.
    if was_flipped:
        new.Flip(new.GetPosition(), False)
    fps[ref] = new
print(f"replaced {len(swapped)} footprint(s)" + ("\n    " + "\n    ".join(swapped) if swapped else ""))

# b.Remove() POISONS THE FOOTPRINT CONTAINER FOR THE REST OF THE PROCESS. Not just the handle that
# was removed -- every handle taken before it, and every handle taken after it: a fresh
# b.GetFootprints() then yields bare SwigPyObjects with no methods on them at all, so the next
# fp.Pads() or fp.GetReference() dies. Re-snapshotting does not help; nor does reloading the board,
# which segfaults while Python still holds footprints from the old one.
#
# So a swap ends this process. Save what we have, then re-run ourselves on a clean load to do the
# nets. The second pass finds the packages already correct and swaps nothing.
#
# Each pass does one swap and hands the rest to the next, so a board with N package changes costs
# N+1 passes. `pass` is bounded: without it a footprint that swaps but still compares unequal would
# re-run for ever, and the board has 104 refs it could do that with.
PASS_LIMIT = 40
depth = next((int(a.split("=")[1]) for a in sys.argv if a.startswith("--pass=")), 0)
if swapped:
    if depth >= PASS_LIMIT:
        sys.exit(f"still swapping packages after {depth} passes -- {swapped[0]} is not settling")
    pcbnew.SaveBoard(board_path, b)
    print(f"saved the package swap; re-running on a clean load ({depth + 1})")
    # argv[0], NOT __file__: this file lives in a submodule, and re-execing it
    # directly would run the library copy with none of the product shim's path
    # setup. It only fires on a board that needs a package swap, so a smoke test
    # that swaps nothing would never notice.
    r = subprocess.run([sys.executable, os.path.abspath(sys.argv[0]),
                        board_path, net_path, proj_dir, f"--pass={depth + 1}"])
    sys.exit(r.returncode)

# --- 4. refresh values --------------------------------------------------------------------------
# A value only set at insert time goes stale the moment the part is re-specced, and the value is what
# reaches the silkscreen and the BOM. RLY1 sat on the board as a TQ2SA-3V-Z long after the schematic
# had moved to the 9V coil.
revalued = []
for ref, fp in fps.items():
    if ref not in comps:
        continue
    want = comps[ref][0]
    if fp.GetValue() != want:
        revalued.append(f"{ref}: {fp.GetValue()!r} -> {want!r}")
        fp.SetValue(want)
print(f"revalued {len(revalued)} footprint(s)" + (": " + "; ".join(revalued) if revalued else ""))

# --- 5. every net in the schematic must exist -----------------------------------------------------
made = 0
for name in sorted(set(pins.values())):
    if b.FindNet(name) is None:
        b.Add(pcbnew.NETINFO_ITEM(b, name))
        made += 1
print(f"created {made} missing net(s)")

# --- 6. assign every pad from the netlist ---------------------------------------------------------
assigned, cleared = 0, 0
for ref, fp in fps.items():
    for pad in fp.Pads():
        if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
            continue
        exp = pins.get((ref, pad.GetNumber()))
        if exp is None:
            if pad.GetNetCode() != 0:
                pad.SetNetCode(0)
                cleared += 1
            continue
        net = b.FindNet(exp)
        if pad.GetNetCode() != net.GetNetCode():
            pad.SetNet(net)
            assigned += 1
        # Carry the pin TYPE across too. Without it the board cannot tell a VCC pin from a WP pin
        # tied high to the same rail -- they look identical, both just "a pad on +3V3" -- and every
        # check downstream ends up demanding a bypass cap for a logic input.
        t, f = ptypes.get((ref, pad.GetNumber()), ("", ""))
        if t:
            pad.SetPinType(t)
        if f:
            pad.SetPinFunction(f)
print(f"assigned {assigned} pad net(s); cleared {cleared} orphan(s)")

pcbnew.SaveBoard(board_path, b)
print("saved", board_path)
