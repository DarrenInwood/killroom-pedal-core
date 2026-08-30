# pedal-core

Shared firmware core for a family of guitar pedals. [README.md](README.md) lists every module and
what it is; read it before changing anything here, and don't duplicate it into this file.

## The one rule that shapes everything

**This repo is nobody's application.** Every product consumes it as a git submodule pinned to a
commit, so a change here reaches a product only when that product re-pins. Two things follow:

- **Additive beats breaking.** A new function on an existing class costs consumers nothing. A
  changed signature or changed behaviour costs every product a co-ordinated bump. Prefer the
  former; when the latter is genuinely right, say which products it affects in the issue.
- **Land here first.** Merge and push in this repo, then bump the submodule in the product, in
  the same commit as the code that needs it. A product PR that moves the pin cannot merge before
  the commit it points at exists on `origin`.

## Build & test

Host-native suites, one per module, are the whole test story — there is no hardware in the loop:

```
pio test -e native
```

The `native` env names its sources explicitly in `build_src_filter` ([platformio.ini](platformio.ini));
a new `.cpp` that a suite needs has to be added there, or linked by donor-`#include` from the suite.

**Run this once per clone:**

```
bash tools/install_hooks.sh
```

It points `core.hooksPath` at [.githooks/](.githooks/), whose `pre-push` runs the whole gate —
the native suites, every `tools/**/test_*.py`, the doc-link check and
[tools/check_warnings.sh](tools/check_warnings.sh) — before anything leaves the machine. The
board tests need `pcbnew`, and the warning sweep's ARM leg needs an ARM toolchain; both are
skipped, loudly, where those are absent, and everything else still runs.
`git push --no-verify` bypasses the lot.

**A product names the parts of the library it wants.** Every consumer supplies a
`pedal_core_features.hpp` on the include path, switching each optional domain on or off:

```cpp
#define PEDAL_CORE_HAS_TEMPO    1
#define PEDAL_CORE_HAS_EXTINPUT 0
```

The modules of a domain that is off compile to nothing; the headers of one refuse to be
included at all, naming the switch. A switch left undefined is an error rather than a
preprocessor zero, because a module silently producing no symbols surfaces as a link failure
somewhere unrelated. [test/support/pedal_core_features.hpp](test/support/pedal_core_features.hpp)
is the reference, and a domain switched on obliges its config header too.

Both sides are built: `pio test -e native` runs the suites with every domain on, and
`pio test -e native_minimal` builds the library with all of them off. The pre-push hook runs
both. See [docs/adr/0003-a-product-names-the-domains-it-wants.md](docs/adr/0003-a-product-names-the-domains-it-wants.md).

## Writing style for comments & docs

Comments and docs describe the library **as it is now**: state the present-tense fact and the
reason it holds, rather than narrating the change that produced it. Avoid "previously", "used
to", "no longer", "was X, now Y". Git commit messages, PR descriptions and ADRs are the
carve-outs, since history is their subject there.

## Agent skills

The [mattpocock engineering skills](https://github.com/mattpocock/skills) learn how this repo
works from three files under [docs/agents/](docs/agents/) — `issue-tracker.md`, `domain.md` and
`triage-labels.md`. Edit those files to change what the skills do; the skills themselves are
identical in every repo.

### Issue tracker

Issues and specs are GitHub issues in `DarrenInwood/killroom-pedal-core`, driven through the `gh`
CLI. Work that arrives from a product repo is cross-referenced in both directions, and the
multi-effect's autonomous loop does not sweep this repo. See
[docs/agents/issue-tracker.md](docs/agents/issue-tracker.md).

### Triage labels

The five canonical roles, each label string equal to its name: `needs-triage`, `needs-info`,
`ready-for-agent`, `ready-for-human`, `wontfix`. One further axis names the part of the library:
`area:tempo` … `area:platform`. See [docs/agents/triage-labels.md](docs/agents/triage-labels.md).

### Domain docs

Single-context: the [CONTEXT.md](CONTEXT.md) glossary and [docs/adr/](docs/adr/) at the root,
both written lazily as terms and decisions resolve. Terms the products already settled are
borrowed rather than reinvented. See [docs/agents/domain.md](docs/agents/domain.md).
