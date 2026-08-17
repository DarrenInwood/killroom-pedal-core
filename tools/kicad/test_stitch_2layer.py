# -*- coding: utf-8 -*-
"""A 2-layer board gets stitching vias.

This one has no golden to compare against, because no 2-layer board exists in
either repository yet -- which is exactly why it is written down. Until this fix,
`in_gnd_copper()` required a hit on In1 or In2 *as well as* an outer layer. A
2-layer board has neither inner layer, so the test was false at every point on
it: the stitcher placed zero vias and printed a summary that reads like success.

"It silently did nothing and reported success" is the failure class this whole
directory exists to prevent, so it gets a test rather than a comment.

Needs pcbnew, so it does not run in CI:

    "C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad/test_stitch_2layer.py
"""
import json
import os
import sys
import tempfile

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FM = pcbnew.FromMM


def build(path, layers):
    """A square board with a GND pour on every copper layer, and two GND pads."""
    b = pcbnew.BOARD()
    b.SetCopperLayerCount(layers)

    for (x0, y0, x1, y1) in ((0, 0, 40, 0), (40, 0, 40, 40), (40, 40, 0, 40), (0, 40, 0, 0)):
        seg = pcbnew.PCB_SHAPE(b)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(pcbnew.VECTOR2I(FM(x0), FM(y0)))
        seg.SetEnd(pcbnew.VECTOR2I(FM(x1), FM(y1)))
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(FM(0.1))
        b.Add(seg)

    net = pcbnew.NETINFO_ITEM(b, "GND")
    b.Add(net)

    cu = [pcbnew.F_Cu, pcbnew.B_Cu]
    if layers >= 4:
        cu += [pcbnew.In1_Cu, pcbnew.In2_Cu]
    for layer in cu:
        z = pcbnew.ZONE(b)
        z.SetLayer(layer)
        z.SetNet(net)
        z.SetAssignedPriority(0)
        z.Outline().RemoveAllContours()
        z.Outline().NewOutline()
        for (x, y) in ((2, 2), (38, 2), (38, 38), (2, 38)):
            z.Outline().Append(FM(x), FM(y), 0)
        b.Add(z)

    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(path, b)


def stitched(path, reach_names):
    """Run the stitcher over `path` with `reach_names` reserved, return the via count.

    Driven through a real board_config.json rather than by poking the module:
    main() sets REACH_LAYERS from the config, so anything assigned beforehand is
    discarded -- which is how an earlier version of this test managed to report a
    pass while exercising neither rule.
    """
    root = os.path.dirname(path)
    cfgdir = os.path.join(root, "kicad")
    os.makedirs(cfgdir, exist_ok=True)
    cfg = os.path.join(cfgdir, "board_config.json")
    with open(cfg, "w", encoding="utf-8") as fh:
        json.dump({"schema": 1, "boards": {"b": {
            "dir": ".", "board": os.path.basename(path),
            "stitch_reach_layers": list(reach_names)}}}, fh)
    os.environ["KICAD_BOARD_CONFIG"] = cfg

    for m in ("stitch_gnd", "board_config"):
        sys.modules.pop(m, None)
    import stitch_gnd
    sys.argv = ["stitch_gnd.py", path]
    try:
        stitch_gnd.main()
    except SystemExit:
        pass
    reached = stitch_gnd.REACH_LAYERS
    after = [t for t in pcbnew.LoadBoard(path).GetTracks() if isinstance(t, pcbnew.PCB_VIA)]
    return len(after), reached


failures = []

# 2-layer, nothing reserved: the fallback is F-to-B, and it must place vias.
d2 = tempfile.mkdtemp()
p2 = os.path.join(d2, "two.kicad_pcb")
build(p2, 2)
vias2, reach2 = stitched(p2, [])
if reach2:
    failures.append("2-layer: expected no reserved layers, got %r" % (reach2,))
if vias2 <= 0:
    failures.append("2-layer board: expected stitching vias, got %d "
                    "(this is the silent no-op the fix is for)" % vias2)

# The old rule, reproduced: demand an inner hit on a board that has none. If this
# ever stops yielding zero, the check above has stopped proving anything.
d2b = tempfile.mkdtemp()
p2b = os.path.join(d2b, "two.kicad_pcb")
build(p2b, 2)
vias_old, reach_old = stitched(p2b, ["In1.Cu", "In2.Cu"])
if not reach_old:
    failures.append("the old-rule case did not actually reserve any layer, so it "
                    "tested the fallback twice")
# Not zero, and worth being exact about why: the island-seating pass earlier in
# main() places a via inside any GND island that has none, and it does not consult
# in_gnd_copper at all. What the old rule zeroes is the GRID stitching -- the 36
# vias that actually tie the two pours together. One seated via on a whole board
# is the "did nothing and reported success" case; the margin is the assertion.
elif vias_old * 5 >= vias2:
    failures.append("the old rule placed %d via(s) against the new rule's %d. It should "
                    "place only the few the island-seating pass seats regardless, so "
                    "either the grid stitching is not being suppressed or the fix is not "
                    "what makes the difference" % (vias_old, vias2))

# 4-layer with the planes reserved: the original behaviour, unchanged.
d4 = tempfile.mkdtemp()
p4 = os.path.join(d4, "four.kicad_pcb")
build(p4, 4)
vias4, reach4 = stitched(p4, ["In1.Cu", "In2.Cu"])
if not reach4:
    failures.append("4-layer: the reserved layers did not reach the stitcher")
if vias4 <= 0:
    failures.append("4-layer board: expected stitching vias, got %d" % vias4)

if failures:
    print("test_stitch_2layer: %d failure(s)" % len(failures))
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("test_stitch_2layer: PASS (2-layer %d vias, 4-layer %d vias; "
      "the old rule placed %d on 2 layers)" % (vias2, vias4, vias_old))
