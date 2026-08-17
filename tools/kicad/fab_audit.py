# -*- coding: utf-8 -*-
"""Audit a finished fab package against the board it claims to represent.

    "C:/Program Files/KiCad/10.0/bin/python.exe" fab_audit.py <main|ctrl>

Runs AFTER gen_cpl_bom.py and export_gerbers.py, BEFORE anything is uploaded. Every check here
is a way a package that passed every board-level gate can still fail at the fab:

  * The gerber zip can be STALE. The gerber step used to be the one manual step in the package,
    and it drifted three weeks behind the CPL -- the copper being fabricated was not the copper
    being populated. The zip must be newer than the board file, and its outline must measure
    exactly what the board's Edge.Cuts measures.

  * The outline can be the wrong SIZE. JLCPCB prices by outline: 100x100 is one bracket,
    110x100 is the next (roughly double). A stray Edge.Cuts object parked off-board silently
    reprices every order, and nothing else checks the plotted extents.

  * A copper layer can be EMPTY. A 4-layer board that plots two copper files fabricates with
    blank inner layers, and nobody notices until the boards arrive.

  * Drills can fall outside capability. PTH below 0.3mm moves the order to a paid tier;
    NPTH below 0.5mm is outside JLCPCB's drilling capability entirely.

  * The BOM and CPL can DISAGREE. JLCPCB hard-errors on a designator in one file and not the
    other ("designators don't exist in the BOM/CPL file"), which is also why the master
    *-bom-full.csv with its C1-C4 range notation must never be uploaded.

  * The CPL can disagree with the BOARD: a part that moved after the CPL was written, a row
    for a bottom-side or DNP part on a single-side order, a rotation off the 90-degree grid
    that the parts.csv offset table cannot correct.

Parsing notes, each one a mis-parse this audit made before it learned better:
  - A gerber coordinate only counts if it belongs to a D01/D02/D03 operation. The format line
    %FSLAX46Y46*% contains "X46Y46", and a naive regex reads it as a point at (0,0), which
    makes every board measure 50mm oversized.
  - The CPL is in the gerber frame: absolute KiCad coordinates with Y negated. That is what
    lets JLCPCB align CPL to copper one-to-one.
  - For a part whose origin is not its body centre (the USB-C, with its offset shield tabs),
    gen_cpl_bom emits the PAD-FIELD centre, because JLC centres its placement model on the
    pins. Both references are accepted when mapping a CPL row back to its footprint.

Exit 0 = the package matches the board and is consistent with itself.
"""
import csv
import os
import re
import sys
import zipfile

import pcbnew

MM = pcbnew.ToMM

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config
from jlcpcb import PTH_MIN, NPTH_MIN


POS_TOL = 0.05      # mm, CPL row to footprint
EDGE_TOL = 0.3      # mm, plotted outline vs Edge.Cuts bbox (the bbox is stroke-inflated)

# a coordinate must belong to an operation -- see the FSLAX46Y46 note in the docstring
OP = re.compile(r"X(-?\d+)Y(-?\d+)(?:I-?\d+J-?\d+)?D0[123]\*")


