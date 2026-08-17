# -*- coding: utf-8 -*-
"""Give every SMD ground pad its own via BEFORE the board is routed.

    "C:/Program Files/KiCad/10.0/bin/python.exe" fanout_gnd.py <board.kicad_pcb>

A ground pour is not a ground connection. Routing carves the outer pours into islands, and any
island holding a GND pad but no via is an open circuit that looks perfect on the bench -- which is
the defect that scrapped prototype 1 and put 6.8V on a 3.3V rail.

Doing it AFTERWARDS does not work. Once the router has filled the board there is nowhere left to
put the via: on the control board 19 of 21 stranded pads came back "no via site within 5.2mm that is
clear ... and can be reached without crossing another net". The anchor copper was there -- 26 of 36
probe points at 1mm landed in live GND -- but the space for a 0.6mm via and a track to it was gone.
So the via has to exist first, and the router has to route around it.

This is NOT the 6mm stitching GRID that stranded 11 LQFP pins by parking vias in the escape. Each
via here belongs to one pad, sits as close to it as it will fit, and goes where that pad's own
return current needs it regardless.

Run on bare copper, after the clocks are laid and before the DSN is exported. prep_dsn re-injects
every pre-laid feature as a keepout, so freerouting sees these and goes around.
"""
import math
import sys

import pcbnew

MM = pcbnew.ToMM
FM = pcbnew.FromMM

VIA_D, VIA_H = 0.6, 0.3
TRACK = 0.3
CLEAR = 0.25
STEP = 0.1
MAX_R = 3.0
ANGLES = 32


def main():
    path = sys.argv[1]
    # --only REF.PAD,REF.PAD  restricts the fanout to named pads.
    #
    # Fanning out EVERY SMD ground pad works but costs too much: 67 vias on the control board became
    # 335 keepouts, and signal unrouted went from 1 to 22 while the router spent 31 minutes. Ground
    # was fixed and the board was ruined. The pads that actually strand are a fraction of the total
    # and they are known -- the gate names them after a route -- so fan out those and leave the rest
    # to the pour they are already connected by.
    only = None
    for a in sys.argv[2:]:
        if a.startswith("--only="):
            only = {s.strip() for s in a[len("--only="):].split(",") if s.strip()}
    b = pcbnew.LoadBoard(path)
    gnd = b.FindNet("GND")
    if gnd is None:
        sys.exit("no GND net on this board")

    edge = b.GetBoardEdgesBoundingBox()
    x0, y0 = MM(edge.GetLeft()) + 1.0, MM(edge.GetTop()) + 1.0
    x1, y1 = MM(edge.GetRight()) - 1.0, MM(edge.GetBottom()) - 1.0

    # every pad is an obstacle for a via, its own net included: a hole inside a mask opening is the
    # paid via-in-pad process at JLCPCB and the one hard rule in the DFM profile.
    pads = []
    for fp in b.GetFootprints():
        for p in fp.Pads():
            bb = p.GetBoundingBox()
            m = VIA_D / 2.0 + CLEAR
            pads.append((MM(bb.GetLeft()) - m, MM(bb.GetTop()) - m,
                         MM(bb.GetRight()) + m, MM(bb.GetBottom()) + m))

    # anything already laid (the hand-routed clocks) is an obstacle too
    laid = []
    for t in b.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            q = t.GetPosition()
            laid.append((MM(q.x), MM(q.y), MM(t.GetWidth(t.GetLayer())) / 2.0 + VIA_D / 2.0 + CLEAR))
        else:
            a, c = t.GetStart(), t.GetEnd()
            n = max(1, int(math.hypot(MM(c.x - a.x), MM(c.y - a.y)) / 0.4))
            r = MM(t.GetWidth()) / 2.0 + VIA_D / 2.0 + CLEAR
            for i in range(n + 1):
                laid.append((MM(a.x) + MM(c.x - a.x) * i / n,
                             MM(a.y) + MM(c.y - a.y) * i / n, r))

    placed = []

    def free(vx, vy):
        if not (x0 <= vx <= x1 and y0 <= vy <= y1):
            return False
        for a, bb_, c, d in pads:
            if a <= vx <= c and bb_ <= vy <= d:
                return False
        for ox, oy, r in laid + placed:
            if (vx - ox) ** 2 + (vy - oy) ** 2 < r * r:
                return False
        return True

    targets = []
    for fp in b.GetFootprints():
        for p in fp.Pads():
            if p.GetNetCode() != gnd.GetNetCode():
                continue
            if p.GetAttribute() != pcbnew.PAD_ATTRIB_SMD:
                continue        # a through-hole pad already punches every layer
            if only is not None and f"{fp.GetReference()}.{p.GetPadName()}" not in only:
                continue
            targets.append((fp, p))

    done, stuck = 0, []
    for fp, p in targets:
        q = p.GetPosition()
        px, py = MM(q.x), MM(q.y)
        bb = p.GetBoundingBox()
        start = math.hypot(MM(bb.GetWidth()), MM(bb.GetHeight())) / 2.0 + VIA_D / 2.0 + CLEAR
        spot = None
        r = start
        while r <= start + MAX_R and spot is None:
            for k in range(ANGLES):
                a = k * 2.0 * math.pi / ANGLES
                vx, vy = px + math.cos(a) * r, py + math.sin(a) * r
                if free(vx, vy):
                    spot = (vx, vy)
                    break
            r += STEP
        if spot is None:
            stuck.append(f"{fp.GetReference()}.{p.GetPadName()}")
            continue

        v = pcbnew.PCB_VIA(b)
        v.SetPosition(pcbnew.VECTOR2I(FM(spot[0]), FM(spot[1])))
        v.SetWidth(FM(VIA_D))
        v.SetDrill(FM(VIA_H))
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(gnd)
        b.Add(v)

        t = pcbnew.PCB_TRACK(b)
        t.SetStart(pcbnew.VECTOR2I(FM(px), FM(py)))
        t.SetEnd(pcbnew.VECTOR2I(FM(spot[0]), FM(spot[1])))
        t.SetWidth(FM(TRACK))
        t.SetLayer(p.GetLayer())
        t.SetNet(gnd)
        b.Add(t)

        placed.append((spot[0], spot[1], VIA_D + CLEAR))
        done += 1

    pcbnew.SaveBoard(path, b)
    print(f"GND fanout: {done} via(s) placed for {len(targets)} SMD ground pad(s)")
    if stuck:
        print(f"  {len(stuck)} pad(s) had no room within {MAX_R}mm: {' '.join(stuck[:12])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
