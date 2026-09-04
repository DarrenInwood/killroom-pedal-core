# -*- coding: utf-8 -*-
"""How much room a via or track laid AFTER routing must leave around other copper.

0.25mm to anything is the floor, and for most boards it is also the answer. But a board may carry
KiCad custom rules that demand more around a particular netclass, and those live in a `.kicad_dru`
which the SWIG API does not expose -- so a tool placing copper cannot read them and places to the
floor instead. The multi-effect asks 0.35mm around HiZ_Audio (`protect_hiz_nodes`, its
high-impedance CV nodes) and 0.4mm around Clock (`clock_away_from_audio`); every route ended with
the same handful of clearance errors against those two classes, each one a via the stitcher or the
rescuer had placed to 0.25mm and been satisfied with.

The board states the numbers it needs in `board_config.json`:

    "stitch_clearance": { "HiZ_Audio": 0.35, "Clock": 0.4 }

A board that says nothing keeps the flat floor, so this changes no other product's output.
"""
import math

FLOOR = 0.25

_by_class = {}


def load(board_cfg):
    """Take the clearance map off a board_config Board. Safe to call with None."""
    _by_class.clear()
    if board_cfg is None:
        return
    for name, mm in (board_cfg.get("stitch_clearance") or {}).items():
        _by_class[str(name)] = float(mm)


def clear_for(item, cache):
    """Clearance to keep from `item` (a pad, track or via), in mm.

    `cache` is a caller-owned dict keyed by netcode -- resolving the class per obstacle is far too
    slow otherwise, and rescue_gnd rebuilds its obstacle set once per net.
    """
    if not _by_class:
        return FLOOR
    code = item.GetNetCode()
    if code not in cache:
        # GetNetClassName() resolves the .kicad_pro's netclass PATTERNS, so it only answers
        # correctly for a board sitting beside its own project file -- which is where every caller
        # runs it. A board opened away from its .kicad_pro reports "Default" for every net, and
        # would quietly get the flat floor back with nothing to say why.
        try:
            cache[code] = item.GetNetClassName()
        except AttributeError:
            cache[code] = ""
    return max(FLOOR, _by_class.get(cache[code], 0.0))

def disc_chain(ax, ay, cx, cy, r, step=0.5):
    """A track segment as overlapping discs of radius `r`, for a point-in-circle test.

    Discs spaced along a segment leave a SAG between neighbours: midway between two, the union
    reaches only sqrt(r^2 - (s/2)^2), not r. A via landing there clears the check and fails DRC,
    which is what left one 0.344mm violation against a 0.35mm rule -- 0.045mm of sag at the
    spacing this uses. Growing each disc to sqrt(r^2 + (s/2)^2) makes the union cover the whole
    swept band: the midpoint then reaches exactly r. It costs no extra discs, and it is never
    permissive -- at a sample point it is over-strict by that same 0.045mm, which is the safe
    direction for a tool deciding where to drop copper.
    """
    seg = math.hypot(cx - ax, cy - ay)
    n = max(1, int(seg / step))
    rr = math.sqrt(r * r + (seg / n / 2.0) ** 2)
    return [(ax + (cx - ax) * i / n, ay + (cy - ay) * i / n, rr) for i in range(n + 1)]
