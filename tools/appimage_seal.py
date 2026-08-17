#!/usr/bin/env python3
"""Seal a linked application image so the bootloader will accept it.

The other half of [app_image.py](app_image.py): that one asks whether an image
is sealed, this one does the sealing. Both read the descriptor contract from
[`pedal_core/app_image.hpp`](../include/pedal_core/app_image.hpp), which is why
they share its constants rather than each carrying a copy.

Sealing makes the image self-describing, so a firmware update interrupted
mid-flash is caught at the next boot instead of crash-looping. Two edits to the
raw firmware.bin:

  1. Patch the descriptor's `size` field (the product's app_image.cpp, pinned by
     its linker script at HEADER_OFF) to the final image length.
  2. Append a 4-byte little-endian CRC32 trailer over the whole size-patched
     image, using the standard Ethernet polynomial — byte-for-byte the CRC the
     bootloader recomputes (`sysex_codec::crc32_update`) and the updater sends at
     FW_END, so the update path needs no host-side change.

The bootloader reads `size`, then checks CRC32 over [base, base + size - 4)
against the trailer word at base + size - 4.

**This module is imported, never executed by SCons.** PlatformIO runs the file
named in `extra_scripts` inside SCons, so a product keeps a real post-build
script that does `Import("env")` and `env.AddPostAction(...)`; only the algorithm
lives here. Standard library only.

    from appimage_seal import seal_image
    seal_image(str(target[0]))      # -> (size, crc), or None if already sealed
"""

from __future__ import annotations

import struct
import zlib

from app_image import HEADER_OFF, MAGIC

# 0x204: the `size` field within the descriptor.
SIZE_FIELD_OFF = HEADER_OFF + 4


def seal_image(path: str) -> tuple[int, int] | None:
    """Patch the size field and append the CRC32 trailer, in place.

    Returns `(size, crc)`, or `None` when the image was already sealed —
    re-running a build must never append a second trailer.
    """
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < HEADER_OFF + 8:
        raise Exception(
            "appimage_crc: image too small (%d bytes) to hold the descriptor at "
            "0x%X — did the linker place .app_header?" % (len(data), HEADER_OFF)
        )

    magic = struct.unpack_from("<I", data, HEADER_OFF)[0]
    if magic != MAGIC:
        raise Exception(
            "appimage_crc: no descriptor magic at offset 0x%X (found 0x%08X, "
            "expected 0x%08X). The app linker script must pin .app_header there."
            % (HEADER_OFF, magic, MAGIC)
        )

    # A fresh elf->bin carries size == 0; a non-zero value means this bin was
    # already patched (idempotency guard — never append a second trailer).
    if struct.unpack_from("<I", data, SIZE_FIELD_OFF)[0] != 0:
        print("appimage_crc: %s already has a CRC trailer; leaving it as is." % path)
        return None

    # Word-align the body so the trailer and every DFU 32-bit flash write land on
    # a 4-byte boundary (pad with 0xFF, the erased-flash value).
    while len(data) % 4 != 0:
        data.append(0xFF)

    size = len(data) + 4  # total image length including the 4-byte CRC trailer
    struct.pack_into("<I", data, SIZE_FIELD_OFF, size)

    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    data += struct.pack("<I", crc)

    with open(path, "wb") as f:
        f.write(data)

    print("appimage_crc: %s -> size=%d bytes (0x%X), CRC32=0x%08X"
          % (path, size, size, crc))
    return size, crc
