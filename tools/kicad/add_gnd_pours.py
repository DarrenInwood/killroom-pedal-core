"""Clone a board's F.Cu GND pour onto its other layers.

One board per process: pcbnew is not safe to keep using after a SaveBoard().

    <kicad-python> kicad/tools/add_gnd_pours.py <board.kicad_pcb> [In1.Cu In2.Cu ...]

With no layers named, every inner copper layer the board has. A 2-layer board
therefore gets nothing and says so, rather than being asked for planes it has no
room for.
"""
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config

if len(sys.argv) < 2:
    sys.exit("usage: add_gnd_pours.py <board.kicad_pcb> [layer ...]")
path = sys.argv[1]
b = board_config.load_board(pcbnew, path)
zones = list(b.Zones())
gnd = b.FindNet("GND")
have = {z.GetLayer() for z in zones if z.GetNetname() == "GND"}
template = next(z for z in zones if z.GetNetname() == "GND" and z.GetLayer() == pcbnew.F_Cu)

_IDS = {"In1.Cu": pcbnew.In1_Cu, "In2.Cu": pcbnew.In2_Cu,
        "In3.Cu": pcbnew.In3_Cu, "In4.Cu": pcbnew.In4_Cu}
if len(sys.argv) > 2:
    targets = tuple(_IDS[n] for n in sys.argv[2:] if n in _IDS)
else:
    n_cu = b.GetCopperLayerCount()
    targets = tuple(_IDS["In%d.Cu" % i] for i in range(1, n_cu - 1) if "In%d.Cu" % i in _IDS)

added = []
for layer in targets:
    if layer in have:
        continue
    z = pcbnew.ZONE(b)
    z.SetLayer(layer)
    z.SetNet(gnd)
    z.SetAssignedPriority(0)
    z.SetIsFilled(False)
    z.SetLocalClearance(template.GetLocalClearance())
    z.SetMinThickness(template.GetMinThickness())
    z.SetPadConnection(template.GetPadConnection())
    z.SetThermalReliefGap(template.GetThermalReliefGap())
    z.SetThermalReliefSpokeWidth(template.GetThermalReliefSpokeWidth())
    # same outline as the existing pour
    z.Outline().RemoveAllContours()
    src = template.Outline()
    for c in range(src.OutlineCount()):
        pts = src.Outline(c)
        z.Outline().NewOutline()
        for i in range(pts.PointCount()):
            p = pts.CPoint(i)
            z.Outline().Append(p.x, p.y, c)
    b.Add(z)
    added.append(b.GetLayerName(layer))

print("%s: added GND pour on %s" % (os.path.basename(path), added or "(nothing)"))
pcbnew.SaveBoard(path, b)
print("saved")
