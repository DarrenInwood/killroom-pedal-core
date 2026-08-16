#!/usr/bin/env python3
"""Shared drawing toolkit for the manual's line-art figures.

A thin vector-style layer over PIL: everything is drawn at SS× and box-filtered down on save,
so strokes and type land anti-aliased without a vector toolchain. Coordinates are always final
pixels — the primitives scale themselves.

The PNGs carry a 200 dpi pHYs chunk, which is what Pandoc reads to size a figure. A sheet wider
than about 1300 px therefore lands wider than the manual's 166 mm text block, and the figure
fills the column.

Shared by the family: each product keeps its own figure scripts -- what a pedal's signal path
looks like is the pedal's business -- and they all draw with these primitives, so the figures
in two manuals are recognisably the same hand. A product imports it from this submodule:

    sys.path.insert(0, str(ROOT / "firmware/lib/pedal-core/tools"))
    from diagram_kit import Sheet, block, jack, ...
"""

from __future__ import annotations

import os
from functools import lru_cache

from PIL import Image, ImageDraw, ImageFont

SS = 3            # supersample factor
DPI = 200         # written into the PNG so Pandoc scales the figure to the text width
MARGIN = 60       # sheet margin: titles start here, notes wrap before its mirror

PAPER = (255, 255, 255)
INK = (29, 37, 41)          # wires, symbols, primary type
SOFT = (112, 122, 130)      # secondary type, dividers
NAVY = (27, 58, 91)         # linknavy from manual-header.tex — headings and callouts
PANEL = (243, 245, 247)     # block fill
HAIR = (214, 219, 223)      # panel outlines that must not compete with the wires

LW = 2.6          # wire weight
TITLE, HEAD, LBL, SMALL = 34, 25, 24, 22

