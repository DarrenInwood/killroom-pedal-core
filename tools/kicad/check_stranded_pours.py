# -*- coding: utf-8 -*-
"""Stranded copper-pour gate.

Finds pads whose only copper is a pour island that does not reach the rest of its
net. A zone fill is not automatically part of that net: if the router leaves a
pocket of pour with no via and no track crossing it, that pocket is isolated
copper, and any pad sitting in it is electrically open even though the joint
looks perfect.

DRC does report this, but not in a form anyone reads. The pad *is* touching
copper, so it is never listed as an unconnected pad -- the violation surfaces as
"Missing connection between items: Zone [GND] ... Zone [GND]", reported at the
zone's anchor point rather than at the island, so the coordinates point at empty
board. On prototype 1 that hid a floating ground pin under the +3V3 regulator:
U101 (HT7333) had no ground reference, ran wide open, and put 6.8 V onto the
3.3 V rail across the whole control board. See docs/FIXES-PROTOTYPE-1.md.

This gate names the pad, the part and the island instead.

WHY THIS IS A CONNECTIVITY MODEL AND NOT A CONTAINMENT TEST
-----------------------------------------------------------
Asking "does a via or track land inside this island" is necessary but not
sufficient. A via sitting inside a stranded island whose other end lands in a
*second* stranded island marks both islands live, and both are still floating.
With 148 pour-dependent pads on the main board that is not a theoretical hole --
it is most of the ground.

So the copper is modelled as a graph and solved with union-find:

    nodes   every filled polygon, every via, every track, every pad
    edges   via/track <-> the island it lands inside, per layer
            pad       <-> the island whose fill reaches it (SERVING_RANGE_MM)
            any       <-> any same-net copper whose SHAPE IT OVERLAPS

Each net then has one ANCHOR component -- the one carrying a connector pin where
there is one, else the largest by copper area -- and a pour-dependent pad outside
it is stranded no matter how much copper its own island touches.

Overlap is tested with pcbnew's own GetEffectiveShape(layer).Collide(). Cheaper
tests were tried and are wrong in ways this board demonstrates:

  * Shared endpoints. route_clocks.py lays B.Cu tracks on a 0.25 mm grid but
    puts vias at computed positions, so on BBD0_CLK a via sits at
    (74.971, 115.625) while the track feeding it starts at (75.000, 115.500).
    0.128 mm apart, solidly joined in copper, sharing no endpoint. Endpoint
    matching reported all five clock nets as fragmented.
  * Centre-inside-shape. On ROM_CS a via sits 0.66 mm from R502 pad 2's centre,
    with neither centre inside the other, joined at the pad's edge.

KiCad's own CONNECTIVITY_DATA.GetConnectedItems() would answer the non-zone part
of this correctly, but its transitive closure can run *through* zones, which is
precisely the thing being audited -- so the graph is built here instead.

Run it before any fab package, with KiCad's bundled Python so pcbnew is
importable:

    "C:/Program Files/KiCad/10.0/bin/python.exe" check_stranded_pours.py <board.kicad_pcb>...

Exit code 0 = every pad reaches its net's main body. Non-zero = findings printed,
one per line. Acceptance for a fab package is findings == 0 AND KiCad's own
unconnected count == 0; the two are computed independently and printed together
so they can be compared.
"""
import sys

import pcbnew

MM = pcbnew.ToMM

# How far a pour may sit from a pad centre and still be the pour that feeds it.
# Thermal spokes and the pad's own clearance ring put the fill edge well under a
# millimetre away; anything further is a different pour that happens to be nearby.
SERVING_RANGE_MM = 1.0

# Spatial-index cell for the overlap search. Copper features are millimetre-scale,
# so a 1 mm grid keeps each bucket to a handful of candidates.
CELL_NM = pcbnew.FromMM(1.0)


class DSU(object):
    """Union-find over hashable node keys."""

    def __init__(self):
        self.parent = {}

    def add(self, k):
        self.parent.setdefault(k, k)

    def find(self, k):
        p = self.parent
        root = k
        while p[root] != root:
            root = p[root]
        while p[k] != root:            # path compression
            p[k], k = root, p[k]
        return root

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.parent[rb] = ra


def collect_islands(board):
    """Every filled polygon on the board, with its own one-outline poly and bbox."""
    islands = []
    for zone in board.Zones():
        net = zone.GetNetname()
        for lid in zone.GetLayerSet().CuStack():
            try:
                fills = zone.GetFilledPolysList(lid)
            except Exception:
                continue
            for i in range(fills.OutlineCount()):
                outline = fills.Outline(i)
                if outline.PointCount() < 3:
                    continue
                poly = pcbnew.SHAPE_POLY_SET()
                poly.AddOutline(outline)
                bb = outline.BBox()
                islands.append({
                    "id": len(islands),
                    "net": net,
                    "lid": lid,
                    "layer": board.GetLayerName(lid),
                    "poly": poly,
                    "outline": outline,
                    "bbox": (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom()),
                    "area": abs(outline.Area()) / 1e12,
                })
    return islands


