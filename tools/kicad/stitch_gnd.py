# -*- coding: utf-8 -*-
"""Stitch the ground plane -- AFTER routing, never before.

THE ORDER IS THE WHOLE POINT. Pre-placing stitching vias is the single biggest self-inflicted wound
of the last attempt: 144 locked GND/+3V3 vias, dropped before the router ran, sat in the LQFP's escape
region and ate the space it needed. With them the router stranded 11 MCU pins; without them, 7. The
board that routed to zero had no pre-stitching at all -- the router placed all 606 of its own vias,
coherently, and the stitching went in afterwards, around what was already there.

So this runs LAST. It does two things, in this order:

  1. RESCUE. A GND pad is not stranded because it has no copper on it -- it is stranded because the
     copper island it sits in never reaches a plane layer. Find the GND pads whose local pour island
     has no via to the inner planes, and give each one a via right beside it.

  2. STITCH. Sprinkle vias on a grid through the remaining GND copper, tying the outer pours to the
     inner planes so the return current under a clock has somewhere to go at every point along it.

Vias never land in a pad's mask opening: a hole inside one is a paid epoxy-fill process at JLCPCB,
and it is the only HARD via rule in the DFM profile.

"ISLAND" MEANS ONE FILLED POLYGON, AND EVERY TEST HERE HAS TO SAY SO. GetFilledPolysList(layer)
returns ALL the islands on that layer as one SHAPE_POLY_SET, so `polys.Collide(pt)` answers "is this
point in ANY island on this layer" -- not "in THIS one". Asking it the loose question while walking
island k lets a via sitting in a neighbouring island satisfy k's "already stitched" test, and lets
the scan seat k's via in a different island entirely. Both tests therefore rebuild a one-outline
SHAPE_POLY_SET for the island under test, which is the same definition check_stranded_pours.py uses.

    python stitch_gnd.py <board.kicad_pcb> [pitch_mm]

Exit code 0 = no pad is left on stranded copper. Non-zero = the pads it could not reach are printed,
for rescue_gnd.py to finish with a via AND a track.

The verdict comes from check_stranded_pours.check() rather than from a second opinion held here.
An island with no via is usually harmless -- most are pockets between traces and between connector
pins, carrying nothing -- and "an island within a millimetre of some pad" is not the test either,
because nearly every island in a THT pin field is. One definition of stranded, in one place, or the
two drift apart and the looser one wins by accident.
"""
import math
import os
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_stranded_pours import (check, collect_islands,   # noqa: E402
                                  SERVING_RANGE_MM)         # same island definition

MM = pcbnew.ToMM
FM = pcbnew.FromMM

VIA_D, VIA_H = 0.6, 0.3
CLEAR = 0.25          # to any other copper
PITCH = 6.0           # mm, stitching grid
EDGE = 1.0            # mm from the board outline


def keepouts(b, gnd_code):
    """Everything a stitching via must stay off: every pad, and every track that is not GND."""
    rects, discs = [], []
    m = VIA_D / 2.0 + CLEAR
    for fp in b.GetFootprints():
        for pad in fp.Pads():
            bb = pad.GetBoundingBox()
            # ANY pad, GND or not: a via inside a mask opening is the paid process
            rects.append((MM(bb.GetLeft()) - m, MM(bb.GetTop()) - m,
                          MM(bb.GetRight()) + m, MM(bb.GetBottom()) + m))
    for t in b.GetTracks():
        if t.GetNetCode() == gnd_code:
            continue
        if t.Type() == pcbnew.PCB_VIA_T:
            q = t.GetPosition()
            discs.append((MM(q.x), MM(q.y), MM(t.GetWidth(t.GetLayer())) / 2.0 + m))
        else:
            a, c = t.GetStart(), t.GetEnd()
            ax, ay, cx, cy = MM(a.x), MM(a.y), MM(c.x), MM(c.y)
            r = MM(t.GetWidth()) / 2.0 + m
            n = max(1, int(math.hypot(cx - ax, cy - ay) / 0.5))
            for i in range(n + 1):
                discs.append((ax + (cx - ax) * i / n, ay + (cy - ay) * i / n, r))
    return rects, discs


def free(x, y, rects, discs, vias):
    for x0, y0, x1, y1 in rects:
        if x0 <= x <= x1 and y0 <= y <= y1:
            return False
    for ox, oy, r in discs:
        if (x - ox) ** 2 + (y - oy) ** 2 < r * r:
            return False
    for vx, vy in vias:
        if (x - vx) ** 2 + (y - vy) ** 2 < (VIA_D + CLEAR) ** 2:
            return False
    return True


# Which layers a via must reach to be worth placing. A stitching via earns its
# place by joining an OUTER layer to a plane -- so the test is "an outer hit, and
# a hit on one of the layers this board reserves as plane".
#
# The reserved list is the board's, and on a 2-layer board it is EMPTY: there is
# no inner layer to reach. Requiring an inner hit unconditionally -- which this
# did -- makes the test false at every point on such a board, so the stitcher
# places nothing and prints a summary that reads like success. Falling back to
# "the opposite outer layer" is what makes F-to-B stitching mean what it says.
REACH_LAYERS = ()     # set by main() from the board's config

