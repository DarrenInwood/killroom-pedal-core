#!/usr/bin/env python3
"""The version a firmware is, and the manifest entry that publishes it.

Two jobs, together because they have to agree.

A pedal reports its version over SysEx 0x31, and the editor compares that against
the manifest to decide whether to offer an update. If a release tag says one
thing and product.hpp says another, the editor offers an upgrade that installs
the firmware already on the pedal: an update that appears to work, changes
nothing, and comes back next time. So the tag is checked against the source
rather than trusted, and a mismatch fails the release.

The manifest is per product rather than one shared file. Two repositories
releasing near each other would otherwise read-modify-write the same object and
one would silently lose its entry; separate files cannot race, and the editor
fetches both. That is also why the product's identity arrives as arguments: the
publishing *mechanism* is the family's, the identity is not.

`device_id` and `slug` are wire and URL contracts. The editor keys on the device
id, and the slug names the published artifacts, so the manifest URL is built from
it — changing either is a breaking change, not a rename.

Standard library only.

A product calls this through a shim carrying its own identity:

    from release_manifest import main
    sys.exit(main(sys.argv, product="...", device_id=1, slug="...",
                  header=Path("firmware/src/config/product.hpp"),
                  macros=("..._MAJOR", "..._MINOR", "..._PATCH")))
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence
import re


def version_from_source(header: Path, macros: Sequence[str]) -> str:
    """The version the firmware will actually report over 0x31."""
    text = header.read_text(encoding="utf-8")
    parts = []
    for macro in macros:
        match = re.search(rf"{macro}\s*=?\s*(\d+)", text)
        if not match:
            raise SystemExit(f"release_manifest: {macro} not found in {header}")
        parts.append(match.group(1))
    return ".".join(parts)


def main(argv: list[str], *, product: str, device_id: int, slug: str,
         header: Path, macros: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-version", help="the release tag's version, e.g. 1.2.0")
    parser.add_argument("--image", type=Path, help="the built .bin; omit to check the version only")
    parser.add_argument("--out", type=Path, help="where to write the manifest")
    parser.add_argument("--notes", default="", help="what changed, in a sentence a player cares about")
    args = parser.parse_args(argv[1:])

    version = version_from_source(header, macros)

    if args.expect_version and args.expect_version != version:
        print(
            f"release_manifest: the tag says {args.expect_version}, {header} says {version}.\n"
            "  A pedal reports the source version over 0x31, so publishing this would offer an\n"
            "  update that installs the firmware already on the pedal. Fix one of them.",
            file=sys.stderr,
        )
        return 1

    # Version-check only.
    if args.image is None or args.out is None:
        print(f"release_manifest: {product} {version}")
        return 0

    if not args.image.exists():
        print(f"release_manifest: {args.image} does not exist — build it first", file=sys.stderr)
        return 1

    entry = {
        "deviceId": device_id,
        "product": product,
        "version": version,
        # Relative to the manifest, so the bucket can move without a rewrite.
        "url": f"{slug}-{version}.bin",
        "bytes": args.image.stat().st_size,
    }
    notes = args.notes.strip()
    if notes:
        entry["notes"] = notes

    args.out.write_text(json.dumps({"releases": [entry]}, indent=2) + "\n", encoding="utf-8")
    print(f"release_manifest: {product} {version}, {entry['bytes']} bytes -> {args.out}")
    return 0