def audit_gerbers(zp, board, problems):
    bb = board.GetBoardEdgesBoundingBox()
    want_w, want_h = MM(bb.GetWidth()), MM(bb.GetHeight())

    if os.path.getmtime(zp) < os.path.getmtime(board.GetFileName()):
        problems.append(f"{os.path.basename(zp)} is OLDER than the board file -- regenerate "
                        f"with export_gerbers.py; a stale zip is how copper shipped three "
                        f"weeks behind the CPL")

    z = zipfile.ZipFile(zp)
    names = z.namelist()
    coppers, edge_seen, pth_seen = [], False, False
    for n in sorted(names):
        body = z.read(n).decode("utf-8", "replace")
        if n.endswith(".gbr"):
            if "MOMM" not in body:
                problems.append(f"{n}: not millimetre units")
            pts = [(int(m.group(1)) / 1e6, int(m.group(2)) / 1e6) for m in OP.finditer(body)]
            if "Edge_Cuts" in n:
                edge_seen = True
                if not pts:
                    problems.append(f"{n}: empty outline")
                    continue
                w = max(p[0] for p in pts) - min(p[0] for p in pts)
                h = max(p[1] for p in pts) - min(p[1] for p in pts)
                if abs(w - want_w) > EDGE_TOL or abs(h - want_h) > EDGE_TOL:
                    problems.append(f"{n}: plotted outline {w:.2f}x{h:.2f}mm, board says "
                                    f"{want_w:.2f}x{want_h:.2f} -- JLCPCB prices the plot")
            elif "_Cu" in n:
                coppers.append(n)
                ops = body.count("D01*") + body.count("D03*")
                if ops < 10:
                    problems.append(f"{n}: copper layer nearly empty ({ops} operation(s))")
        elif n.endswith(".drl"):
            if "METRIC" not in body and "M71" not in body:
                problems.append(f"{n}: drill file not metric")
            tools = [float(t) for t in re.findall(r"T\d+C([\d.]+)", body)]
            hits = len(re.findall(r"^X-?[\d.]+Y-?[\d.]+", body, re.M))
            if "NPTH" in n:
                small = [t for t in tools if t < NPTH_MIN]
                if small:
                    problems.append(f"{n}: NPTH below {NPTH_MIN}mm capability: {small}")
            elif "PTH" in n:
                pth_seen = True
                small = [t for t in tools if t < PTH_MIN]
                if small:
                    problems.append(f"{n}: PTH below {PTH_MIN}mm -- paid tier: {small}")
                if hits == 0:
                    problems.append(f"{n}: PTH file has no holes")
    if len(coppers) != 4:
        problems.append(f"expected 4 copper layers in the zip, found {len(coppers)}: {coppers}")
    if not edge_seen:
        problems.append("no Edge_Cuts gerber in the zip")
    if not pth_seen:
        problems.append("no PTH drill file in the zip")
    print(f"  gerbers: {len(names)} file(s), {len(coppers)} copper, outline "
          f"checked against Edge.Cuts")


