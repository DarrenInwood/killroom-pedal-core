# -*- coding: utf-8 -*-
"""Give every pad left on stranded copper its own via, wired to it with a track.

Runs AFTER stitch_gnd.py, on the pads the stitcher could not reach. A pad is not
stranded because there is no copper on it. It is stranded because the copper
ISLAND it sits in never reaches the rest of its net -- the router's traces have
fenced it off from the pour. Dropping a stitching via "near" it does not help:
the via has to land inside live copper, and a grid does not know where the
islands are. Some islands cannot hold a via at all -- a 0.25 mm2 pocket has no
room for a 0.6 mm via plus its clearance -- and those are exactly the ones that
shipped open.

So this places the via in copper that is already part of the net, and runs a
short track from the pad to it. THE TRACK IS WHAT GUARANTEES THE CONNECTION,
regardless of what the pour did or did not do.

    python rescue_gnd.py <board.kicad_pcb>

Two things it previously got wrong, both of which mattered:

  * It read kicad-cli's DRC JSON and matched "Pad X [GND] of REF". This defect
    does not surface that way -- it surfaces as "Missing connection between
    items: Zone [GND] ... Zone [GND]", reported at the zone anchor. So the regex
    matched nothing and the tool was blind to the one class it exists for. It now
    asks check_stranded_pours, which is the definition of stranded.

  * It hard-coded FindNet("GND"). D201 pad 2 on the main board is a stranded
    +9V pad, so a rail pad can need this too. Each finding now carries its own net.

And two it could not have got right before:

  * The via site has to land in copper that is in the net's ANCHOR component.
    Landing in *an* island is not enough -- a via joining one stranded island to
    another has been paid for and connects nothing, which is the failure the gate
    exists to detect.

  * THE TRACK HAS TO BE ABLE TO GET THERE. Only the via site was ever checked, and
    the track was then laid to it regardless of what lay in between. Wired into the
    pipeline for the first time, that took the control board from 0 DRC errors to
    19 -- three of them outright shorts, GND across FS1 and across +3V3.

Worth knowing what the second one reveals: on the prototype-1 boards it rejects
every candidate, because the island is fenced off BY a trace and any path from the
pad out to live copper has to cross that same trace. The 20 "rescues" it reported
before were all illegal crossings. These pads cannot be fixed by copper on that
placement -- which is the argument for re-placing and re-routing rather than
patching, not an argument for a looser check.

Exit code 0 = every stranded pad now has a via and a track. Non-zero = the ones
with nowhere legal to put a via are printed; those need the placement opened up.
"""
import math
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_stranded_pours import (analyse, findings_from,     # noqa: E402
                                  in_anchor_copper)

MM = pcbnew.ToMM
FM = pcbnew.FromMM

VIA_D, VIA_H = 0.6, 0.3
TRACK = 0.3
CLEAR = 0.25
STEP = 0.2            # mm, how far out to step the search each round
ROUNDS = 20           # so the search reaches ~4mm from the pad
ANGLES = 24


def obstacles(b, own_net, margin, own_pads_block):
    """Everything a new feature must stay off, inflated by `margin`.

    Copper of `own_net` is never an obstacle -- landing on it is the point. Pads are
    a different question depending on what is being placed, hence own_pads_block:

      a VIA must clear EVERY pad including this net's own, because a hole inside a
      mask opening is the paid epoxy-fill process at JLCPCB and the only hard via
      rule in the DFM profile;

      the TRACK starts on one of this net's pads, so its own net's pads cannot be
      obstacles or nothing would ever be placeable.
    """
    rects, discs = [], []
    for fp in b.GetFootprints():
        for pad in fp.Pads():
            if not own_pads_block and pad.GetNetCode() == own_net:
                continue
            bb = pad.GetBoundingBox()
            rects.append((MM(bb.GetLeft()) - margin, MM(bb.GetTop()) - margin,
                          MM(bb.GetRight()) + margin, MM(bb.GetBottom()) + margin))
    for t in b.GetTracks():
        if t.GetNetCode() == own_net:
            continue
        if t.Type() == pcbnew.PCB_VIA_T:
            q = t.GetPosition()
            discs.append((MM(q.x), MM(q.y),
                          MM(t.GetWidth(t.GetLayer())) / 2.0 + margin))
        else:
            a, c = t.GetStart(), t.GetEnd()
            ax, ay, cx, cy = MM(a.x), MM(a.y), MM(c.x), MM(c.y)
            r = MM(t.GetWidth()) / 2.0 + margin
            n = max(1, int(math.hypot(cx - ax, cy - ay) / 0.5))
            for i in range(n + 1):
                discs.append((ax + (cx - ax) * i / n, ay + (cy - ay) * i / n, r))
    return rects, discs


def path_clear(px, py, vx, vy, rects, discs):
    """Is the straight run from the pad to the via free of other nets?

    THE TRACK IS THE PART THAT WAS NOT BEING CHECKED. Testing only the via site and
    then laying a track to it regardless is how this tool put a 3.6mm GND track
    straight across FS1 and EXT_RING on the control board, and a 4.1mm one across
    +3V3: 0 DRC errors before the rescue, 19 after, 3 of them outright shorts.
    """
    n = max(2, int(math.hypot(vx - px, vy - py) / 0.1))
    for i in range(n + 1):
        x = px + (vx - px) * i / n
        y = py + (vy - py) * i / n
        for x0, y0, x1, y1 in rects:
            if x0 <= x <= x1 and y0 <= y <= y1:
                return False
        for ox, oy, r in discs:
            if (x - ox) ** 2 + (y - oy) ** 2 < r * r:
                return False
    return True


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


