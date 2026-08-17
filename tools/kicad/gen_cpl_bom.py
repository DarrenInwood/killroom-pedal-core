# -*- coding: utf-8 -*-
"""Generate the JLCPCB assembly files (CPL + machine BOM) from a board.

The assembly roster is: SMD-only footprints on the profile's machine side, minus
hand-soldered refs (board_config.json) and fiducials. Everything else from the
master BOM (THT parts, panel LEDs) lands in HAND_ASSEMBLY.md with its LCSC number
for self-ordering. Run with KiCad's bundled Python:

    <kicad-python> kicad/tools/gen_cpl_bom.py <board.kicad_pcb> <profile> \\
        <bom-in.csv> <cpl-out.csv> <bom-out.csv> <hand-out.md>

The <bom-in.csv> master BOM (all parts, may use "C1-C4" range notation) lives OUTSIDE
the fab/ folder so it is never mistaken for the upload artifact — JLCPCB does not
expand ranges, so uploading the master directly mis-matches against the CPL.

Every path is an argument rather than read from board_config.json, because this
writes four files and a tool that both chooses its outputs and writes them can
overwrite a package nobody meant to regenerate. The product's wrapper names them;
the config is only consulted for the roster (which side, which refs are hand-fitted
or global-sourced, which footprints need JLC's 270° model correction).

Only the fab/*-jlcpcb.csv (individual designators, machine parts) get uploaded.
"""
import csv
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config
import jlcpcb

MM = pcbnew.ToMM


def expand(token):
    """'C601-C605' -> C601..C605; plain refs pass through."""
    if "-" in token:
        a, z = token.split("-", 1)
        pa = a.rstrip("0123456789")
        pz = z.rstrip("0123456789")
        if pa == pz and pa:
            lo, hi = int(a[len(pa):]), int(z[len(pz):])
            if lo < hi:
                return [pa + str(i) for i in range(lo, hi + 1)]
    return [token]


def load_ref_lcsc(bom_in):
    """Map every designator -> its LCSC part number from the master BOM (for rotation keying)."""
    ref2lcsc = {}
    for r in csv.reader(open(bom_in, newline="", encoding="utf-8-sig")):
        if len(r) < 5 or not r[4].strip():
            continue
        for tok in r[1].split(","):
            for ref in expand(tok.strip()) if tok.strip() else []:
                ref2lcsc[ref] = r[4].strip()
    return ref2lcsc


# JLCPCB's assembly library draws each part at its own default angle, so the exported CPL
# rotation needs a per-part offset or the part is soldered turned. BOTTOM-side parts are
# additionally mirrored on the Y axis (180 - angle) BEFORE the offset -- JLCPCB's
# convention, matching the kicad-jlcpcb-tools rotation model.
#
# The correction is keyed by LCSC part, NOT package: two parts sharing one KiCad footprint
# can need different angles because JLC drew them at different reel orientations (e.g.
# SGM2210 vs OPA1677, both SOT-23-5, verified 90 apart on the assembly preview). Values
# here are verified against JLCPCB's preview; anything unlisted falls back to the
# per-package default -- +270 for the 2-row SO-family ICs / LQFP / relay (KiCad's pin-1
# top-left reads 90 off in JLC's library), 0 otherwise.






