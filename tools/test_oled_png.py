# -*- coding: utf-8 -*-
"""Pin write_png()'s "only when the pixels change" rule.

A PNG's compressed bytes depend on the zlib build that wrote them, so rewriting an
unchanged screen produces a different file with identical pixels. A consumer that
regenerates its screenshots -- which the multi-effect's checklist asks for after any UI
change, icon redraw or version bump -- then gets a diff of every screen it has, in which
the handful that genuinely changed is indistinguishable from the noise.

So write_png() compares what it is about to write against what the file already holds,
decoded, and skips the write when they match. Decoded, because the bytes are exactly the
thing that cannot be trusted.

    python tools/test_oled_png.py
"""
import os
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oled_png

failures = []


def check(what, got, want):
    if got != want:
        failures.append("%s\n     got: %r\n    want: %r" % (what, got, want))


PALETTE = [(0, 0, 0), (0, 0, 0), (0, 255, 0)]
OTHER_PALETTE = [(0, 0, 0), (0, 0, 0), (0, 0, 255)]
W, H = 8, 4


def pixels(fill=1):
    return bytearray([fill] * (W * H))


def raw_scanlines(indices):
    return b"".join(b"\x00" + bytes(indices[y * W:(y + 1) * W]) for y in range(H))


def hand_write(path, indices, palette, level):
    """A PNG of the same pixels, compressed differently -- what another zlib build gives."""
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    plte = b"".join(bytes(c) for c in palette)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 3, 0, 0, 0))
           + chunk(b"PLTE", plte)
           + chunk(b"IDAT", zlib.compress(raw_scanlines(indices), level))
           + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def with_phys(path):
    """Insert a pHYs chunk after IHDR, as a consumer's DPI stamp does."""
    with open(path, "rb") as fh:
        raw = fh.read()
    out, i = bytearray(raw[:8]), 8
    while i < len(raw):
        length = int.from_bytes(raw[i:i + 4], "big")
        kind = raw[i + 4:i + 8]
        out += raw[i:i + 12 + length]
        i += 12 + length
        if kind == b"IHDR":
            data = b"pHYs" + (7874).to_bytes(4, "big") + (7874).to_bytes(4, "big") + b"\x01"
            out += ((len(data) - 4).to_bytes(4, "big") + data
                    + zlib.crc32(data).to_bytes(4, "big"))
    with open(path, "wb") as fh:
        fh.write(bytes(out))


tmp = tempfile.mkdtemp(prefix="oled_png_test_")
try:
    p = os.path.join(tmp, "screen.png")

    # A file that is not there yet is written.
    check("first write reports written", oled_png.write_png(p, W, H, pixels(1), PALETTE), True)
    first = open(p, "rb").read()

    # The same pixels again change nothing -- neither the answer nor the bytes.
    check("same pixels reports skipped", oled_png.write_png(p, W, H, pixels(1), PALETTE), False)
    check("same pixels leaves the file alone", open(p, "rb").read(), first)

    # The actual bug: a file holding these pixels compressed by a different zlib build.
    # The bytes differ, the pixels do not, and the file must survive untouched.
    hand_write(p, pixels(1), PALETTE, level=1)
    other = open(p, "rb").read()
    check("a differently-compressed file is not byte-equal", other == first, False)
    check("different compression, same pixels reports skipped",
          oled_png.write_png(p, W, H, pixels(1), PALETTE), False)
    check("different compression leaves the file alone", open(p, "rb").read(), other)

    # A consumer stamps pHYs in after the fact. Chunks we do not compare must not
    # provoke a rewrite, or the whole thing churns anyway one step later.
    with_phys(p)
    stamped = open(p, "rb").read()
    check("pHYs-stamped file reports skipped",
          oled_png.write_png(p, W, H, pixels(1), PALETTE), False)
    check("pHYs-stamped file is left alone", open(p, "rb").read(), stamped)

    # One changed pixel is a changed screen.
    changed = pixels(1)
    changed[0] = 2
    check("changed pixel reports written", oled_png.write_png(p, W, H, changed, PALETTE), True)
    check("changed pixel rewrites", open(p, "rb").read() != stamped, True)

    # The palette is part of the picture: the same indices in another colour are a
    # different screenshot, and each pedal renders its own panel colour.
    check("changed palette reports written",
          oled_png.write_png(p, W, H, changed, OTHER_PALETTE), True)
    check("changed palette then settles",
          oled_png.write_png(p, W, H, changed, OTHER_PALETTE), False)

    # Size changes are changes, even when every pixel value matches.
    check("changed height reports written",
          oled_png.write_png(p, W, H * 2, bytearray([2] * (W * H * 2)), OTHER_PALETTE), True)

    # A truncated or corrupt file is replaced rather than trusted or raised over.
    with open(p, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\nnot really a png")
    check("corrupt file reports written", oled_png.write_png(p, W, H, pixels(1), PALETTE), True)
    check("corrupt file is replaced",
          open(p, "rb").read()[:8], b"\x89PNG\r\n\x1a\n")

    # A file that is not a PNG at all is likewise replaced, not parsed.
    with open(p, "wb") as fh:
        fh.write(b"")
    check("empty file reports written", oled_png.write_png(p, W, H, pixels(1), PALETTE), True)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

if failures:
    print("test_oled_png: %d failure(s)\n" % len(failures))
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("test_oled_png: PASS (write_png rewrites on a real change and only then)")
