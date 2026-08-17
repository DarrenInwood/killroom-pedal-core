#!/usr/bin/env python3
"""Every relative markdown link resolves to a file that exists.

A doc that links a file which was never written reads as a promise. The failure
is quiet -- nothing breaks, the link is simply followed to nowhere, usually by
someone who was told to read it first -- so it needs a gate rather than a habit.

The repository root arrives as an argument. This file lives in a submodule, so
resolving it from `__file__` would point inside the library and check the wrong
tree; the product's shim knows where it is and passes it in.

Standard library only, so CI needs no pip step.

    from check_doc_links import main
    sys.exit(main(root=ROOT))
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

# Directories whose markdown is not the product's to answer for: build output and
# version control. A product adds to these -- a vendored dependency's docs are the
# obvious case, and a submodule that gates its own docs is another.
DEFAULT_SKIP = (".git", ".pio", "node_modules", "build", ".venv", "__pycache__")

# [text](target) -- the target runs to the first unescaped ')'. Reference-style
# links and bare autolinks are not used in this family.
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")

SKIP_SCHEMES = ("http://", "https://", "mailto:", "ftp://", "#")

# Fenced code blocks hold example paths that are deliberately hypothetical.
FENCE = re.compile(r"^\s*(```|~~~)")

# An inline code span. Markup inside one is shown, not followed -- a template that
# quotes the link its instances should carry must not have that quote resolved from
# the template's own directory.
CODE_SPAN = re.compile(r"(`+)(?:(?!\1).)*\1", re.DOTALL)


def strip_fenced_blocks(text: str) -> str:
    """Blank out fenced code blocks, keeping line count so numbers stay right."""
    out, in_fence = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            out.append("")
        else:
            out.append("" if in_fence else line)
    return "\n".join(out)


def check(root: Path, skip_dirs=DEFAULT_SKIP):
    """-> (failures, checked). A failure is 'path:line: target'."""
    failures: list[str] = []
    checked = 0
    skip = set(skip_dirs)

    for md in sorted(root.rglob("*.md")):
        if skip.intersection(md.parts):
            continue
        text = strip_fenced_blocks(md.read_text(encoding="utf-8", errors="replace"))

        for lineno, line in enumerate(text.splitlines(), 1):
            line = CODE_SPAN.sub("", line)
            for target in LINK.findall(line):
                if target.startswith(SKIP_SCHEMES):
                    continue

                # Split off the anchor. An empty path means a same-file anchor,
                # which is always resolvable.
                path_part = unquote(target.split("#", 1)[0])
                if not path_part:
                    continue

                checked += 1
                if not (md.parent / path_part).resolve().exists():
                    rel = md.relative_to(root).as_posix()
                    failures.append(f"{rel}:{lineno}: {target}")

    return failures, checked


def main(argv=None, *, root: Path, skip_dirs=DEFAULT_SKIP,
         hint: str = "Write the file, or don't link it.") -> int:
    failures, checked = check(Path(root), skip_dirs)

    if failures:
        print(f"check_doc_links: {len(failures)} dangling link(s)\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print(f"\n{hint}", file=sys.stderr)
        return 1

    print(f"check_doc_links: PASS ({checked} relative links resolve)")
    return 0


if __name__ == "__main__":
    # Run directly, this gates the library's own docs. Resolving from __file__ is
    # right here and only here: standalone, this file's grandparent IS the repo
    # root. Imported by a consumer it would be the submodule, which is why the
    # function takes the root instead.
    sys.exit(main(root=Path(__file__).resolve().parent.parent))
