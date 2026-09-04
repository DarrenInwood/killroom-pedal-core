# -*- coding: utf-8 -*-
"""A netclass that asks for more room than the floor gets it.

The bug this guards: stitch_gnd and rescue_gnd both inflated every obstacle by one flat 0.25mm,
so every via they laid beside the multi-effect's HiZ_Audio nets satisfied them and then failed
`protect_hiz_nodes`, which wants 0.35mm. Five such errors survived every route, were nudged by
hand, and came back on the next one.

No pcbnew here -- the lookup is pure arithmetic over a name, so it runs in CI:

    python tools/kicad/test_via_clearance.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import via_clearance  # noqa: E402


class Item(object):
    """The two methods clear_for() uses off a pad, track or via."""

    def __init__(self, code, klass, counter):
        self._code, self._klass, self._counter = code, klass, counter

    def GetNetCode(self):
        return self._code

    def GetNetClassName(self):
        self._counter[0] += 1
        return self._klass


class Cfg(object):
    def __init__(self, d):
        self._d = d

    def get(self, key, default=None):
        return self._d.get(key, default)


def main():
    fails = []

    def eq(got, want, what):
        if abs(got - want) > 1e-9:
            fails.append("%s: got %r, want %r" % (what, got, want))

    n = [0]
    # 1. no config at all -- every board that says nothing keeps the flat floor
    via_clearance.load(None)
    eq(via_clearance.clear_for(Item(1, "HiZ_Audio", n), {}), 0.25, "no config, HiZ_Audio")

    # 2. a board that says nothing about clearance, same answer
    via_clearance.load(Cfg({"stitch_reach_layers": ["In1.Cu"]}))
    eq(via_clearance.clear_for(Item(1, "HiZ_Audio", n), {}), 0.25, "config without the key")

    # 3. the named classes get their number, everything else the floor
    via_clearance.load(Cfg({"stitch_clearance": {"HiZ_Audio": 0.35, "Clock": 0.4}}))
    eq(via_clearance.clear_for(Item(1, "HiZ_Audio", n), {}), 0.35, "HiZ_Audio")
    eq(via_clearance.clear_for(Item(2, "Clock", n), {}), 0.40, "Clock")
    eq(via_clearance.clear_for(Item(3, "Audio", n), {}), 0.25, "an unnamed class")
    eq(via_clearance.clear_for(Item(4, "", n), {}), 0.25, "a net with no class name")

    # 4. the floor is a FLOOR: a board cannot ask for less and get it
    via_clearance.load(Cfg({"stitch_clearance": {"Sloppy": 0.1}}))
    eq(via_clearance.clear_for(Item(5, "Sloppy", n), {}), 0.25, "a class asking below the floor")

    # 5. the cache is per netcode, and it is what keeps this off the hot path: rescue_gnd rebuilds
    #    its obstacle set once per net over every pad and track on the board.
    via_clearance.load(Cfg({"stitch_clearance": {"HiZ_Audio": 0.35}}))
    cache, n[0] = {}, 0
    for _ in range(50):
        via_clearance.clear_for(Item(7, "HiZ_Audio", n), cache)
    if n[0] != 1:
        fails.append("cache: GetNetClassName called %d times for one netcode, want 1" % n[0])

    # 6. load() replaces rather than accumulates, so one process can do two boards
    via_clearance.load(Cfg({"stitch_clearance": {"Clock": 0.4}}))
    eq(via_clearance.clear_for(Item(8, "HiZ_Audio", n), {}), 0.25, "stale map after a reload")

    for f in fails:
        print("FAIL", f)
    print("%s -- %d check(s)" % ("FAIL" if fails else "ok", 9))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
