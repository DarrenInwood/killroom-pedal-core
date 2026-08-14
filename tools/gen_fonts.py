"""
Variable-width bitmap font tooling for the OLED display.

Phase A (this file, --preview): load candidate fonts (BDF or TTF) and render an
on-OLED preview sheet so a human can choose one font per size slot.

Later phases extend this to emit the chosen fonts as C arrays (firmware/src/drivers/font_data.*)
and a Python module (tools/font_data.py) so the firmware and the render_display.py mock
share one source of truth.

Usage:
    python tools/gen_fonts.py --preview      # writes docs/font_previews/*.png
"""

import os
import sys

# Pillow is imported lazily inside the preview/TTF rendering paths only. The default
# emit() path (font_data.{hpp,cpp,py} generation) parses BDF glyphs in pure Python, so
# it must run on a minimal environment without Pillow installed.

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT      = os.path.normpath(os.path.join(TOOLS_DIR, ".."))
FONTS_DIR = os.path.join(TOOLS_DIR, "fonts")
PREVIEW_DIR = os.path.join(ROOT, "docs", "font_previews")

ASCII_FIRST, ASCII_LAST = 0x20, 0x7E

# Extra glyphs appended right after the printable-ASCII block, in output-code order,
# so the firmware and mocks can reference each by a single contiguous byte. Each entry
# maps the next output slot to the BDF source codepoint it is drawn from.
#   0x7F (just past '~') = ± (plus-minus, BDF/Latin-1 codepoint 0xB1).
EXTRA_GLYPHS = [(0x7F, 0xB1)]
EXTRA_SRC = {src for _slot, src in EXTRA_GLYPHS}


# ---------------------------------------------------------------------------
# Font front-ends: each exposes ascent/descent/height + render_line(fb, x, top_y, s)
# fb is a 2D list[y][x] of 0/1; render_line draws 1s and returns the x advance.
# ---------------------------------------------------------------------------
class BdfFont:
    """Parse the printable-ASCII glyphs of a BDF bitmap font."""

    def __init__(self, path):
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.glyphs = {}   # code -> (bbw, bbh, xoff, yoff, dwidth, rows[list[int]])
        self._parse(path)
        tops = [yoff + bbh for (_, bbh, _, yoff, _, _) in self.glyphs.values()]
        bots = [yoff for (_, _, _, yoff, _, _) in self.glyphs.values()]
        self.ascent = max(tops) if tops else 0
        self.descent = max(0, -min(bots)) if bots else 0
        self.height = self.ascent + self.descent

    def _parse(self, path):
        with open(path, encoding="latin-1") as fh:
            lines = fh.read().splitlines()
        i, n = 0, len(lines)
        while i < n:
            if lines[i].startswith("STARTCHAR"):
                code = dwidth = None
                bbx = None
                rows = []
                while i < n and not lines[i].startswith("ENDCHAR"):
                    ln = lines[i]
                    if ln.startswith("ENCODING"):
                        code = int(ln.split()[1])
                    elif ln.startswith("DWIDTH"):
                        dwidth = int(ln.split()[1])
                    elif ln.startswith("BBX"):
                        p = ln.split()
                        bbx = (int(p[1]), int(p[2]), int(p[3]), int(p[4]))
                    elif ln == "BITMAP":
                        i += 1
                        while i < n and not lines[i].startswith("ENDCHAR"):
                            rows.append(int(lines[i].strip() or "0", 16))
                            i += 1
                        break
                    i += 1
                if code is not None and bbx and (ASCII_FIRST <= code <= ASCII_LAST
                                                  or code in EXTRA_SRC):
                    bbw, bbh, xoff, yoff = bbx
                    self.glyphs[code] = (bbw, bbh, xoff, yoff,
                                         dwidth if dwidth is not None else bbw, rows)
            i += 1

    def render_line(self, fb, x, top_y, s):
        baseline = top_y + self.ascent
        H = len(fb)
        W = len(fb[0]) if fb else 0
        for ch in s:
            g = self.glyphs.get(ord(ch)) or self.glyphs.get(ord("?"))
            if not g:
                x += 4
                continue
            bbw, bbh, xoff, yoff, dwidth, rows = g
            rowbytes = (bbw + 7) // 8
            y0 = baseline - (yoff + bbh)
            for r, val in enumerate(rows):
                yy = y0 + r
                if yy < 0 or yy >= H:
                    continue
                for col in range(bbw):
                    if (val >> (rowbytes * 8 - 1 - col)) & 1:
                        xx = x + xoff + col
                        if 0 <= xx < W:
                            fb[yy][xx] = 1
            x += dwidth
        return x

    def text_width(self, s):
        w = 0
        for ch in s:
            g = self.glyphs.get(ord(ch)) or self.glyphs.get(ord("?"))
            w += g[4] if g else 4
        return w

    def glyph_columns(self, code):
        """Normalise a glyph into the fixed-height box: (ink_width, advance, [col ints]).

        Each column int holds `height` bits, bit 0 = top of the box (LSB-top).
        """
        g = self.glyphs.get(code) or self.glyphs.get(ord("?"))
        if g is None:
            return 0, 4, [], self.height
        bbw, bbh, xoff, yoff, dwidth, rows = g
        left = max(0, xoff)               # left side bearing as padding
        width = left + bbw
        top = self.ascent - (yoff + bbh)  # box row of the glyph's top edge
        rowbytes = (bbw + 7) // 8
        cols = [0] * width
        for r, val in enumerate(rows):
            box_row = top + r
            if box_row < 0 or box_row >= self.height:
                continue
            for c in range(bbw):
                if (val >> (rowbytes * 8 - 1 - c)) & 1:
                    col = left + c
                    if 0 <= col < width:
                        cols[col] |= (1 << box_row)
        return width, dwidth, cols, self.height


