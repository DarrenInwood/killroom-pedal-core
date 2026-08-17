# -*- coding: utf-8 -*-
"""Shortest rework path from each stranded pour island to live copper.

[check_stranded_pours.py](check_stranded_pours.py) names the pads that sit on
isolated copper. This answers the next question, the one asked with a soldering
iron in hand: what is the cheapest way to close each one on a board already
assembled?

Two routes exist for every finding, and which is shorter is not obvious:

- **Bridge the pour clearance.** A stranded island is separated from live copper
  by the clearance gap, not by open board, so scraping the mask off both sides
  and bridging is often under a millimetre of work. The bridge point is the
  closest point of the *island* to live copper, which is not always beside the
  pad -- the whole island is one piece of copper, so any point on it serves.
- **Run a wire to a live pad.** Longer, but it lands on solderable metal and
  needs no scraping. Measured pad centre to pad centre, as a flying lead spans.

Both are reported per finding, so the shorter one can be picked per pad.

Liveness here is the gate's own model: a pour or a pad is live when it is in the
net's anchor component, the one the connector pins land on. Run it under KiCad's
bundled Python so pcbnew is importable:

    "C:/Program Files/KiCad/10.0/bin/python.exe" island_bridge_gaps.py <board.kicad_pcb>...

This reports; it does not gate. check_stranded_pours.py is the gate, and its
finding count is printed here too so the two can be compared.
"""
import os
import sys

import pcbnew

# The sibling lives beside this file, which is not necessarily on sys.path:
# a product runs this through a shim in its own kicad/tools, and that is the
# directory Python would otherwise search.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_stranded_pours as gate

MM = pcbnew.ToMM


def nearest_on_outline(outline, pt):
    """Closest point on an outline's edges to pt, and the distance to it."""
    best, best_d = None, None
    n = outline.PointCount()
    for i in range(n):
        a = outline.CPoint(i)
        b = outline.CPoint((i + 1) % n)
        dx, dy = b.x - a.x, b.y - a.y
        span = float(dx * dx + dy * dy)
        t = 0.0 if span == 0 else ((pt[0] - a.x) * dx + (pt[1] - a.y) * dy) / span
        t = max(0.0, min(1.0, t))
        px, py = a.x + t * dx, a.y + t * dy
        d = ((px - pt[0]) ** 2 + (py - pt[1]) ** 2) ** 0.5
        if best_d is None or d < best_d:
            best, best_d = (px, py), d
    return best, best_d


def outline_gap(a, b):
    """(distance, point on a, point on b) for the closest approach of two outlines.

    Sampling each outline's vertices against the other's edges catches the
    closest approach whichever side its foot lies on.
    """
    best = None
    for outline, other, flipped in ((a, b, False), (b, a, True)):
        for i in range(outline.PointCount()):
            p = outline.CPoint(i)
            q, d = nearest_on_outline(other, (p.x, p.y))
            if best is None or d < best[0]:
                best = (d, q, (p.x, p.y)) if flipped else (d, (p.x, p.y), q)
    return best


def nearest_live_pour(island, live_islands):
    """Shortest clearance gap to a live pour on the same net and layer."""
    best = None
    for other in live_islands:
        if other["net"] != island["net"] or other["lid"] != island["lid"]:
            continue
        d, here, there = outline_gap(island["outline"], other["outline"])
        if best is None or d < best[0]:
            best = (d, here, there, other["area"])
    return best


def nearest_live_pad(pad, live_pads):
    """Shortest flying lead to a live pad on the same net, pad centre to centre."""
    best = None
    for other in live_pads:
        if other["net"] != pad["net"] or other["node"] == pad["node"]:
            continue
        dx = other["pos"].x - pad["pos"].x
        dy = other["pos"].y - pad["pos"].y
        d = (dx * dx + dy * dy) ** 0.5
        if best is None or d < best[0]:
            best = (d, other)
    return best


def report(board_path):
    board = pcbnew.LoadBoard(board_path)
    islands = gate.collect_islands(board)
    idx = gate.index_islands(islands)
    dsu, items = gate.build_graph(board, islands, idx)
    anchor = gate.anchors(dsu, islands, items)

    def live(node, net):
        return dsu.find(node) == anchor.get(net)

    live_islands = [i for i in islands if live(("isl", i["id"]), i["net"])]
    pads = [r for r in items if r["kind"] == "pad"]
    live_pads = [p for p in pads if live(p["node"], p["net"])]

    # The same selection check() makes: off the net's anchor component, and with
    # no copper of its own, so the pour is the only thing feeding it.
    stranded = [p for p in pads if not live(p["node"], p["net"]) and not p["landed"]]

    print("board: %s" % board_path)
    print("%-11s %-6s %-8s %-9s %-19s %-19s %-9s %-13s %s"
          % ("pad", "layer", "island", "bridge", "from", "to", "onto", "wire to pad", "lead"))

    rows = 0
    for pad in sorted(stranded, key=lambda p: (p["ref"], p["name"])):
        tag = "%s.%s" % (pad["ref"], pad["name"])
        if not pad["serving"]:
            print("%-11s no %s pour within %.1fmm on any of its layers"
                  % (tag, pad["net"], gate.SERVING_RANGE_MM))
            rows += 1
            continue
        # A pad can straddle two stranded islands; bridge the largest, which is
        # the one most likely to recover neighbouring pads at the same time.
        island = max(pad["serving"], key=lambda i: i["area"])
        if len(pad["serving"]) > 1:
            tag += "*"

        pour = nearest_live_pour(island, live_islands)
        wire = nearest_live_pad(pad, live_pads)
        bridge = ("%-8.2f %-19s %-19s %-9s"
                  % (MM(pour[0]),
                     "(%.2f, %.2f)" % (MM(pour[1][0]), MM(pour[1][1])),
                     "(%.2f, %.2f)" % (MM(pour[2][0]), MM(pour[2][1])),
                     "%.1fmm2" % pour[3])
                  ) if pour else "%-8s %-19s %-19s %-9s" % ("-", "no live pour", "", "")
        lead = ("%-13s %.1fmm" % ("%s.%s" % (wire[1]["ref"], wire[1]["name"]), MM(wire[0]))
                ) if wire else "%-13s -" % "no live pad"
        print("%-11s %-6s %-9.2f %s %s"
              % (tag, island["layer"], island["area"], bridge, lead))
        rows += 1

    print("%d stranded pad(s); * = pad straddles more than one stranded island" % rows)
    print("gate cross-check: check_stranded_pours.py should report the same count")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        report(path)
        print("")


if __name__ == "__main__":
    main()