_LAYER_IDS = {"F.Cu": pcbnew.F_Cu, "B.Cu": pcbnew.B_Cu,
              "In1.Cu": pcbnew.In1_Cu, "In2.Cu": pcbnew.In2_Cu,
              "In3.Cu": pcbnew.In3_Cu, "In4.Cu": pcbnew.In4_Cu}


def in_gnd_copper(b, zones, x, y):
    """Is (x, y) a point where a stitching via would join copper at both ends?

    An outer layer, plus one of REACH_LAYERS -- or, when the board reserves none,
    the opposite outer layer.
    """
    pt = pcbnew.VECTOR2I(FM(x), FM(y))
    probe = (pcbnew.F_Cu, pcbnew.B_Cu) + tuple(REACH_LAYERS)
    hits = set()
    for z in zones:
        for layer in probe:
            if not z.IsOnLayer(layer):
                continue
            poly = z.GetFilledPolysList(layer)
            if poly and poly.Collide(pt):
                hits.add(layer)
    if REACH_LAYERS:
        return ((pcbnew.F_Cu in hits or pcbnew.B_Cu in hits)
                and any(l in hits for l in REACH_LAYERS))
    return pcbnew.F_Cu in hits and pcbnew.B_Cu in hits


def gnd_islands(b, gc):
    """Every filled polygon of the GND net, on every copper layer."""
    net = b.GetNetsByNetcode()[gc].GetNetname()
    return [i for i in collect_islands(b) if i["net"] == net]


def has_via(isl, gnd_vias):
    """Is one of these vias inside THIS island? Bbox first, then the polygon."""
    x0, y0, x1, y1 = isl["bbox"]
    for vx, vy in gnd_vias:
        nx, ny = FM(vx), FM(vy)
        if nx < x0 or nx > x1 or ny < y0 or ny > y1:
            continue
        if isl["poly"].Collide(pcbnew.VECTOR2I(nx, ny)):
            return True
    return False


def seat_via_in(isl, rects, discs, existing, bounds):
    """Find a legal via site inside this island. -> (x, y) or None.

    Scans the island on a 0.4mm grid. Sampling a few points across the bounding box
    finds nothing on an island shaped like an L or a crescent -- which is the shape a
    router leaves behind -- and the island silently keeps its pads and no route out.
    """
    bx0, by0, bx1, by1 = (MM(v) for v in isl["bbox"])
    x0, y0, x1, y1 = bounds
    gy = by0
    while gy <= by1:
        gx = bx0
        while gx <= bx1:
            if (x0 <= gx <= x1 and y0 <= gy <= y1
                    and isl["poly"].Collide(pcbnew.VECTOR2I(FM(gx), FM(gy)))
                    and free(gx, gy, rects, discs, existing)):
                return gx, gy
            gx += 0.4
        gy += 0.4
    return None


def add_via(b, net, x, y):
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(pcbnew.VECTOR2I(FM(x), FM(y)))
    v.SetWidth(FM(VIA_D))
    v.SetDrill(FM(VIA_H))
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNet(net)
    b.Add(v)


