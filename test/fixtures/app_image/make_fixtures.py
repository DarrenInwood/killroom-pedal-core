#!/usr/bin/env python3
"""Build the image-descriptor fixtures. Run by hand; the output is committed.

The fixtures exist so that the three implementations of the descriptor contract --
`app_image::validate()` in C++, `tools/appimage_seal.py` which writes it, and
`tools/app_image.py` which gates a release on it -- are checked against the same
bytes rather than each asserting the layout independently.

This script is deliberately NOT imported by either test. It spells the layout out
from literals rather than importing tools/app_image.py, because a generator that
shares a module with one side under test would let a shared misunderstanding
produce a passing test. The tests read the committed bytes and never run this.

    python test/fixtures/app_image/make_fixtures.py
"""

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The layout, written out here rather than imported. If these drift from
# include/pedal_core/app_image.hpp the fixtures stop describing the contract, and
# both tests fail -- which is the point.
MAGIC = 0x4D4F4448
HEADER_OFF = 0x200
HEADER_SIZE = 16
IDENT_FORMAT = 1


def ident(manufacturer, device):
    return IDENT_FORMAT | (manufacturer << 8) | (device << 16)


def version(major, minor, patch):
    return patch | (minor << 8) | (major << 16)


def image(size_field, total_len, *, magic=MAGIC, seal=True, corrupt_crc=False):
    """An image `total_len` bytes long, declaring `size_field` in its descriptor.

    Filled with a recognisable pattern so a fixture opened in a hex editor reads as
    deliberate rather than as leftover memory.
    """
    img = bytearray((i * 7 + 3) & 0xFF for i in range(total_len))

    struct.pack_into("<II", img, HEADER_OFF, magic, size_field)
    struct.pack_into("<I", img, HEADER_OFF + 8, ident(0x7D, 0x01))
    struct.pack_into("<I", img, HEADER_OFF + 12, version(1, 2, 3))

    if seal and size_field >= 4 and size_field <= total_len:
        crc = zlib.crc32(bytes(img[: size_field - 4])) & 0xFFFFFFFF
        if corrupt_crc:
            crc ^= 0xA5A5A5A5
        struct.pack_into("<I", img, size_field - 4, crc)
    return bytes(img)


# (name, bytes) -- the manifest records what each one should be judged.
FIXTURES = [
    # A sealed image both sides accept.
    ("valid.bin", image(0x240, 0x240)),

    # No descriptor where one is expected: app_image.cpp unlinked, or the vector
    # table grown over it.
    ("bad_magic.bin", image(0x240, 0x240, magic=0xDEADBEEF)),

    # The size field was never patched by the sealing step.
    ("size_zero.bin", image(0, 0x240, seal=False)),

    # Not word-aligned: the build pads to a 4-byte boundary before the trailer.
    ("unaligned.bin", image(0x241, 0x241)),

    # Shorter than a descriptor plus a trailer can fit.
    ("too_short.bin", image(0x210, 0x210)),

    # Larger than the region the manifest gives it.
    ("oversize.bin", image(0x340, 0x340)),

    # Sealed, then the trailer corrupted -- what an aborted flash leaves behind.
    ("bad_crc.bin", image(0x240, 0x240, corrupt_crc=True)),

    # The descriptor is honest and the CRC is right, but the file carries trailing
    # bytes past the declared size. The two sides disagree about this one on
    # purpose; the manifest says so and why.
    ("trailing_bytes.bin", image(0x240, 0x300)),

    # Too small to hold a descriptor at all. Only the file-oriented side can judge
    # this: validate() reads at HEADER_OFFSET before it knows any size.
    ("stub.bin", bytes((i * 7 + 3) & 0xFF for i in range(0x100))),
]


def main():
    for name, data in FIXTURES:
        (HERE / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
