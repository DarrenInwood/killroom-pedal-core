# -*- coding: utf-8 -*-
"""Refit the ground pours to the board outline.

    python fit_zones.py <board.kicad_pcb> [inset_mm]

A ZONE THAT DOES NOT MATCH THE OUTLINE HANGS FREEROUTING. Not "fills badly" -- hangs, with exactly the
same silent signature as a DSN full of pre-laid traces: it prints nothing, not even its "Opening"
line, and sits there burning CPU. Growing the control board from 96x60 to 96x65 left its four GND
zones at their old extent, and freerouting sat on a DSN with zero pre-laid wires and zero keepouts for
twenty minutes. Refit the zones and the same board routes in under two minutes.

The second, quieter cost is worse: the strip of board the zones no longer reach has **no ground on any
layer**, inner planes included. On the control board that was the front 6mm -- where the two mating
sockets, both LEDs and both footswitch leads live.

So this runs whenever the outline moves, before anything is routed. It is idempotent.
"""
import sys

import pcbnew

MM = pcbnew.ToMM
FM = pcbnew.FromMM


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: fit_zones.py <board.kicad_pcb> [inset_mm]")
    path = sys.argv[1]
    inset = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0

    b = pcbnew.LoadBoard(path)
    e = b.GetBoardEdgesBoundingBox()

    # GetBoardEdgesBoundingBox() includes the Edge.Cuts STROKE, half a line width proud on each side.
    # The fab cuts the centreline, so take the stroke back off before insetting.
    w = max([MM(d.GetWidth()) for d in b.GetDrawings()
             if d.GetLayer() == pcbnew.Edge_Cuts] or [0.0])
    x0 = MM(e.GetX()) + w / 2 + inset
    y0 = MM(e.GetY()) + w / 2 + inset
    x1 = MM(e.GetX() + e.GetWidth()) - w / 2 - inset
    y1 = MM(e.GetY() + e.GetHeight()) - w / 2 - inset

    moved = 0
    for z in b.Zones():
        bb = z.Outline().BBox()
        cur = (MM(bb.GetLeft()), MM(bb.GetTop()), MM(bb.GetRight()), MM(bb.GetBottom()))
        want = (x0, y0, x1, y1)
        if all(abs(a - c) < 0.05 for a, c in zip(cur, want)):
            continue
        o = z.Outline()
        o.RemoveAllContours()
        o.NewOutline()
        for px, py in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            o.Append(FM(px), FM(py))
        z.HatchBorder()
        moved += 1
        lay = b.GetLayerName(z.GetLayerSet().Seq()[0])
        print(f"  {z.GetNetname()} on {lay}: "
              f"({cur[0]:.1f},{cur[1]:.1f})..({cur[2]:.1f},{cur[3]:.1f}) -> "
              f"({x0:.1f},{y0:.1f})..({x1:.1f},{y1:.1f})")

    if not moved:
        print(f"zones already fit the outline ({inset:.1f}mm inset)")
        return 0

    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(path, b)
    print(f"refitted {moved} zone(s) to the board outline and saved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