def main():
    global REACH_LAYERS
    path = sys.argv[1]
    try:
        _b = board_config.load().board_for_path(path)
        REACH_LAYERS = tuple(_LAYER_IDS[n] for n in (_b.get("stitch_reach_layers") or ())
                             if n in _LAYER_IDS)
    except Exception:
        REACH_LAYERS = ()
    pitch = float(sys.argv[2]) if len(sys.argv) > 2 else PITCH
    b = pcbnew.LoadBoard(path)

    gnd = b.FindNet("GND")
    if gnd is None:
        sys.exit("no GND net")
    gc = gnd.GetNetCode()

    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    zones = [z for z in b.Zones() if z.GetNetCode() == gc]

    rects, discs = keepouts(b, gc)
    # `existing` is every via on the board -- used for SPACING, so a new one is not dropped on top of
    # somebody else's. `gnd_vias` is only the GND ones, and it is what says whether an island is
    # actually connected: a signal via passing through a GND island connects it to precisely nothing,
    # and counting it left 25 islands stranded while the stitcher reported itself converged.
    existing = [(MM(t.GetPosition().x), MM(t.GetPosition().y))
                for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
    gnd_vias = [(MM(t.GetPosition().x), MM(t.GetPosition().y))
                for t in b.GetTracks()
                if t.Type() == pcbnew.PCB_VIA_T and t.GetNetCode() == gc]

    e = b.GetBoardEdgesBoundingBox()
    x0, y0 = MM(e.GetX()) + EDGE, MM(e.GetY()) + EDGE
    x1, y1 = x0 + MM(e.GetWidth()) - 2 * EDGE, y0 + MM(e.GetHeight()) - 2 * EDGE

    bounds = (x0, y0, x1, y1)
    islands = gnd_islands(b, gc)

    # --- 1. rescue: a GND pad whose island never reaches a plane -----------------------------------
    rescued = 0
    for fp in b.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetCode() != gc:
                continue
            p = pad.GetPosition()
            px, py = MM(p.x), MM(p.y)
            # "Is there already a GND via near this pad" is the WRONG question, and it was asked as
            # a 3mm euclidean radius. A via 2mm away on the far side of a trace the router laid is
            # in a different island and connects this pad to nothing. Ask whether a via sits in an
            # island that actually FEEDS this pad.
            reach_nm = FM(SERVING_RANGE_MM)
            serving = [i for i in islands
                       if i["lid"] in list(pad.GetLayerSet().CuStack())
                       and i["outline"].Distance(p) <= reach_nm]
            if serving and any(has_via(i, gnd_vias) for i in serving):
                continue
            bb = pad.GetBoundingBox()
            reach = math.hypot(MM(bb.GetWidth()), MM(bb.GetHeight())) / 2.0 + VIA_D / 2.0 + CLEAR
            for extra in (0.1, 0.4, 0.8, 1.4, 2.0):
                done = False
                for k in range(16):
                    a = k * math.pi / 8.0
                    vx = px + math.cos(a) * (reach + extra)
                    vy = py + math.sin(a) * (reach + extra)
                    if not (x0 <= vx <= x1 and y0 <= vy <= y1):
                        continue
                    if free(vx, vy, rects, discs, existing) and in_gnd_copper(b, zones, vx, vy):
                        add_via(b, gnd, vx, vy)
                        existing.append((vx, vy))
                        gnd_vias.append((vx, vy))
                        rescued += 1
                        done = True
                        break
                if done:
                    break
    print(f"rescue vias for GND pads with no route to a plane: {rescued}")

    # --- 1b. rescue the ISLANDS -----------------------------------------------------------------
    # The pour is not one sheet. The router's traces fence it into islands, and an island with no via
    # in it is a piece of copper connected to nothing -- DRC reports it as GND-not-connected-to-GND.
    # A stitching grid does not find these: a 5mm grid steps straight over a 3mm island. Walk the
    # filled polygons instead and make sure every one of them has a via.
    # Every copper layer, not just the outer two. On the main board In2 is FREED FOR ROUTING and
    # carries a GND fill that the router carves up exactly like F.Cu; on the control board it is In1.
    # Walking only (F_Cu, B_Cu) left both inner layers unguarded.
    seated = 0
    for isl in islands:
        if has_via(isl, gnd_vias):
            continue
        spot = seat_via_in(isl, rects, discs, existing, bounds)
        if spot is None:
            continue
        add_via(b, gnd, spot[0], spot[1])
        existing.append(spot)
        gnd_vias.append(spot)
        seated += 1
    print(f"vias into GND islands that had none (the router fenced them off): {seated}")

    # --- 2. stitch the field ------------------------------------------------------------------------
    stitched = 0
    y = y0
    while y <= y1:
        x = x0
        while x <= x1:
            if free(x, y, rects, discs, existing) and in_gnd_copper(b, zones, x, y):
                add_via(b, gnd, x, y)
                existing.append((x, y))
                gnd_vias.append((x, y))
                stitched += 1
            x += pitch
        y += pitch
    print(f"stitching vias on a {pitch:.1f}mm grid: {stitched}")

    pcbnew.ZONE_FILLER(b).Fill(b.Zones())

    # Verify against the REFILLED pour, not the one we planned against. Adding vias moves the fill
    # -- each one carries its own clearance -- so islands merge, split and appear. Reporting the
    # pre-fill state would be reporting an intention.
    print(f"total vias now on the board: "
          f"{sum(1 for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T)}")
    pcbnew.SaveBoard(path, b)

    # An island with no via is only a problem if a pad actually depends on it. Most of them are
    # pockets the router left between traces and between connector pins; they carry nothing, and
    # failing on those would fail on every board forever. Proximity is not the test either -- almost
    # every island in a THT pin field sits within a millimetre of some ground pin whose pad is
    # perfectly healthy. So ask the gate, which is the one definition of stranded, rather than
    # keeping a second, looser one here that will drift.
    unstitched = sum(1 for i in gnd_islands(b, gc) if not has_via(i, gnd_vias))
    print(f"GND islands with no via (most are pad-less pockets): {unstitched}")

    findings = check(path, verbose=False)          # re-reads from disk, post-save
    if findings:
        print(f"STITCH: {len(findings)} pad(s) still stranded -- their island is too small or too "
              f"boxed-in for a via. rescue_gnd.py lays a via nearby AND a track to it, which is the "
              f"only thing that reaches a 0.25mm2 island.")
        for f in findings:
            print(f"    {f['ref']}.{f['pad']} [{f['net']}] @ ({f['x']:.2f}, {f['y']:.2f})")
        return 1
    print("STITCH: no pad is left on stranded copper")
    return 0


if __name__ == "__main__":
    sys.exit(main())