FONT_REGULAR = [
    "C:/Windows/Fonts/arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/Library/Fonts/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]
FONT_BOLD = [
    "C:/Windows/Fonts/arialbd.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
]


@lru_cache(maxsize=None)
def font(size_px: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    for cand in (FONT_BOLD if bold else FONT_REGULAR):
        if os.path.exists(cand):
            return ImageFont.truetype(cand, round(size_px * SS))
    raise SystemExit("no diagram font found; edit FONT_REGULAR / FONT_BOLD")


class Sheet:
    """A drawing surface in final-pixel coordinates."""

    def __init__(self, w: int, h: int):
        self.w, self.h = w, h
        self.im = Image.new("RGB", (w * SS, h * SS), PAPER)
        self.d = ImageDraw.Draw(self.im)

    def _s(self, pts):
        return [(x * SS, y * SS) for x, y in pts]

    # ---- strokes -------------------------------------------------------------------

    def line(self, pts, w=LW, fill=INK):
        self.d.line(self._s(pts), fill=fill, width=max(1, round(w * SS)), joint="curve")

    def wire(self, *pts, fill=INK):
        self.line(list(pts), LW, fill)

    def arrow(self, *pts, fill=INK, head=13.0):
        """Polyline ending in a solid head, aimed along the last segment."""
        self.line(list(pts), LW, fill)
        (x0, y0), (x1, y1) = pts[-2], pts[-1]
        dx, dy = x1 - x0, y1 - y0
        n = max(1e-6, (dx * dx + dy * dy) ** 0.5)
        ux, uy = dx / n, dy / n
        bx, by = x1 - ux * head, y1 - uy * head
        self.poly([(x1, y1), (bx - uy * head * 0.44, by + ux * head * 0.44),
                   (bx + uy * head * 0.44, by - ux * head * 0.44)], fill)

    def dashed(self, x0, y0, x1, y1, dash=10.0, gap=8.0, w=1.6, fill=SOFT, skip=None):
        """Dashed segment. `skip` is an optional (start, end) run left blank, which is how a
        control line hops a signal wire it merely crosses."""
        n = max(1e-6, ((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5)
        ux, uy = (x1 - x0) / n, (y1 - y0) / n
        t = 0.0
        while t < n:
            a, b = t, min(t + dash, n)
            t += dash + gap
            if skip and b > skip[0] and a < skip[1]:
                continue
            self.line([(x0 + ux * a, y0 + uy * a), (x0 + ux * b, y0 + uy * b)], w, fill)

    def dashed_v(self, x, y0, y1, **kw):
        self.dashed(x, y0, x, y1, **kw)

    # ---- shapes --------------------------------------------------------------------

    def dot(self, x, y, r=6.0, fill=INK):
        self.d.ellipse(self._s([(x - r, y - r), (x + r, y + r)]), fill=fill)

    def circle(self, x, y, r, w=LW, outline=INK, fill=None):
        self.d.ellipse(self._s([(x - r, y - r), (x + r, y + r)]),
                       outline=outline, width=max(1, round(w * SS)), fill=fill)

    def rrect(self, x0, y0, x1, y1, r=16, w=2.0, outline=INK, fill=None):
        self.d.rounded_rectangle(self._s([(x0, y0), (x1, y1)]), radius=r * SS,
                                 outline=outline, width=max(1, round(w * SS)), fill=fill)

    def poly(self, pts, fill=INK):
        self.d.polygon(self._s(pts), fill=fill)

    # ---- type ----------------------------------------------------------------------

    def text(self, x, y, s, size=LBL, fill=INK, bold=False, anchor="lm"):
        self.d.text((x * SS, y * SS), s, font=font(size, bold), fill=fill, anchor=anchor)

    def width_of(self, s, size=LBL, bold=False):
        return self.d.textlength(s, font=font(size, bold)) / SS

    def fit(self, s, limit, size, bold=False):
        """Guard for type that would silently crop: the sheet is fixed, the strings are not."""
        w = self.width_of(s, size, bold)
        if w > limit:
            raise SystemExit(f"text overruns its {round(limit)} px slot by {round(w - limit)} px: {s!r}")
        return w

    def save(self, path):
        out = self.im.resize((self.w, self.h), Image.LANCZOS)
        path.parent.mkdir(parents=True, exist_ok=True)
        out.save(path, dpi=(DPI, DPI))
        print(f"  {os.path.relpath(path)}  {out.width}x{out.height}")


# --------------------------------------------------------------------------- common parts

def title_block(sh: Sheet, title, subtitle=None):
    sh.text(MARGIN, 46, title, TITLE, NAVY, bold=True)
    if subtitle:
        sh.text(MARGIN, 88, subtitle, SMALL, SOFT)


def notes(sh: Sheet, y0, lines, step=34, x=MARGIN):
    """Bulleted footnotes across the bottom of a sheet, one line each."""
    for i, text in enumerate(lines):
        y = y0 + i * step
        sh.text(x, y, "\u2022", SMALL, NAVY, bold=True)
        sh.fit(text, sh.w - MARGIN - (x + 22), SMALL)
        sh.text(x + 22, y, text, SMALL, SOFT)


def block(sh: Sheet, x0, y0, x1, y1, title, sub=None, size=LBL - 2, sub_size=SMALL - 3):
    """A labelled stage box: bold name, optional soft second line."""
    sh.rrect(x0, y0, x1, y1, r=10, w=2.2, fill=PANEL)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    slot = x1 - x0 - 16
    sh.fit(title, slot, size, bold=True)
    if sub:
        sh.fit(sub, slot, sub_size)
        sh.text(cx, cy - 13, title, size, INK, bold=True, anchor="mm")
        sh.text(cx, cy + 14, sub, sub_size, SOFT, anchor="mm")
    else:
        sh.text(cx, cy, title, size, INK, bold=True, anchor="mm")


def sum_node(sh: Sheet, cx, cy, r=22.0):
    """Summing junction: a circle with a plus in it."""
    sh.circle(cx, cy, r, LW, INK, PAPER)
    sh.line([(cx - r * 0.45, cy), (cx + r * 0.45, cy)], LW)
    sh.line([(cx, cy - r * 0.45), (cx, cy + r * 0.45)], LW)


def amp(sh: Sheet, x0, cy, w=38.0, h=36.0):
    """Gain block: the classic amplifier triangle, pointing along the flow."""
    sh.d.polygon(sh._s([(x0, cy - h / 2), (x0, cy + h / 2), (x0 + w, cy)]),
                 outline=INK, width=max(1, round(LW * SS)), fill=PANEL)


def jack(sh: Sheet, cx, cy, label, r=11.0, above=True, offset=32.0):
    """Panel connector: a ringed dot with its name set clear of the wire."""
    sh.circle(cx, cy, r, LW, INK, PAPER)
    sh.dot(cx, cy, r * 0.42)
    sh.text(cx, cy - offset if above else cy + offset, label, LBL - 2, INK, bold=True, anchor="mm")
