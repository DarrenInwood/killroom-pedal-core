# -*- coding: utf-8 -*-
"""JLCPCB DFM cost gate.

Checks a board against the rules that cost money: via drill and diameter, mask
tenting, non-plated hole size, fiducial-to-edge, banned passive packages, board
size against the price break, and the single-side placement Economic PCBA needs.
Every threshold is JLCPCB's and lives in jlcpcb.py; every figure about a
particular board -- which side is machine-placed, which refs are hand-fitted or
global-sourced, how big the outline may get -- comes from the product's
kicad/board_config.json. Each repository's kicad/JLCPCB_DFM.md is the prose
behind both.

Run with KiCad's bundled Python so pcbnew is importable:

    <kicad-python> kicad/tools/dfm_gate.py <board.kicad_pcb> <profile>

Exit code 0 = every gate passes. Non-zero = findings printed, one per line.
"""
import os
import sys
import math

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config
from jlcpcb import (MIN_VIA_DRILL, MIN_VIA_DIA, VIA_TO_MASK_OPENING, MIN_NPTH,
                    FIDUCIAL_EDGE, EDGE_WARN, BANNED_PACKAGES)

MM = pcbnew.ToMM



def pad_mask_rect(pad, mask_margin):
    """Axis-aligned bbox of the pad's soldermask opening, in mm."""
    bb = pad.GetBoundingBox()
    m = pad.GetLocalSolderMaskMargin()
    extra = MM(m.value()) if hasattr(m, "value") and m.has_value() else mask_margin
    return (MM(bb.GetX()) - extra, MM(bb.GetY()) - extra,
            MM(bb.GetX() + bb.GetWidth()) + extra, MM(bb.GetY() + bb.GetHeight()) + extra)


def rect_point_dist(rect, x, y):
    x0, y0, x1, y1 = rect
    dx = max(x0 - x, 0.0, x - x1)
    dy = max(y0 - y, 0.0, y - y1)
    return math.hypot(dx, dy)


