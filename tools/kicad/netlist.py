# -*- coding: utf-8 -*-
"""Read a KiCad s-expression netlist.

One parser. There were three, in placelib.py, verify_sync.py and sync_full.py,
each grown for what its own caller needed: one collected sheet paths, one
collected pin types, one collected neither. They shared their two regexes, so a
fix to how a `(comp ...)` block is matched had to be made three times or be wrong
twice.

The three differed in one way that mattered. The sheet-path variant required a
`(sheetpath ...)` inside the comp block for the component to be seen at all, so a
component without one would have been dropped silently rather than reported. Here
every field is optional and absence is recorded as absence: `sheets` simply has no
entry for a component that carries no sheet path.

Text, not s-expression parsing, because that is what the originals did and this
move is not the place to change how a netlist is read. Standard library only.

    nl = netlist.parse(path)
    nl.comps   # {ref: (value, fpid)}
    nl.pins    # {(ref, pin): net}
    nl.sheets  # {ref: sheet}      -- only for comps that carry a sheetpath
    nl.types   # {(ref, pin): (pintype, pinfunction)}
"""
import re

# A component block: ref and value are always present; footprint and sheetpath are
# matched when they are there and skipped when they are not.
_COMP = re.compile(
    r'\(comp\s*\r?\n\s*\(ref "([^"]+)"\)\s*\r?\n\s*\(value "([^"]*)"\)'
    r'(?:\s*\r?\n\s*\(footprint "([^"]*)"\))?', re.S)

_COMP_SHEET = re.compile(
    r'\(comp\s*\r?\n\s*\(ref "([^"]+)"\)\s*\r?\n\s*\(value "([^"]*)"\)'
    r'(?:\s*\r?\n\s*\(footprint "([^"]*)"\))?.*?'
    r'\(sheetpath\s*\r?\n\s*\(names "([^"]*)"\)', re.S)

_NET_NAME = re.compile(r'^\t\t\t\(name "([^"]+)"\)')
_REF = re.compile(r'\(ref "([^"]+)"\)')
_PIN = re.compile(r'\(pin "([^"]+)"\)')
_PINTYPE = re.compile(r'\(pintype "([^"]*)"\)')
_PINFUNC = re.compile(r'\(pinfunction "([^"]*)"\)')

# How far past a (ref ...) line the matching (pin ...) can be. The originals all
# used five lines; a node block is four.
_NODE_SPAN = 5


class Netlist(object):
    __slots__ = ("path", "comps", "pins", "sheets", "types")

    def __init__(self, path):
        self.path = path
        self.comps = {}
        self.pins = {}
        self.sheets = {}
        self.types = {}

    def __repr__(self):
        return "<Netlist %s: %d comps, %d pins>" % (
            self.path, len(self.comps), len(self.pins))


def parse(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()

    nl = Netlist(path)

    for m in _COMP.finditer(text):
        nl.comps[m.group(1)] = (m.group(2), m.group(3) or "")
    for m in _COMP_SHEET.finditer(text):
        nl.sheets[m.group(1)] = m.group(4)

    lines = text.split("\n")
    cur = None
    for i, line in enumerate(lines):
        g = _NET_NAME.match(line)
        if g:
            cur = g.group(1)
            continue
        r = _REF.search(line)
        if r and cur:
            blk = "\n".join(lines[i:i + _NODE_SPAN])
            p = _PIN.search(blk)
            if p:
                key = (r.group(1), p.group(1))
                nl.pins[key] = cur
                t = _PINTYPE.search(blk)
                f = _PINFUNC.search(blk)
                nl.types[key] = (t.group(1) if t else "", f.group(1) if f else "")
    return nl