def audit_bom_cpl(board, bom_path, cpl_path, problems):
    cpl = list(csv.DictReader(open(cpl_path, encoding="utf-8-sig")))
    cols = {k.lower().replace(" ", ""): k for k in (cpl[0].keys() if cpl else [])}
    c_ref, c_rot, c_layer = cols["designator"], cols["rotation"], cols["layer"]
    c_x = cols.get("midx", cols.get("mid_x"))
    c_y = cols.get("midy", cols.get("mid_y"))

    cpl_refs = [r[c_ref] for r in cpl]
    dupes = {r for r in cpl_refs if cpl_refs.count(r) > 1}
    if dupes:
        problems.append(f"CPL duplicate designators: {sorted(dupes)}")

    for r in cpl:
        ref = r[c_ref]
        if r[c_layer].strip().lower() not in ("top", "t"):
            problems.append(f"CPL {ref}: layer {r[c_layer]!r} on a single-side (Top) order")
        rot = float(r[c_rot])
        if abs(rot % 90) > 0.01 and abs(rot % 90 - 90) > 0.01:
            problems.append(f"CPL {ref}: rotation {rot} off the 90-degree grid that the "
                            f"parts.csv offsets assume")
        fp = board.FindFootprintByReference(ref)
        if fp is None:
            problems.append(f"CPL {ref}: not on the board")
            continue
        if fp.IsFlipped():
            problems.append(f"CPL {ref}: footprint is on the BOTTOM of the board")
        if fp.IsDNP():
            problems.append(f"CPL {ref}: footprint is DNP")
        x = float(r[c_x].replace("mm", ""))
        y = float(r[c_y].replace("mm", ""))
        px, py = MM(fp.GetPosition().x), MM(fp.GetPosition().y)
        qx = [MM(p.GetPosition().x) for p in fp.Pads()]
        qy = [MM(p.GetPosition().y) for p in fp.Pads()]
        ppx, ppy = (min(qx) + max(qx)) / 2, (min(qy) + max(qy)) / 2
        # the CPL frame is absolute with Y negated (the gerber frame); origin or pad-centre
        if not any(abs(rx - x) < POS_TOL and abs(ry - (-y)) < POS_TOL
                   for rx, ry in ((px, py), (ppx, ppy))):
            problems.append(f"CPL {ref}: row ({x},{y}) matches neither the footprint origin "
                            f"({px:.2f},{py:.2f}) nor its pad-field centre ({ppx:.2f},{ppy:.2f})"
                            f" -- the part moved after the CPL was written")

    bom = list(csv.DictReader(open(bom_path, encoding="utf-8-sig")))
    bcols = {k.lower().replace(" ", "").replace("#", ""): k for k in (bom[0].keys() if bom else [])}
    b_ref = next(bcols[k] for k in bcols if "designator" in k)
    b_lcsc = next(bcols[k] for k in bcols if "lcsc" in k)
    b_qty = next((bcols[k] for k in bcols if k == "qty"), None)

    bom_refs = []
    for line in bom:
        refs = [x.strip() for x in line[b_ref].replace(";", ",").split(",") if x.strip()]
        bom_refs.extend(refs)
        lcsc = (line.get(b_lcsc) or "").strip()
        if not (lcsc.startswith("C") and lcsc[1:].isdigit()):
            problems.append(f"BOM {line[b_ref][:36]!r}: LCSC {lcsc!r} not a Cxxxx number")
        if b_qty and line.get(b_qty, "").strip().isdigit():
            if int(line[b_qty]) != len(refs):
                problems.append(f"BOM {line[b_ref][:36]!r}: Qty {line[b_qty]} but "
                                f"{len(refs)} designator(s)")

    bdup = {r for r in bom_refs if bom_refs.count(r) > 1}
    if bdup:
        problems.append(f"BOM duplicate designators: {sorted(bdup)}")
    only_cpl = set(cpl_refs) - set(bom_refs)
    only_bom = set(bom_refs) - set(cpl_refs)
    if only_cpl:
        problems.append(f"in CPL but not BOM -- JLCPCB rejects this: {sorted(only_cpl)}")
    if only_bom:
        problems.append(f"in BOM but not CPL -- JLCPCB rejects this: {sorted(only_bom)}")

    # nothing machine-placeable silently dropped
    missing = []
    for fp in board.GetFootprints():
        if fp.IsFlipped() or fp.IsDNP():
            continue
        pads = list(fp.Pads())
        if not any(p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD for p in pads):
            continue
        if any(p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for p in pads):
            continue        # hybrid SMD+PTH parts are an ordering decision, not an omission
        if fp.GetReference() not in cpl_refs:
            missing.append(fp.GetReference())
    if missing:
        problems.append(f"top-side SMD on the board but missing from the CPL: {sorted(missing)}")

    print(f"  BOM {len(bom)} line(s) / CPL {len(cpl)} row(s): designators agree, "
          f"all Top, rotations on the 90-degree grid")


def main():
    cfg = board_config.load()
    name = sys.argv[1] if len(sys.argv) > 1 else ""
    if name not in cfg.boards:
        sys.exit(f"usage: fab_audit.py <{'|'.join(sorted(cfg.boards))}>")
    prof = cfg.board(name)
    paths = {"board": prof.path("board"),
             "zip":   prof.path("gerber_zip"),
             "bom":   prof.path("bom_jlcpcb"),
             "cpl":   prof.path("cpl_jlcpcb")}
    for k, p in paths.items():
        if not os.path.exists(p):
            sys.exit(f"missing {k}: {p}")

    print(f"fab_audit: {sys.argv[1]}")
    board = pcbnew.LoadBoard(paths["board"])
    problems = []
    audit_gerbers(paths["zip"], board, problems)
    audit_bom_cpl(board, paths["bom"], paths["cpl"], problems)

    for p in problems:
        print("  XX " + p)
    if problems:
        print(f"FAB AUDIT: {len(problems)} problem(s) -- do not upload this package")
        return 1
    print("FAB AUDIT: PASS -- the package matches the board and is consistent with itself")
    return 0


if __name__ == "__main__":
    sys.exit(main())
