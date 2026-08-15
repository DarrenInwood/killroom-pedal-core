#!/usr/bin/env python3
"""Render a product's Markdown user manual to PDF: Pandoc + Eisvogel + Tectonic.

The family's pedals ship the same kind of manual and there is no reason for each
of them to carry its own copy of a 1000-line LaTeX template. What differs
between products is the *content* (`MANUAL.md`) and the *title page*
(`manual-meta.yaml`); everything else -- the template, the preamble, the engine
choice, the tool discovery -- lives here.

  - **Pandoc** converts the Markdown and applies the vendored Eisvogel template
    ([manual/eisvogel.latex](manual/eisvogel.latex)) plus the product's metadata.
  - **Tectonic** is the PDF engine. It fetches any missing LaTeX packages on
    demand and caches them, so no system-wide TeX distribution is needed. The
    first run needs network access to populate that cache.
  - Images are referenced from the manual's own directory via `--resource-path`,
    so `../images/foo.png` in the Markdown resolves the same way GitHub renders
    it. Nothing is embedded or copied.

A product drives it with a three-line shim in its own `tools/build_pdf.py`, so
the documented command stays `python tools/build_pdf.py` in every repo:

    from pathlib import Path
    import sys
    ROOT = Path(__file__).resolve().parent.parent
    sys.path.insert(0, str(ROOT / "firmware/lib/pedal-core/tools"))
    from build_manual import main
    sys.exit(main(["--manual", str(ROOT / "docs/manual/MANUAL.md"),
                   "--meta",   str(ROOT / "tools/manual-meta.yaml")]))

Or from the command line:

    python firmware/lib/pedal-core/tools/build_manual.py \
        --manual docs/manual/MANUAL.md --meta tools/manual-meta.yaml

Override the tool locations with the PANDOC / TECTONIC environment variables.
Standard library only.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
THEME_DIR = TOOLS_DIR / "manual"
TEMPLATE = THEME_DIR / "eisvogel.latex"
HEADER_TEX = THEME_DIR / "manual-header.tex"

_LOCALAPPDATA = os.environ.get("LOCALAPPDATA", "")

# PATH first, then the usual per-user install locations on each platform.
_PANDOC_FALLBACKS = [
    Path(_LOCALAPPDATA) / "Pandoc" / "pandoc.exe" if _LOCALAPPDATA else None,
    Path(r"C:\Program Files\Pandoc\pandoc.exe"),
    Path("/usr/bin/pandoc"),
    Path("/usr/local/bin/pandoc"),
    Path("/opt/homebrew/bin/pandoc"),
]
_TECTONIC_FALLBACKS = [
    Path(_LOCALAPPDATA) / "Programs" / "tectonic" / "tectonic.exe" if _LOCALAPPDATA else None,
    Path("/usr/bin/tectonic"),
    Path("/usr/local/bin/tectonic"),
    Path("/opt/homebrew/bin/tectonic"),
]


def find_exe(name: str, env_var: str, fallbacks) -> str | None:
    """Resolve a tool from its env override, then PATH, then known install paths."""
    override = os.environ.get(env_var)
    if override:
        return override
    found = shutil.which(name)
    if found:
        return found
    for cand in fallbacks:
        if cand is not None and cand.exists():
            return str(cand)
    return None


def build_argv() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="build_manual.py",
        description="Render a pedal's MANUAL.md to PDF with Pandoc, Eisvogel and Tectonic.",
    )
    p.add_argument("--manual", required=True, type=Path,
                   help="the product's MANUAL.md")
    p.add_argument("--meta", required=True, type=Path,
                   help="the product's Pandoc metadata (title page, geometry, colours)")
    p.add_argument("--out", type=Path, default=None,
                   help="output PDF (default: the manual's path with a .pdf suffix)")
    p.add_argument("--extra-header", type=Path, default=None,
                   help="additional verbatim LaTeX preamble, appended after the shared one")
    p.add_argument("--resource-path", type=Path, action="append", default=None,
                   help="extra directory to resolve images from; repeatable. The manual's "
                        "own directory is always searched first")
    return p


def main(argv=None) -> int:
    args = build_argv().parse_args(argv)

    manual = args.manual.resolve()
    meta = args.meta.resolve()
    out = (args.out or manual.with_suffix(".pdf")).resolve()

    for label, path in (("manual", manual), ("metadata", meta)):
        if not path.is_file():
            sys.exit(f"build_manual: {label} not found: {path}")
    if not TEMPLATE.is_file():
        sys.exit(f"build_manual: the Eisvogel template is missing: {TEMPLATE}\n"
                 "This file ships with pedal-core; if the submodule was checked out "
                 "shallowly or is behind, run `git submodule update --init --remote`.")

    pandoc = find_exe("pandoc", "PANDOC", _PANDOC_FALLBACKS)
    if not pandoc:
        sys.exit("build_manual: pandoc not found. Install it "
                 "(https://pandoc.org/installing.html) or set the PANDOC "
                 "environment variable.")
    tectonic = find_exe("tectonic", "TECTONIC", _TECTONIC_FALLBACKS)
    if not tectonic:
        sys.exit("build_manual: tectonic not found. Install it "
                 "(https://tectonic-typesetting.github.io/) or set the TECTONIC "
                 "environment variable.")

    # The manual's own directory first, so `../images/x.png` resolves exactly as it
    # does when GitHub renders the Markdown.
    resources = [manual.parent] + [p.resolve() for p in (args.resource_path or [])]

    cmd = [
        pandoc,
        str(manual),
        "--from", "markdown",
        "--template", str(TEMPLATE),
        "--metadata-file", str(meta),
        # Verbatim preamble (named colours, code line-wrapping); not Markdown-parsed.
        "--include-in-header", str(HEADER_TEX),
    ]
    if args.extra_header:
        if not args.extra_header.is_file():
            sys.exit(f"build_manual: extra header not found: {args.extra_header}")
        cmd += ["--include-in-header", str(args.extra_header.resolve())]
    cmd += [
        "--pdf-engine", tectonic,
        # Source sections are H2 (## Introduction, ...) under a single document title
        # from the metadata; promote them so each becomes a top-level LaTeX section.
        "--shift-heading-level-by=-1",
    ]
    for r in resources:
        cmd += ["--resource-path", str(r)]
    cmd += ["-o", str(out)]

    print(f"pandoc:   {pandoc}")
    print(f"tectonic: {tectonic}")
    print(f"Rendering {manual} -> {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stderr.strip():
        print(result.stderr, file=sys.stderr)
    if result.returncode != 0:
        sys.exit(f"build_manual: pandoc failed (exit {result.returncode}).")

    print(f"Done: {out}  ({out.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
