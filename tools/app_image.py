#!/usr/bin/env python3
"""A built application image is one the bootloader will accept.

The bootloader refuses to jump into an image it cannot verify, so an image that
fails these checks does not crash — it silently leaves every pedal sitting in
DFU. That failure mode is quiet enough to ship, which is why it is checked at
build time rather than discovered on hardware.

Re-implements [`pedal_core/app_image.hpp`](../include/pedal_core/app_image.hpp)'s
`validate()` against the linked binary:

  * the descriptor sits at `HEADER_OFF` with the right magic -- proving
    app_image.cpp was linked, .app_header was KEEP'd past --gc-sections, and
    the vector table did not grow over it;
  * the size field was patched, is word-aligned, and fits the region;
  * the CRC32 trailer matches the body.

The descriptor's shape is the library's, so it lives here. The size of the
application region is the product's, so it arrives as `region_bytes` — a product
whose bootloader reserves a different slice of flash passes its own figure and
everything else is shared.

Only a production env produces a bootloader-compatible image. A development build
links standalone at the flash base and is flashed with a programmer, so it has no
pinned descriptor and this check does not apply to it.

Standard library only, so the gate runs in CI without a pip step.

A product calls this through a shim that supplies its own region size:

    from app_image import main
    sys.exit(main(sys.argv, region_bytes=..., usage=__doc__))
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

# The descriptor contract, shared with include/pedal_core/app_image.hpp and each
# product's linker script. These are the library's, not a product's.
MAGIC = 0x4D4F4448
HEADER_OFF = 0x200
HEADER_SIZE = 16


def check(path: Path, region_bytes: int) -> list[str]:
    """Every reason this image would be refused, or an empty list."""
    img = path.read_bytes()
    problems: list[str] = []

    if len(img) < HEADER_OFF + HEADER_SIZE:
        return [f"{path}: only {len(img)} bytes — too small to hold its descriptor"]

    magic, size = struct.unpack_from("<II", img, HEADER_OFF)
    if magic != MAGIC:
        problems.append(
            f"{path}: no descriptor at 0x{HEADER_OFF:X} (found 0x{magic:08X}, want "
            f"0x{MAGIC:08X}) — app_image.cpp unlinked, .app_header dropped by "
            "--gc-sections, or the vector table has grown over it"
        )
        return problems

    if size == 0:
        problems.append(f"{path}: size field is 0 — the sealing post-build step did not run")
        return problems
    if size != len(img):
        problems.append(f"{path}: size field {size} != image length {len(img)}")
    if size % 4:
        problems.append(f"{path}: size {size} is not word-aligned")
    if size > region_bytes:
        problems.append(f"{path}: {size} bytes exceeds the {region_bytes}-byte app region")
    if size < HEADER_OFF + HEADER_SIZE + 4:
        problems.append(f"{path}: {size} bytes is shorter than a descriptor plus a trailer")

    if not problems:
        stored = struct.unpack_from("<I", img, size - 4)[0]
        calc = zlib.crc32(img[: size - 4]) & 0xFFFFFFFF
        if stored != calc:
            problems.append(
                f"{path}: CRC32 trailer 0x{stored:08X} != computed 0x{calc:08X}"
            )
    return problems


def main(argv: list[str], *, region_bytes: int, usage: str | None = None) -> int:
    args = argv[1:]
    if len(args) != 1:
        print(usage or __doc__)
        return 2

    path = Path(args[0])
    if not path.exists():
        print(f"check_app_image: {path} does not exist — build it first", file=sys.stderr)
        return 1

    problems = check(path, region_bytes)
    if problems:
        print(f"check_app_image: {len(problems)} problem(s)\n", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    size = struct.unpack_from("<I", path.read_bytes(), HEADER_OFF + 4)[0]
    print(f"check_app_image: PASS ({path.name} sealed, {size} bytes, "
          f"{size * 100 // region_bytes}% of the app region)")
    return 0