# ---------------------------------------------------------------------------
# Emit: chosen fonts -> firmware/src/drivers/font_data.{hpp,cpp} + tools/font_data.py
# ---------------------------------------------------------------------------
SRC_DRIVERS = os.path.join(ROOT, "src")
HPP_PATH = os.path.join(ROOT, "include", "pedal_core", "font_data.hpp")
CPP_PATH = os.path.join(SRC_DRIVERS, "font_data.cpp")
PY_PATH  = os.path.join(TOOLS_DIR, "font_data.py")
GEN_NOTE = "Generated by tools/gen_fonts.py from tools/fonts/ — do not edit by hand."

# role name -> (source BDF, human label for comments)
CHOSEN = [
    ("FONT_NAME",    "helvB12.bdf", "preset name, 12pt (~17px)"),
    ("FONT_NAME_MD", "helvB10.bdf", "preset name, 10pt header fallback (~14px)"),
    ("FONT_NAME_SM", "helvB08.bdf", "preset name, 8pt header fallback (~11px)"),
    ("FONT_TEXT",    "helvR08.bdf", "algorithm / param names + value magnitudes (9px)"),
    ("FONT_SMALL",   "4x6.bdf",     "page indicator / units / message (4x6)"),
]


def _resolved():
    out = []
    for role, src, label in CHOSEN:
        font = BdfFont(os.path.join(FONTS_DIR, src))
        bpc = (font.height + 7) // 8
        widths, advances, offsets, blob = [], [], [], bytearray()
        out_codes = list(range(ASCII_FIRST, ASCII_LAST + 1)) + [src for _slot, src in EXTRA_GLYPHS]
        for code in out_codes:
            w, adv, cols, h = font.glyph_columns(code)
            offsets.append(len(blob))
            widths.append(w)
            advances.append(adv)
            for col in cols:
                for b in range(bpc):
                    blob.append((col >> (8 * b)) & 0xFF)
        out.append((role, src, label, font, bpc, widths, advances, offsets, bytes(blob)))
    return out