def mid_xy(fp, lcsc):
    if lcsc in jlcpcb.CENTROID_LCSC:
        xs = [pd.GetPosition().x for pd in fp.Pads()]
        ys = [pd.GetPosition().y for pd in fp.Pads()]
        return ((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
    p = fp.GetPosition()
    return p.x, p.y


def main():
    board_path, profile_name, bom_in, cpl_out, bom_out, hand_out = sys.argv[1:7]
    profile = board_config.load().board(profile_name)
    b = board_config.load_board(pcbnew, board_path)
    ref2lcsc = load_ref_lcsc(bom_in)

    roster = {}
    for fp in b.GetFootprints():
        ref = fp.GetReference()
        if ref.startswith("FID") or ref in profile["hand_solder"]:
            continue
        pads = list(fp.Pads())
        has_smd = any(p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD for p in pads)
        has_pth = any(p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for p in pads)
        if not has_smd or (has_pth and ref not in profile.get("mixed_smd", set())):
            continue
        side = "B" if fp.IsFlipped() else "F"
        if side != profile["smd_side"]:
            continue
        roster[ref] = fp

    with open(cpl_out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for ref in sorted(roster, key=lambda r: (r.rstrip("0123456789"), int("0" + "".join(ch for ch in r if ch.isdigit())))):
            fp = roster[ref]
            mx, my = mid_xy(fp, ref2lcsc.get(ref, ""))
            rot = fp.GetOrientationDegrees()
            if fp.IsFlipped():
                rot = (180 - rot) % 360      # JLC mirrors bottom-side angles on Y
            rot = (rot + jlcpcb.rotation_correction(fp.GetFPID().GetUniStringLibId(),
                                                    ref2lcsc.get(ref, ""),
                                                    profile.refs("rotate_270"))) % 360
            w.writerow([ref, "%.6f" % MM(mx), "%.6f" % (-MM(my)),
                        "Bottom" if fp.IsFlipped() else "Top",
                        "%.6f" % rot])

    rows = list(csv.reader(open(bom_in, newline="", encoding="utf-8-sig")))
    hdr, body = rows[0], rows[1:]
    machine, hand = [hdr], []
    for r in body:
        refs = [e for x in r[1].split(",") if x.strip() for e in expand(x.strip())]
        keep = [x for x in refs if x in roster]
        drop = [x for x in refs if x not in roster]
        if keep:
            machine.append([r[0], ",".join(keep), r[2], str(len(keep)), r[4] if len(r) > 4 else ""])
        if drop:
            hand.append((r[0], ",".join(drop), r[2], len(drop), r[4] if len(r) > 4 else ""))
    with open(bom_out, "w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerows(machine)

    with open(hand_out, "w", encoding="utf-8") as f:
        f.write("# Hand-assembled parts — %s\n\n" % os.path.basename(board_path))
        f.write("These parts are NOT in the JLCPCB assembly BOM/CPL. Source and solder them\n")
        f.write("during final assembly (LCSC numbers given where known).\n\n")
        f.write("| Comment | Designators | Footprint | Qty | LCSC |\n|---|---|---|---|---|\n")
        for (c, refs, fpt, q, lcsc) in hand:
            f.write("| %s | %s | %s | %d | %s |\n" % (c, refs, fpt.split(":")[-1], q, lcsc or "—"))

    print("CPL rows: %d | machine BOM lines: %d | hand lines: %d" % (len(roster), len(machine) - 1, len(hand)))

    # A machine-placed part with no LCSC number is a part the assembler cannot buy. The line still
    # uploads -- JLCPCB simply cannot match it, and the order stalls at the review step or, worse,
    # someone picks a substitute. It is not an error KiCad or DRC can see, because it is not about the
    # board; it is about the purchase. So it is checked HERE, and it FAILS, because a silently
    # incomplete BOM looks exactly like a complete one.
    #
    # If a part genuinely cannot be machine-placed (a specialty audio IC that LCSC does not stock),
    # the answer is to put its ref in board_config.json's "hand_solder", not to ship a blank line: that
    # moves it to HAND_ASSEMBLY.md where somebody will actually see it. The exception is a part JLC
    # WILL place but through Global Sourcing, so its C9900... number is not assigned yet: those go in
    # board_config.json's "global_source" -- they belong in the CPL/BOM, exempt here until sourced.
    gs = profile.get("global_source", set())
    def all_global(r):
        refs = [x for x in r[1].split(",") if x]
        return bool(refs) and all(x in gs for x in refs)
    blank = [r for r in machine[1:] if not (r[4] or "").strip()]
    pending = [r for r in blank if all_global(r)]
    hard = [r for r in blank if not all_global(r)]

    if pending:
        n = sum(int(r[3]) for r in pending)
        print("\nGLOBAL SOURCING: %d line(s), %d part(s) machine-placed with LCSC pending (exempt):" % (len(pending), n))
        for r in pending:
            print("   %-16s %-30s x%-3s %s" % (r[0], r[2].split(":")[-1], r[3], r[1]))
        print("   -> JLC places these via Global Sourcing; add the C9900... number before ordering.")
    if hard:
        n = sum(int(r[3]) for r in hard)
        print("\nBOM GATE: %d machine-placed line(s), %d part(s), have NO LCSC number:" % (len(hard), n))
        for r in hard:
            print("   %-16s %-30s x%-3s %s" % (r[0], r[2].split(":")[-1], r[3], r[1]))
        print("\nJLCPCB cannot source these. Give them a part number, move them to hand_solder, or\n"
              "add them to global_source if JLC will place them via Global Sourcing.")
        return 1

    print("BOM GATE: PASS — every machine-placed part has an LCSC number or is global-sourced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