def index_islands(islands):
    """-> {(net, layer id): [island, ...]}"""
    idx = {}
    for isl in islands:
        idx.setdefault((isl["net"], isl["lid"]), []).append(isl)
    return idx


def islands_at(idx, net, lid, x, y):
    """Islands of this net/layer whose polygon contains (x, y). Bbox pre-filtered,
    because containment is the expensive test and most islands miss."""
    hits = []
    for isl in idx.get((net, lid), ()):
        x0, y0, x1, y1 = isl["bbox"]
        if x < x0 or x > x1 or y < y0 or y > y1:
            continue
        if isl["poly"].Contains(pcbnew.VECTOR2I(int(x), int(y))):
            hits.append(isl)
    return hits


def _cells(bb):
    """The grid cells a bounding box covers."""
    for cx in range(bb[0] // CELL_NM, bb[2] // CELL_NM + 1):
        for cy in range(bb[1] // CELL_NM, bb[3] // CELL_NM + 1):
            yield (cx, cy)


def collect_copper(board):
    """Tracks, vias and pads as one uniform list, plus a spatial index.

    Each record caches its effective shape per layer: the overlap test is per
    (item, item, layer), and rebuilding the shape each time is what makes it slow.
    """
    items, grid = [], {}

    def add(node, net, item, bb, **extra):
        shapes = {}
        for lid in item.GetLayerSet().CuStack():
            try:
                shapes[lid] = item.GetEffectiveShape(lid)
            except Exception:
                pass
        rec = {"node": node, "net": net, "item": item,
               "layers": frozenset(shapes), "shapes": shapes, "landed": False}
        rec.update(extra)
        i = len(items)
        items.append(rec)
        for c in _cells(bb):
            grid.setdefault(c, []).append(i)

    def box(item):
        bb = item.GetBoundingBox()
        return (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom())

    for t in board.GetTracks():
        net = t.GetNetname()
        if not net:
            continue
        is_via = t.Type() == pcbnew.PCB_VIA_T
        add(("via" if is_via else "trk", t.m_Uuid.AsString()), net, t, box(t),
            kind="via" if is_via else "trk",
            pts=[t.GetPosition()] if is_via else [t.GetStart(), t.GetEnd()])

    for fp in board.GetFootprints():
        ref = fp.GetReference()
        for pad in fp.Pads():
            net = pad.GetNetname()
            if not net or net.startswith("unconnected-"):
                continue
            add(("pad", ref, pad.GetPadName()), net, pad, box(pad),
                kind="pad", ref=ref, name=pad.GetPadName(),
                pos=pad.GetPosition(), value=fp.GetValue(), serving=[])

    return items, grid


def join_overlapping(dsu, items, grid):
    """Union every pair of same-net copper whose shapes actually touch.

    Pairs come from shared grid cells, which is complete: overlapping shapes have
    overlapping bounding boxes and therefore share at least one cell. A pad is
    also marked `landed` when it meets copper of its own, which is what separates
    "fed only by the pour" from "routed".
    """
    seen = set()
    for bucket in grid.values():
        n = len(bucket)
        for bi in range(n):
            for bj in range(bi + 1, n):
                i, j = bucket[bi], bucket[bj]
                key = (i, j) if i < j else (j, i)
                if key in seen:
                    continue
                seen.add(key)
                a, b = items[i], items[j]
                if a["net"] != b["net"]:
                    continue
                shared = a["layers"] & b["layers"]
                if not shared:
                    continue
                if not any(a["shapes"][L].Collide(b["shapes"][L], 0) for L in shared):
                    continue
                dsu.union(a["node"], b["node"])
                if a["kind"] == "pad" and b["kind"] != "pad":
                    a["landed"] = True
                if b["kind"] == "pad" and a["kind"] != "pad":
                    b["landed"] = True


def build_graph(board, islands, idx):
    """The whole copper graph: pours, copper-in-pour, and copper-on-copper."""
    dsu = DSU()
    for isl in islands:
        dsu.add(("isl", isl["id"]))

    items, grid = collect_copper(board)
    for rec in items:
        dsu.add(rec["node"])

    reach = pcbnew.FromMM(SERVING_RANGE_MM)
    for rec in items:
        if rec["kind"] == "pad":
            for lid in rec["layers"]:
                for isl in idx.get((rec["net"], lid), ()):
                    if isl["outline"].Distance(rec["pos"]) <= reach:
                        rec["serving"].append(isl)
                        dsu.union(rec["node"], ("isl", isl["id"]))
        else:
            for lid in rec["layers"]:
                for pt in rec["pts"]:
                    for isl in islands_at(idx, rec["net"], lid, pt.x, pt.y):
                        dsu.union(rec["node"], ("isl", isl["id"]))

    join_overlapping(dsu, items, grid)
    return dsu, items


def anchors(dsu, islands, items):
    """One component per net that counts as 'the net'.

    Prefer the component holding a connector pin -- that is what the outside
    world actually joins to. Fall back to the largest by copper area, then by
    node count, so a net with no connector still resolves.
    """
    stats = {}

    def bump(net, root):
        return stats.setdefault((net, root),
                                {"area": 0.0, "n": 0, "conn": False})

    for isl in islands:
        s = bump(isl["net"], dsu.find(("isl", isl["id"])))
        s["area"] += isl["area"]
        s["n"] += 1
    for rec in items:
        s = bump(rec["net"], dsu.find(rec["node"]))
        s["n"] += 1
        if rec["kind"] == "pad" and rec["ref"][:1] == "J":
            s["conn"] = True

    best = {}
    for (net, root), s in stats.items():
        score = (s["conn"], s["area"], s["n"])
        if net not in best or score > best[net][1]:
            best[net] = (root, score)
    return {net: root for net, (root, _) in best.items()}


def analyse(board):
    """The whole model for an already-loaded board.

    Split out from check() so rescue_gnd.py can work on the board it is about to
    modify, with the same islands and the same anchor components this gate judges
    by -- rather than a second, drifting opinion of its own.
    """
    islands = collect_islands(board)
    idx = index_islands(islands)
    dsu, items = build_graph(board, islands, idx)
    return {"board": board, "islands": islands, "idx": idx,
            "dsu": dsu, "items": items, "anchor": anchors(dsu, islands, items)}


def in_anchor_copper(an, net, x, y):
    """Is (x, y) inside a pour island of `net` that is part of that net's main body?

    This is the test a rescue via has to pass. Landing in *an* island is not enough:
    a via joining one stranded island to another has been paid for and connects
    nothing, which is the failure this gate exists to detect.
    """
    for lid_net, lid in an["idx"]:
        if lid_net != net:
            continue
        for isl in islands_at(an["idx"], net, lid, x, y):
            if an["dsu"].find(("isl", isl["id"])) == an["anchor"].get(net):
                return True
    return False


def findings_from(an, verbose_extra=None):
    """-> (findings, pads that are off-anchor but have copper of their own)."""
    dsu, anchor = an["dsu"], an["anchor"]
    pads = [r for r in an["items"] if r["kind"] == "pad"]
    findings, offnet_routed = [], []
    for p in pads:
        if dsu.find(p["node"]) == anchor.get(p["net"]):
            continue
        if p["landed"]:
            # it has copper of its own and still does not reach the net: an
            # unrouted connection, which DRC reports properly. Not this gate's lane.
            offnet_routed.append(p)
            continue
        if not p["serving"]:
            why = "no %s pour within %.1fmm on any of its layers" % (
                p["net"], SERVING_RANGE_MM)
        else:
            why = "every serving pour is stranded: " + ", ".join(
                "%s %.2fmm2" % (i["layer"], i["area"]) for i in p["serving"])
        findings.append({
            "ref": p["ref"], "pad": p["name"], "net": p["net"], "value": p["value"],
            "x": MM(p["pos"].x), "y": MM(p["pos"].y), "why": why,
            "serving": p["serving"], "pos": p["pos"],
        })

    findings.sort(key=lambda f: (f["ref"], f["pad"]))
    return findings, offnet_routed


def check(board_path, verbose=True):
    """-> list of findings. Each is a dict; an empty list means the board passes."""
    board = pcbnew.LoadBoard(board_path)
    conn = board.GetConnectivity()
    conn.RecalculateRatsnest()

    an = analyse(board)
    islands, dsu, anchor = an["islands"], an["dsu"], an["anchor"]
    pads = [r for r in an["items"] if r["kind"] == "pad"]
    findings, offnet_routed = findings_from(an)

    if verbose:
        off = sum(1 for i in islands
                  if dsu.find(("isl", i["id"])) != anchor.get(i["net"]))
        pour_dep = sum(1 for p in pads if not p["landed"])
        print("board: %s" % board_path)
        print("islands: %d (%d off-anchor) | pour-dependent pads: %d | "
              "KiCad unconnected count: %d"
              % (len(islands), off, pour_dep, conn.GetUnconnectedCount(True)))
        for f in findings:
            print("FAIL  %s pad %s [%s] (%s) @ (%.2f, %.2f) -- %s"
                  % (f["ref"], f["pad"], f["net"], f["value"], f["x"], f["y"], f["why"]))
        for p in offnet_routed:
            print("note  %s pad %s [%s] has copper of its own but does not reach its "
                  "net -- unrouted, which is DRC's report to make, not this gate's"
                  % (p["ref"], p["name"], p["net"]))
        if findings:
            print("STRANDED POUR GATE: %d finding(s)" % len(findings))
        else:
            print("STRANDED POUR GATE: PASS")
    return findings


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    total = 0
    for path in sys.argv[1:]:
        total += len(check(path))
        print("")
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