def _c_array(name, values, typ="uint8_t", per_line=16):
    lines = [f"static const {typ} {name}[] = {{"]
    for i in range(0, len(values), per_line):
        lines.append("    " + ",".join(str(v) for v in values[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def emit_c(resolved):
    hpp = ["#pragma once", '#include "font.hpp"', "",
           f"// {GEN_NOTE}", "namespace display {"]
    for role, _src, label, *_ in resolved:
        hpp.append(f"extern const Font {role};  // {label}")
    hpp += ["}  // namespace display", ""]
    with open(HPP_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(hpp))

    cpp = ['#include <pedal_core/font_data.hpp>', "", f"// {GEN_NOTE}", "namespace display {", ""]
    for role, src, label, font, bpc, widths, advances, offsets, blob in resolved:
        p = role.lower()
        cpp.append(f"// {role}: {label}  (source {src}, {font.height}px box, ascent {font.ascent})")
        cpp.append(_c_array(f"{p}_w", widths))
        cpp.append(_c_array(f"{p}_a", advances))
        cpp.append(_c_array(f"{p}_o", offsets, typ="uint16_t", per_line=12))
        cpp.append(_c_array(f"{p}_b", list(blob)))
        cpp.append(
            f"const Font {role} = {{ {font.height}, {font.ascent}, {ASCII_FIRST}, "
            f"{len(widths)}, {p}_w, {p}_a, {p}_o, {p}_b }};")
        cpp.append("")
    cpp.append("}  // namespace display")
    cpp.append("")
    with open(CPP_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(cpp))


def emit_py(resolved):
    lines = [f"# {GEN_NOTE}",
             "# Mirror of firmware/src/drivers/font_data.* for tools/render_display.py.",
             "# Each font: height, ascent, first, glyphs[i] = (ink_width, advance, [column ints]).",
             "# A column int holds `height` bits, bit 0 = top.", "", "FONTS = {"]
    for role, src, label, font, bpc, widths, advances, offsets, blob in resolved:
        glyphs = []
        for i in range(len(widths)):
            w = widths[i]
            start = offsets[i]
            cols = []
            for c in range(w):
                v = 0
                for b in range(bpc):
                    v |= blob[start + c * bpc + b] << (8 * b)
                cols.append(v)
            glyphs.append((w, advances[i], cols))
        lines.append(f'    "{role}": {{  # {label} (source {src})')
        lines.append(f'        "height": {font.height}, "ascent": {font.ascent}, "first": {ASCII_FIRST},')
        lines.append(f'        "glyphs": {glyphs!r},')
        lines.append("    },")
    lines += ["}", ""]
    with open(PY_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def emit():
    resolved = _resolved()
    emit_c(resolved)
    emit_py(resolved)
    for role, src, label, font, bpc, widths, advances, offsets, blob in resolved:
        print(f"{role:11s} {src:16s} {font.height}px box, ascent {font.ascent}, "
              f"{len(blob)} bitmap bytes")
    print(f"  -> {os.path.relpath(HPP_PATH, ROOT)}, {os.path.relpath(CPP_PATH, ROOT)}, "
          f"{os.path.relpath(PY_PATH, ROOT)}")


class TtfFont:
    """Render an outline TTF at a fixed pixel size as a bilevel bitmap."""

    def __init__(self, path, px):
        from PIL import ImageFont
        self.name = f"{os.path.splitext(os.path.basename(path))[0]}@{px}"
        self.px = px
        self.font = ImageFont.truetype(path, px)
        self.ascent, self.descent = self.font.getmetrics()
        self.height = self.ascent + self.descent

    def _mask(self, s):
        m = self.font.getmask(s, mode="1")
        w, h = m.size
        data = bytes(m)
        return w, h, data  # 1 byte per pixel (0 / non-zero)

    def render_line(self, fb, x, top_y, s):
        H = len(fb)
        W = len(fb[0]) if fb else 0
        w, h, data = self._mask(s)
        for yy in range(h):
            ty = top_y + yy
            if ty < 0 or ty >= H:
                continue
            base = yy * w
            for xx in range(w):
                if data[base + xx] and 0 <= x + xx < W:
                    fb[ty][x + xx] = 1
        return x + w

    def text_width(self, s):
        return self.font.getmask(s, mode="1").size[0]


# ---------------------------------------------------------------------------
# Preview sheet
# ---------------------------------------------------------------------------
SAMPLE_LONG = "MyDelay 003"
SAMPLE_MIX = "Chorus 1/2  Rate 1.5kHz +7st 70% Off"


def _fb_to_img(fb, scale, bg=(20, 20, 20), on=(210, 240, 210), off=(30, 40, 30)):
    from PIL import Image
    H, W = len(fb), len(fb[0])
    img = Image.new("RGB", (W * scale, H * scale), bg)
    px = img.load()
    for y in range(H):
        for x in range(W):
            c = on if fb[y][x] else off
            for dy in range(scale):
                for dx in range(scale):
                    px[x * scale + dx, y * scale + dy] = c
    return img


def _strip(font, sample, width=128, scale=3):
    h = max(font.height + 2, 8)
    fb = [[0] * width for _ in range(h)]
    font.render_line(fb, 1, 1, sample)
    return _fb_to_img(fb, scale)


def build_preview():
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    # (slot label, sample text, [(display_name, font, license), ...])
    # Preview the fonts currently in use (one per role). Add candidate fonts here, with
    # their source vendored under tools/fonts/, to compare options before choosing.
    groups = [
        ("FONT_NAME  helvB12 (~16px, preset name)", SAMPLE_LONG, [
            ("helvB12", BdfFont(os.path.join(FONTS_DIR, "helvB12.bdf")), "Adobe X11"),
        ]),
        ("FONT_TEXT  helvR08 (9px, algo / params / values)", SAMPLE_MIX, [
            ("helvR08", BdfFont(os.path.join(FONTS_DIR, "helvR08.bdf")), "Adobe X11"),
        ]),
        ("FONT_SMALL  4x6 (page indicator / units / message)", SAMPLE_MIX, [
            ("4x6", BdfFont(os.path.join(FONTS_DIR, "4x6.bdf")), "Public domain"),
        ]),
    ]

    from PIL import Image, ImageDraw, ImageFont as PF
    label_font = PF.load_default()
    pad, label_h, gap = 10, 14, 10
    rows = []  # (image, caption)
    for slot, sample, fonts in groups:
        rows.append(("HEADER", slot))
        for disp, font, lic in fonts:
            img = _strip(font, sample)
            rows.append((img, f"{disp}   [{lic}]   h={font.height}px"))

    width = 128 * 3 + pad * 2
    total_h = pad
    for kind, _ in rows:
        total_h += (label_h if kind == "HEADER" else (label_h + 96 // 3 + gap))
    # compute precise height
    total_h = pad
    for item, cap in rows:
        if item == "HEADER":
            total_h += label_h + 6
        else:
            total_h += label_h + item.height + gap

    sheet = Image.new("RGB", (width, total_h), (12, 12, 12))
    draw = ImageDraw.Draw(sheet)
    y = pad
    for item, cap in rows:
        if item == "HEADER":
            draw.text((pad, y), cap, fill=(120, 200, 255), font=label_font)
            y += label_h + 6
        else:
            draw.text((pad, y), cap, fill=(180, 180, 180), font=label_font)
            y += label_h
            sheet.paste(item, (pad, y))
            y += item.height + gap

    out = os.path.join(PREVIEW_DIR, "font_candidates.png")
    sheet.save(out)
    print(f"Wrote {os.path.relpath(out, ROOT)}  ({sheet.size[0]}x{sheet.size[1]})")


if __name__ == "__main__":
    if "--preview" in sys.argv:
        build_preview()
    else:
        emit()
