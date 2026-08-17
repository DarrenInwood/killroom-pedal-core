# -*- coding: utf-8 -*-
"""What a product's boards are, for the tools that gate and package them.

The gates in this directory are the family's; the boards they run on are not. A
board's fab paths, its layer count, which side the machine places and which refs
it may not place at all are the product's, and they live in ONE file per
repository:

    <repo>/kicad/board_config.json

One file rather than one per board, because three checks reason across boards at
once -- two-board mating, stack clearance, and the master BOM that merges an LCSC
number found on the other board. A repo-level file is the only place a relation
between two boards can be stated. The map is keyed by profile name ("main",
"ctrl"), which is already the argument every one of these tools takes, so no
command line changes when a tool moves here.

**Machine paths are deliberately absent.** Where KiCad is installed is a property
of the machine, not of the boards, and a committed file naming
C:/Program Files/KiCad/10.0 is wrong on the next one. See kicad_env.py.

JSON because it is stdlib on both interpreters (system Python and KiCad's bundled
3.11), and because this family already keeps machine-read project data as JSON.
Its one real cost is comments, so: **any key beginning with an underscore is
ignored**, which is where the reason for a value goes --

    "_why": "4-layer; B.Cu carries the hand-laid clocks, so In1 stays a solid plane."

A missing key is not an error here. `Board.get(name, default)` lets a gate skip a
check it has no figure for and say so, which is better than inventing one: a
board with no outline yet has no size limit to check against, and a config that
guesses reads exactly like a config that knows.

    cfg  = board_config.load()
    prof = cfg.board(sys.argv[2])
    if prof.layers == 2: ...
"""
import json
import os
import sys

CONFIG_NAME = "board_config.json"
ENV_OVERRIDE = "KICAD_BOARD_CONFIG"


class ConfigError(Exception):
    """The config is missing, malformed, or does not describe the board asked for."""


class Board(object):
    """One board's entry. Attribute access for the common fields, get() for the rest."""

    def __init__(self, name, data, root):
        self.name = name
        self.root = root
        self._d = dict((k, v) for k, v in data.items() if not k.startswith("_"))

    def __getattr__(self, key):
        try:
            return self._d[key]
        except KeyError:
            raise AttributeError(
                "board '%s' has no '%s' in %s" % (self.name, key, CONFIG_NAME))

    def __getitem__(self, key):
        """Subscripting, so a gate lifted from a repo keeps reading `profile["layers"]`.

        The dicts these entries replaced were subscripted throughout. Supporting
        both spellings is what let those tools move without their bodies changing,
        which is the only way a reviewer can see that nothing else did.
        """
        try:
            return self._d[key]
        except KeyError:
            raise ConfigError(
                "board '%s' has no '%s' in %s" % (self.name, key, CONFIG_NAME))

    def get(self, key, default=None):
        return self._d.get(key, default)

    def has(self, key):
        return key in self._d

    def path(self, key):
        """A path field, resolved against the repo root and normalised."""
        rel = self._d.get(key)
        if rel is None:
            raise ConfigError("board '%s' has no '%s' path in %s"
                              % (self.name, key, CONFIG_NAME))
        return os.path.normpath(os.path.join(self.root, self.dir, rel))

    def refs(self, key):
        """A designator set field; absent means empty."""
        return set(self._d.get(key) or ())


class Config(object):
    def __init__(self, path, data):
        self.path = path
        # <repo>/kicad/board_config.json -> <repo>
        self.root = os.path.dirname(os.path.dirname(os.path.abspath(path)))
        self._d = data
        self.boards = {}
        for name, entry in (data.get("boards") or {}).items():
            if name.startswith("_"):
                continue
            self.boards[name] = Board(name, entry, self.root)

    @property
    def vendor(self):
        return self._d.get("vendor", "jlcpcb")

    def board(self, name):
        try:
            return self.boards[name]
        except KeyError:
            raise ConfigError("no board '%s' in %s; this repo has: %s"
                              % (name, self.path, ", ".join(sorted(self.boards)) or "none"))

    def board_for_path(self, pcb):
        """The board whose `board` file is `pcb`, for tools given only a path.

        export_gerbers takes a board and no profile, and deliberately does not
        import pcbnew -- it only shells kicad-cli, so it runs under any Python.
        Reading the layer count from the config keeps it that way.
        """
        want = os.path.normcase(os.path.abspath(pcb))
        for b in self.boards.values():
            try:
                if os.path.normcase(b.path("board")) == want:
                    return b
            except ConfigError:
                continue
        raise ConfigError("%s is not any board in %s; pass --layers to override"
                          % (pcb, self.path))


def find_config(start=None):
    """Locate the product's board_config.json.

    Rooted at the INVOKED script (sys.argv[0]), never at this file: this module
    lives in a submodule, so resolving from __file__ would walk up inside the
    library and find the wrong repository -- or none.
    """
    override = os.environ.get(ENV_OVERRIDE)
    if override:
        return override

    if start is None:
        start = os.path.dirname(os.path.abspath(sys.argv[0] or "."))
    d = os.path.abspath(start)
    while True:
        cand = os.path.join(d, "kicad", CONFIG_NAME)
        if os.path.isfile(cand):
            return cand
        # A board_config.json beside the caller (kicad/tools/ -> kicad/) counts too.
        cand = os.path.join(d, CONFIG_NAME)
        if os.path.isfile(cand):
            return cand
        if os.path.isdir(os.path.join(d, ".git")):
            break
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    raise ConfigError(
        "no kicad/%s found above %s -- set %s, or pass --config"
        % (CONFIG_NAME, start, ENV_OVERRIDE))


def load(path=None):
    if path is None:
        path = find_config()
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except ValueError as e:
        raise ConfigError("%s is not valid JSON: %s" % (path, e))
    except IOError as e:
        raise ConfigError("cannot read %s: %s" % (path, e))
    if not isinstance(data.get("boards"), dict) or not data["boards"]:
        raise ConfigError("%s has no 'boards' map" % path)
    return Config(path, data)


def load_board(pcbnew, path):
    """pcbnew.LoadBoard, but a missing or unreadable board says so.

    pcbnew.LoadBoard returns None for a path that is not there, and the caller
    then fails several lines later on `NoneType has no attribute GetFootprints`.
    That is a poor way to learn a filename is wrong, and it is the common case in
    a repository whose boards do not exist yet.
    """
    import os as _os
    if not _os.path.isfile(path):
        raise SystemExit("no such board: %s" % path)
    b = pcbnew.LoadBoard(path)
    if b is None:
        raise SystemExit("could not read board: %s" % path)
    return b