def has_anchor_pour(an, net):
    """Does this net have any live pour at all?

    Not every net is poured. `+9V` on the main board is routed with tracks and has
    no zone, so "land in live pour copper" is unsatisfiable for it by construction.
    A pad of such a net that the gate flags is simply unrouted -- the router's job,
    not a pour rescue -- and reporting it as a rescue failure would make the
    pipeline fail forever on something this tool was never able to fix.
    """
    for key in an["idx"]:
        if key[0] != net:
            continue
        for isl in an["idx"][key]:
            if an["dsu"].find(("isl", isl["id"])) == an["anchor"].get(net):
                return True
    return False


def find_spot(an, net, px, py, reach, ko, vias):
    """Nearest via site that lands in live copper AND can be reached by a track.

    Three conditions, and the third is the one that was missing: the via site is clear,
    it lands in the net's anchor component, and the straight run from the pad to it does
    not cross anything else.
    """
    (v_rects, v_discs), (t_rects, t_discs) = ko
    for step in range(ROUNDS):
        for k in range(ANGLES):
            a = k * 2.0 * math.pi / ANGLES
            vx = px + math.cos(a) * (reach + step * STEP)
            vy = py + math.sin(a) * (reach + step * STEP)
            if not free(vx, vy, v_rects, v_discs, vias):
                continue
            if not in_anchor_copper(an, net, FM(vx), FM(vy)):
                continue
            if not path_clear(px, py, vx, vy, t_rects, t_discs):
                continue
            return vx, vy
    return None


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)

    # One analysis of the board we are about to modify, so the islands and the
    # anchor components are the same ones the gate judges by. Adding copper only
    # ever MERGES components, never splits them, so the model stays valid as a
    # conservative answer while we work through the list.
    an = analyse(b)
    findings, _ = findings_from(an)
    print(f"stranded pads to rescue: {len(findings)}")
    if not findings:
        print("RESCUE: nothing to do")
        return 0

    nets = {n.GetNetname(): n for n in b.GetNetsByNetcode().values()}
    ko_cache = {}
    vias = [(MM(t.GetPosition().x), MM(t.GetPosition().y))
            for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]

    done, stuck, unpoured = 0, [], []
    for f in findings:
        net = nets.get(f["net"])
        if net is None:
            stuck.append((f, "no such net on the board"))
            continue
        if not has_anchor_pour(an, f["net"]):
            unpoured.append(f)
            continue
        nc = net.GetNetCode()
        if nc not in ko_cache:
            ko_cache[nc] = (obstacles(b, nc, VIA_D / 2.0 + CLEAR, True),
                            obstacles(b, nc, TRACK / 2.0 + CLEAR, False))
        ko = ko_cache[nc]

        fp = b.FindFootprintByReference(f["ref"])
        pad = next((p for p in fp.Pads() if p.GetPadName() == f["pad"]), None)
        if pad is None:
            stuck.append((f, "pad vanished"))
            continue

        p = pad.GetPosition()
        px, py = MM(p.x), MM(p.y)
        bb = pad.GetBoundingBox()
        reach = math.hypot(MM(bb.GetWidth()), MM(bb.GetHeight())) / 2.0 + VIA_D / 2.0 + CLEAR

        spot = find_spot(an, f["net"], px, py, reach, ko, vias)
        if spot is None:
            stuck.append((f, "no via site within %.1fmm that is clear, lands in live %s "
                          "copper, and can be reached without crossing another net"
                          % (reach + ROUNDS * STEP, f["net"])))
            continue

        v = pcbnew.PCB_VIA(b)
        v.SetPosition(pcbnew.VECTOR2I(FM(spot[0]), FM(spot[1])))
        v.SetWidth(FM(VIA_D))
        v.SetDrill(FM(VIA_H))
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(net)
        b.Add(v)

        t = pcbnew.PCB_TRACK(b)
        t.SetStart(pcbnew.VECTOR2I(FM(px), FM(py)))
        t.SetEnd(pcbnew.VECTOR2I(FM(spot[0]), FM(spot[1])))
        t.SetWidth(FM(TRACK))
        t.SetLayer(pcbnew.B_Cu if fp.IsFlipped() else pcbnew.F_Cu)
        t.SetNet(net)
        b.Add(t)

        vias.append(spot)
        done += 1

    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(path, b)
    print(f"rescued {done} pad(s) with a via and a track")

    for f in unpoured:
        print(f"    -- {f['ref']}.{f['pad']} [{f['net']}] @ ({f['x']:.2f}, {f['y']:.2f}): "
              f"net has no pour, so this is an unrouted connection, not a stranded "
              f"island. The router owns it; DRC will report it.")

    if stuck:
        print(f"RESCUE: {len(stuck)} pad(s) could not be reached")
        for f, why in stuck:
            print(f"    {f['ref']}.{f['pad']} [{f['net']}] @ "
                  f"({f['x']:.2f}, {f['y']:.2f}) -- {why}")
        return 1
    print("RESCUE: every stranded pad now has a via and a track")
    return 0


if __name__ == "__main__":
    sys.exit(main())
