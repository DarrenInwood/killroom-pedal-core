#!/usr/bin/env python3
"""Turn captured display frames into PNGs and a contact sheet.

Reads the `frames.bin` a host build writes through `pedal_core/frame_dump.hpp`
and emits one PNG per screen plus a labelled contact sheet of all of them. The
frames are the firmware's own framebuffer contents, so this draws nothing and
decides nothing about layout -- it scales bits and writes a file.

Standard library only (`zlib` + `struct`), matching the doc gates, so CI needs no
pip step to check that a product's screenshots are current.

    python tools/oled_png.py FRAMES --out DIR [--sheet all_screens]

Importable too: `render(frames, out_dir, ...)` is what a product's own wrapper
calls, so it never has to shell out.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"PCFB"
VERSION = 1

# Palette slots. 0 is the surround the panel sits on; 1 and 2 are the panel's own
# dark and lit pixels.
BG, OFF, ON = 0, 1, 2

DEFAULT_BG = "000000"
DEFAULT_OFF = "000000"
DEFAULT_ON = "00FF00"


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------
class FrameFileError(Exception):
    """The container is not one this reader understands."""


class Frame:
    """One captured screen: its name, its caption, and two page-format bitmaps."""

    __slots__ = ("slug", "caption", "width", "pages", "bits", "strip_pages", "strip")

    def __init__(self, slug, caption, width, pages, bits, strip_pages, strip):
        self.slug = slug
        self.caption = caption
        self.width = width
        self.pages = pages
        self.bits = bits
        self.strip_pages = strip_pages
        self.strip = strip

    @property
    def height(self) -> int:
        return self.pages * 8

    @property
    def strip_height(self) -> int:
        return self.strip_pages * 8


class _Reader:
    def __init__(self, data: bytes):
        self.d = data
        self.i = 0

    def take(self, n: int) -> bytes:
        if self.i + n > len(self.d):
            raise FrameFileError(f"truncated at byte {self.i} (wanted {n} more)")
        out = self.d[self.i : self.i + n]
        self.i += n
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def text(self) -> str:
        return self.take(self.u8()).decode("ascii", "replace")


def read_frames(path: Path) -> list[Frame]:
    """Parse a frame_dump container into Frames, in the order they were written."""
    r = _Reader(Path(path).read_bytes())
    if r.take(4) != MAGIC:
        raise FrameFileError(f"{path}: not a frame dump (bad magic)")
    version = r.u16()
    if version != VERSION:
        raise FrameFileError(f"{path}: format version {version}, this reader speaks {VERSION}")
    count = r.u16()

    frames = []
    for _ in range(count):
        slug, caption = r.text(), r.text()
        width, pages = r.u8(), r.u8()
        bits = r.take(pages * width)
        strip_pages = r.u8()
        strip = r.take(strip_pages * width)
        frames.append(Frame(slug, caption, width, pages, bits, strip_pages, strip))

    if r.i != len(r.d):
        raise FrameFileError(f"{path}: {len(r.d) - r.i} trailing byte(s) after {count} frames")
    return frames


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------
def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _holds_already(path: Path, ihdr: bytes, plte: bytes, raw: bytes) -> bool:
    """True when the file at `path` already holds exactly this picture.

    Compares the decoded scanlines, the palette and the header, and ignores every other
    chunk. Decoded, because the compressed bytes are the one thing that cannot be trusted:
    deflate output depends on the zlib build that produced it, so an unchanged screen
    re-encoded elsewhere is a different file holding an identical picture. Ignoring other
    chunks matters just as much -- a consumer that stamps its own pHYs in afterwards would
    otherwise see every screen rewritten on the next run.

    Any surprise (missing, truncated, not a PNG, multiple IDATs that will not inflate)
    answers False, so the caller writes. This decides whether to skip work, never whether
    the output is correct.
    """
    try:
        data = Path(path).read_bytes()
    except OSError:
        return False
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return False

    found_ihdr = found_plte = None
    idat = bytearray()
    i = 8
    while i + 8 <= len(data):
        length = int.from_bytes(data[i:i + 4], "big")
        tag = data[i + 4:i + 8]
        body = data[i + 8:i + 8 + length]
        if len(body) != length:
            return False                      # truncated
        if tag == b"IHDR":
            found_ihdr = body
        elif tag == b"PLTE":
            found_plte = body
        elif tag == b"IDAT":
            idat += body                      # a writer may split the stream
        i += 12 + length

    if found_ihdr != ihdr or found_plte != plte:
        return False
    try:
        return zlib.decompress(bytes(idat)) == raw
    except zlib.error:
        return False


def write_png(path: Path, width: int, height: int, indices: bytearray,
              palette: list[tuple[int, int, int]]) -> bool:
    """An 8-bit palette PNG. Two or three colours compress to a few KB at this size.

    Writes only when the file does not already hold this picture, and returns whether it
    wrote. Regenerating screenshots is a routine step in a consumer's checklist, so a run
    that changed one screen has to leave a diff naming that one screen.
    """
    plte = b"".join(bytes(c) for c in palette)
    raw = b"".join(b"\x00" + bytes(indices[y * width : (y + 1) * width]) for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    if _holds_already(path, ihdr, plte, raw):
        return False
    png = (b"\x89PNG\r\n\x1a\n"
           + _chunk(b"IHDR", ihdr)
           + _chunk(b"PLTE", plte)
           + _chunk(b"IDAT", zlib.compress(raw, 9))
           + _chunk(b"IEND", b""))
    Path(path).write_bytes(png)
    return True


def _blit(dst: bytearray, dst_w: int, x0: int, y0: int,
          bits: bytes, width: int, pages: int, scale: int) -> None:
    """Draw a page-format bitmap into an index buffer, one source pixel per scale x scale block."""
    for page in range(pages):
        row = page * width
        for y_in_page in range(8):
            mask = 1 << y_in_page
            y = y0 + (page * 8 + y_in_page) * scale
            for x in range(width):
                value = ON if bits[row + x] & mask else OFF
                px = x0 + x * scale
                for dy in range(scale):
                    start = (y + dy) * dst_w + px
                    for dx in range(scale):
                        dst[start + dx] = value


def frame_image(f: Frame, scale: int, border: int) -> tuple[int, int, bytearray]:
    """One screen on its own: the panel at `scale`, inset by a `border` of surround."""
    w = f.width * scale + border * 2
    h = f.height * scale + border * 2
    buf = bytearray([BG]) * (w * h)
    _blit(buf, w, border, border, f.bits, f.width, f.pages, scale)
    return w, h, buf


def sheet_image(frames: list[Frame], cols: int, scale: int, border: int,
                gap: int) -> tuple[int, int, bytearray]:
    """Every screen tiled, each above its caption strip. The grid uses the largest cell."""
    if not frames:
        raise ValueError("no frames to tile")
    width = max(f.width for f in frames)
    tall = max(f.height for f in frames)
    cap = max(f.strip_height for f in frames)

    cell_w = width * scale + border * 2
    cell_h = tall * scale + (gap + cap * scale if cap else 0) + border * 2
    rows = (len(frames) + cols - 1) // cols

    sheet_w, sheet_h = cols * cell_w, rows * cell_h
    buf = bytearray([BG]) * (sheet_w * sheet_h)

    for i, f in enumerate(frames):
        x0 = (i % cols) * cell_w + border
        y0 = (i // cols) * cell_h + border
        _blit(buf, sheet_w, x0, y0, f.bits, f.width, f.pages, scale)
        if f.strip_pages:
            _blit(buf, sheet_w, x0, y0 + tall * scale + gap,
                  f.strip, f.width, f.strip_pages, scale)
    return sheet_w, sheet_h, buf


# ---------------------------------------------------------------------------
def _rgb(s: str) -> tuple[int, int, int]:
    v = s.lstrip("#")
    if len(v) != 6:
        raise ValueError(f"colour must be six hex digits, got {s!r}")
    return (int(v[0:2], 16), int(v[2:4], 16), int(v[4:6], 16))


def render(frames_path, out_dir, *, scale=4, border=10, sheet=None, sheet_scale=2,
           cols=5, gap=4, bg=DEFAULT_BG, off=DEFAULT_OFF, on=DEFAULT_ON,
           log=print) -> list[Path]:
    """Write one PNG per frame into out_dir, and optionally `<sheet>.png` beside them.

    Returns every output path, whether or not this run had to write it: a screen that
    already held the right picture is left alone, so regenerating after a change to one
    screen leaves a diff naming that one screen. The log line says how many were rewritten,
    which is the number worth checking against what you expected to change.
    """
    frames = read_frames(Path(frames_path))
    if not frames:
        raise FrameFileError(f"{frames_path}: no frames")

    palette = [_rgb(bg), _rgb(off), _rgb(on)]
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    paths = []
    rewritten = 0
    seen = set()
    for f in frames:
        if f.slug in seen:
            raise FrameFileError(f"{frames_path}: two frames both named {f.slug!r}")
        seen.add(f.slug)
        path = out_dir / f"{f.slug}.png"
        rewritten += write_png(path, *frame_image(f, scale, border), palette)
        paths.append(path)

    if sheet:
        path = out_dir / f"{sheet}.png"
        # The sheet tiles every screen, so it changes whenever any of them does -- through
        # the same rule, which is what keeps it out of a diff when none of them did.
        rewritten += write_png(path, *sheet_image(frames, cols, sheet_scale, border, gap),
                               palette)
        paths.append(path)

    if log:
        log(f"oled_png: {len(frames)} screens -> {out_dir}"
            + (f", contact sheet {sheet}.png" if sheet else "")
            + (f" ({rewritten} rewritten)" if rewritten else " (all unchanged)"))
    return paths


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("frames", help="the frames.bin a host build wrote")
    p.add_argument("--out", required=True, help="directory for the PNGs")
    p.add_argument("--scale", type=int, default=4, help="pixels per display pixel (default 4)")
    p.add_argument("--border", type=int, default=10, help="surround in output pixels (default 10)")
    p.add_argument("--sheet", default=None, help="also write <name>.png tiling every screen")
    p.add_argument("--sheet-scale", type=int, default=2, help="scale inside the sheet (default 2)")
    p.add_argument("--cols", type=int, default=5, help="sheet columns (default 5)")
    p.add_argument("--bg", default=DEFAULT_BG, help=f"surround colour (default {DEFAULT_BG})")
    p.add_argument("--off", default=DEFAULT_OFF, help=f"unlit pixel (default {DEFAULT_OFF})")
    p.add_argument("--on", default=DEFAULT_ON, help=f"lit pixel (default {DEFAULT_ON})")
    a = p.parse_args(argv)

    try:
        render(a.frames, a.out, scale=a.scale, border=a.border, sheet=a.sheet,
               sheet_scale=a.sheet_scale, cols=a.cols, bg=a.bg, off=a.off, on=a.on)
    except (FrameFileError, ValueError, OSError) as e:
        print(f"oled_png: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
