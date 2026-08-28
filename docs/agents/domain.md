# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`CONTEXT.md`** at the repo root, or
- **`CONTEXT-MAP.md`** at the repo root if it exists: it points at one `CONTEXT.md` per context. Read each one relevant to the topic.
- **`docs/adr/`**: read ADRs that touch the area you're about to work in.

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest
creating them upfront. The `/domain-modeling` skill (reached via `/grill-with-docs` and
`/improve-codebase-architecture`) creates them lazily when terms or decisions actually get resolved.

## File structure

This repo is **single-context**:

```
/
├── CLAUDE.md
├── CONTEXT.md                         ← the glossary, written lazily
├── docs/
│   ├── adr/                           ← decisions, written lazily
│   └── agents/                        ← this configuration
├── include/pedal_core/                ← the public headers products compile against
├── src/                               ← the implementations
└── test/                              ← host-native suites, one per module
```

Neither the glossary nor any ADR exists yet, which is the expected state: both are written the
day a term or a decision actually resolves.

## Borrow the multi-effect's vocabulary before inventing one

This library has no glossary of its own yet, and the words for most of what it holds are already
settled in the product that drove them. Before naming a concept here, check
[`CONTEXT.md`](https://github.com/DarrenInwood/killroom-analog-multi-effect/blob/main/CONTEXT.md)
in `DarrenInwood/killroom-analog-multi-effect` and use its term.

A term earns a place in a `CONTEXT.md` **here** when it is genuinely the library's own — a
concept every product shares and none of them owns. A term that only makes sense for one product
belongs in that product's glossary, not this one.

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a
test name), use the term as defined in `CONTEXT.md`. Don't drift to synonyms the glossary
explicitly avoids.

If the concept you need isn't in the glossary yet, that's a signal: either you're inventing
language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently overriding:

> _Contradicts ADR-0007 (event-sourced orders), but worth reopening because…_
