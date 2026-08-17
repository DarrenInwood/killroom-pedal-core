# -*- coding: utf-8 -*-
"""Where KiCad is on THIS machine.

Kept out of board_config.json on purpose. Which boards a repository has is a
property of the repository and belongs in a committed file; where KiCad is
installed is a property of the machine, and a committed
`C:/Program Files/KiCad/10.0` is wrong on the second machine, wrong on a KiCad 9
box, and wrong in CI -- and whoever fixes it breaks the first one.

Resolution order, most explicit first:

  1. an environment variable -- KICAD_CLI, KICAD_PY, KICAD_STOCK_FP,
     KICAD_STOCK_SYM, FREEROUTING_JAR
  2. <repo>/kicad/tools/local.env -- KEY=value, one per line, gitignored
  3. a probe of the usual install locations, newest version first
  4. a failure naming all three ways to fix it

Environment first because the shell pipeline (route_board.sh) exports these once
and every Python tool in that run then agrees about the toolchain. The file
exists because an editor or an agent invoking one tool directly has no exported
environment, and one line in a file beats remembering five exports.
"""
import os
import sys

LOCAL_ENV = "local.env"

_CANDIDATES = {
    "KICAD_CLI": [
        r"C:/Program Files/KiCad/10.0/bin/kicad-cli.exe",
        r"C:/Program Files/KiCad/9.0/bin/kicad-cli.exe",
        "/usr/bin/kicad-cli",
        "/usr/local/bin/kicad-cli",
        "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
    ],
    "KICAD_PY": [
        r"C:/Program Files/KiCad/10.0/bin/python.exe",
        r"C:/Program Files/KiCad/9.0/bin/python.exe",
        "/usr/bin/python3",
        "/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3",
    ],
    "KICAD_STOCK_FP": [
        r"C:/Program Files/KiCad/10.0/share/kicad/footprints",
        r"C:/Program Files/KiCad/9.0/share/kicad/footprints",
        "/usr/share/kicad/footprints",
        "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints",
    ],
    "KICAD_STOCK_SYM": [
        r"C:/Program Files/KiCad/10.0/share/kicad/symbols",
        r"C:/Program Files/KiCad/9.0/share/kicad/symbols",
        "/usr/share/kicad/symbols",
        "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols",
    ],
    "FREEROUTING_JAR": [
        os.path.join(os.path.expanduser("~"), ".kicad-mcp", "freerouting.jar"),
    ],
}

_HINT = {
    "KICAD_CLI": "KiCad's kicad-cli executable",
    "KICAD_PY": "KiCad's bundled Python (the one that can import pcbnew)",
    "KICAD_STOCK_FP": "KiCad's stock footprint libraries",
    "KICAD_STOCK_SYM": "KiCad's stock symbol libraries",
    "FREEROUTING_JAR": "the freerouting jar",
}

_cache = None


def _load_local(start=None):
    """Read <repo>/kicad/tools/local.env, if there is one. Rooted at the caller."""
    global _cache
    if _cache is not None:
        return _cache
    _cache = {}
    if start is None:
        start = os.path.dirname(os.path.abspath(sys.argv[0] or "."))
    d = os.path.abspath(start)
    while True:
        for cand in (os.path.join(d, LOCAL_ENV),
                     os.path.join(d, "kicad", "tools", LOCAL_ENV)):
            if os.path.isfile(cand):
                with open(cand, encoding="utf-8") as fh:
                    for line in fh:
                        line = line.strip()
                        if not line or line.startswith("#") or "=" not in line:
                            continue
                        k, v = line.split("=", 1)
                        _cache[k.strip()] = v.strip().strip('"').strip("'")
                return _cache
        if os.path.isdir(os.path.join(d, ".git")):
            break
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return _cache


def resolve(key, required=True):
    """The path for `key`, or None when it is not required and nothing is found."""
    val = os.environ.get(key) or _load_local().get(key)
    if val and os.path.exists(val):
        return val
    if val and required:
        raise SystemExit("kicad_env: %s is set to %s, which does not exist" % (key, val))
    for cand in _CANDIDATES.get(key, ()):
        if os.path.exists(cand):
            return cand
    if not required:
        return None
    raise SystemExit(
        "kicad_env: cannot find %s.\n"
        "  Set $%s, or add '%s=<path>' to kicad/tools/local.env (gitignored),\n"
        "  or install KiCad where this looked: %s"
        % (_HINT[key], key, key, ", ".join(_CANDIDATES.get(key, ())) or "nowhere"))


def kicad_cli():        return resolve("KICAD_CLI")
def kicad_python():     return resolve("KICAD_PY")
def stock_footprints(): return resolve("KICAD_STOCK_FP")
def stock_symbols():    return resolve("KICAD_STOCK_SYM")
def freerouting_jar():  return resolve("FREEROUTING_JAR")
