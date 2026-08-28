#!/usr/bin/env python3
"""The release gate agrees with the bootloader about what a valid image is.

`tools/app_image.py` re-implements `app_image::validate()` so a build that could not
boot fails CI instead of a pedal. Two implementations of one contract in two
languages is a divergence waiting to happen, and the failure mode is silent: an
image the bootloader refuses does not crash, it leaves the pedal in DFU.

So both sides are held to the same fixtures. This walks
`test/fixtures/app_image/manifest.tsv` through `check()`; `test_app_image.cpp`
walks the same rows through `validate()`. A row where the two are meant to differ
says so, and says why.

Standard library only, like the gate it tests.

    python tools/test_app_image.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from app_image import check  # noqa: E402

FIXTURES = ROOT / "test" / "fixtures" / "app_image"


def rows():
    """The manifest, minus its comments and its header line."""
    for line in (FIXTURES / "manifest.tsv").read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if parts[0] == "file":
            continue
        yield {
            "file": parts[0],
            "region_bytes": int(parts[1]),
            "cpp": parts[2],
            "python": parts[3],
            "note": parts[4] if len(parts) > 4 else "",
        }


def main():
    failures = []
    checked = 0

    manifest = list(rows())
    if not manifest:
        print("FAIL: the manifest has no rows")
        return 1

    for row in manifest:
        path = FIXTURES / row["file"]
        if not path.exists():
            failures.append(f"{row['file']}: missing — regenerate with make_fixtures.py")
            continue

        expected = row["python"]
        if expected == "n-a":
            failures.append(
                f"{row['file']}: marked n-a for python, but check() can judge any file"
            )
            continue

        problems = check(path, row["region_bytes"])
        got = "valid" if not problems else "invalid"
        checked += 1

        if got != expected:
            detail = "; ".join(problems) if problems else "no problems reported"
            failures.append(
                f"{row['file']}: expected {expected}, got {got} — {detail}\n"
                f"    the manifest says: {row['note']}"
            )

    # A fixture nobody lists is a fixture nobody checks.
    listed = {row["file"] for row in manifest}
    for stray in sorted(FIXTURES.glob("*.bin")):
        if stray.name not in listed:
            failures.append(f"{stray.name}: on disk but not in the manifest")

    if failures:
        print(f"FAIL: {len(failures)} of {len(manifest)} fixtures")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"test_app_image: PASS ({checked} fixtures judged as the manifest says)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