def main():
    board_path, profile_name = sys.argv[1], sys.argv[2]
    profile = board_config.load().board(profile_name)
    b = board_config.load_board(pcbnew, board_path)
    findings, warnings = [], []

    bds = b.GetDesignSettings()
    mask_margin = MM(bds.m_SolderMaskExpansion) if hasattr(bds, "m_SolderMaskExpansion") else 0.05

    bb = b.GetBoardEdgesBoundingBox()
    ex0, ey0 = MM(bb.GetX()), MM(bb.GetY())
    ex1, ey1 = ex0 + MM(bb.GetWidth()), ey0 + MM(bb.GetHeight())

    # --- Stackup and outline: the two things that set the board price ---------
    n_cu = b.GetCopperLayerCount()
    if n_cu != profile["layers"]:
        findings.append("copper layer count %d, expected %d" % (n_cu, profile["layers"]))

    # GetBoardEdgesBoundingBox() includes the Edge.Cuts *stroke*, half a line width proud on each
    # side. The fab cuts the centreline, so take the widest edge stroke back off before comparing
    # against the price break — otherwise a true 100.00mm board reports as 100.15 and fails.
    edge_w = max([MM(d.GetWidth()) for d in b.GetDrawings()
                  if d.GetLayer() == pcbnew.Edge_Cuts] or [0.0])
    w, h = ex1 - ex0 - edge_w, ey1 - ey0 - edge_w
    # A board with no outline yet has no size to check against, and inventing one
    # would read exactly like knowing it. Say so instead.
    if profile.has("max_size"):
        mw, mh = profile["max_size"]
        if w > mw + 1e-3 or h > mh + 1e-3:
            findings.append("outline %.2f x %.2f mm exceeds %.0f x %.0f — crosses the price break"
                            % (w, h, mw, mh))
    else:
        warnings.append("no max_size in board_config.json — price-break check skipped")

    # --- Vias: size, type, distance to mask openings -------------------------
    smd_pads = []
    for fp in b.GetFootprints():
        for pad in fp.Pads():
            if pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD:
                side = "F" if pad.IsOnLayer(pcbnew.F_Cu) else "B"
                smd_pads.append((pad_mask_rect(pad, mask_margin), side,
                                 fp.GetReference(), pad.GetNumber(), pad.GetNetCode()))

    vias = [t for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
    for v in vias:
        p = v.GetPosition()
        vx, vy = MM(p.x), MM(p.y)
        drill = MM(v.GetDrillValue())
        dia = MM(v.GetWidth(v.GetLayer()))
        if v.GetViaType() != pcbnew.VIATYPE_THROUGH:
            findings.append("via @(%.2f,%.2f): non-through type %s" % (vx, vy, v.GetViaType()))
        if drill < MIN_VIA_DRILL - 1e-6:
            findings.append("via @(%.2f,%.2f): drill %.2f < %.2f" % (vx, vy, drill, MIN_VIA_DRILL))
        if dia < MIN_VIA_DIA - 1e-6:
            findings.append("via @(%.2f,%.2f): diameter %.2f < %.2f" % (vx, vy, dia, MIN_VIA_DIA))
        vr = dia / 2.0
        hr = drill / 2.0
        for rect, side, ref, num, nc in smd_pads:
            hole_gap = rect_point_dist(rect, vx, vy) - hr
            barrel_gap = rect_point_dist(rect, vx, vy) - vr
            if hole_gap < 1e-6:
                # via-in-pad: triggers the paid epoxy-fill process
                findings.append("via-in-pad @(%.2f,%.2f) net %s: hole inside mask opening %s.%s"
                                % (vx, vy, v.GetNetname(), ref, num))
            elif barrel_gap < VIA_TO_MASK_OPENING - 1e-6:
                # untentable: JLCPCB leaves the via open (no cost; minor wicking risk)
                warnings.append("open via @(%.2f,%.2f) net %s: %.2fmm from mask opening %s.%s (stays untented)"
                                % (vx, vy, v.GetNetname(), max(barrel_gap, 0), ref, num))

    # --- Footprints: side, packages, fiducials, edges, NPTH ------------------
    smd_side_bad, fid_bad, npth_bad, pkg_bad = [], [], [], []
    for fp in b.GetFootprints():
        ref = fp.GetReference()
        name = fp.GetFPID().GetLibItemName().wx_str()
        pads = list(fp.Pads())
        has_smd = any(p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD for p in pads)
        has_pth = any(p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for p in pads)

        for p in pads:
            if p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                d = MM(p.GetDrillSize().x)
                if d < MIN_NPTH - 1e-6:
                    npth_bad.append("%s: NPTH drill %.2f < %.2f" % (ref, d, MIN_NPTH))

        if ref.startswith("FID"):
            pos = fp.GetPosition()
            px, py = MM(pos.x), MM(pos.y)
            margin = min(px - ex0, ex1 - px, py - ey0, ey1 - py)
            if margin < FIDUCIAL_EDGE - 1e-6:
                fid_bad.append("%s: %.2fmm from edge (need >=%.2f)" % (ref, margin, FIDUCIAL_EDGE))
            continue
        if ref.startswith("H") and not has_smd:
            continue

        if any(tag in name for tag in BANNED_PACKAGES) and has_smd:
            pkg_bad.append("%s: %s" % (ref, name))

        machine_placed = has_smd and ref not in profile["hand_solder"] and (
            not has_pth or ref in profile["mixed_smd"])
        if machine_placed:
            side = "B" if fp.IsFlipped() else "F"
            if side != profile["smd_side"]:
                smd_side_bad.append("%s (%s) on %s, expected %s" % (ref, fp.GetValue(), side, profile["smd_side"]))

        # edge proximity (warning only — conveyor clearance is not billed on Economic)
        for cs in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            c = fp.GetCourtyard(cs)
            if not c.OutlineCount():
                continue
            cb = c.BBox()
            m = min(MM(cb.GetX()) - ex0, ex1 - MM(cb.GetX() + cb.GetWidth()),
                    MM(cb.GetY()) - ey0, ey1 - MM(cb.GetY() + cb.GetHeight()))
            if m < EDGE_WARN:
                warnings.append("%s courtyard %.2fmm from board edge" % (ref, m))
            break

    findings += smd_side_bad + fid_bad + npth_bad + pkg_bad

    # --- Report ---------------------------------------------------------------
    print("board: %s  profile: %s" % (board_path, profile_name))
    print("vias: %d | SMD pads: %d | mask margin: %.3f" % (len(vias), len(smd_pads), mask_margin))
    for w in sorted(set(warnings)):
        print("WARN  " + w)
    if findings:
        for f in findings:
            print("FAIL  " + f)
        print("DFM GATE: %d finding(s)" % len(findings))
        sys.exit(1)
    print("DFM GATE: PASS")


if __name__ == "__main__":
    main()
