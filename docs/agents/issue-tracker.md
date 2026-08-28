# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues in `DarrenInwood/killroom-pedal-core`.
Use the `gh` CLI for all operations.

## Conventions

- **Create an issue**: `gh issue create --title "..." --body "..."`. Use a heredoc for multi-line bodies.
- **Read an issue**: `gh issue view <number> --comments`, filtering comments by `jq` and also fetching labels.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --comment "..."`

Infer the repo from `git remote -v`; `gh` does this automatically when run inside a clone.

## This repo is a submodule of every pedal

`pedal-core` is the shared library the pedal family builds on. Each product repo consumes it as
a git submodule pinned to a commit, so **a change here reaches a product only when that product
re-pins**. Two consequences for anything filed in this tracker:

- **Say which products a change affects.** An addition to a shared header is additive for
  everyone; a change to an existing signature or behaviour is not. Name the products you know
  about in the issue.
- **Land here first, then re-pin.** The order is: merge in `pedal-core`, push, then bump the
  submodule in the product repo in the same commit as the code that needs it. A product PR that
  moves the pin cannot merge before the pedal-core commit exists on `origin`.

### Work that arrives from a product repo

The usual way an issue lands here is that someone building a product hits a gap in this library.
That is worth filing rather than working around: the gap is the finding, and working around it in
one product leaves it for the next.

Cross-reference in both directions, which GitHub renders natively:

- In the product issue: `Blocked by DarrenInwood/killroom-pedal-core#<n>`
- In the issue here: `Blocks DarrenInwood/killroom-analog-multi-effect#<n>`

A worked example is `TempoController::on_algo_change()`. The multi-effect wanted the tempo re-seed
to stop reaching past `TempoController` into `tap_tempo` and `tempo_led`, and no event here had
those semantics. The product's ticket said `TempoController` was out of scope, so the two
requirements could not both hold until the event existed here.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

When set to `yes`, PRs run through the same labels and states as issues, using the `gh pr` equivalents:

- **Read a PR**: `gh pr view <number> --comments` and `gh pr diff <number>` for the diff.
- **List external PRs for triage**: `gh pr list --state open --json number,title,body,labels,author,authorAssociation,comments` then keep only `authorAssociation` of `CONTRIBUTOR`, `FIRST_TIME_CONTRIBUTOR`, or `NONE` (drop `OWNER`/`MEMBER`/`COLLABORATOR`).
- **Comment / label / close**: `gh pr comment`, `gh pr edit --add-label`/`--remove-label`, `gh pr close`.

GitHub shares one number space across issues and PRs, so a bare `#42` may be either: resolve with `gh pr view 42` and fall back to `gh issue view 42`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --comments`.

## The autonomous loop does not run here

The multi-effect runs a scheduled improvement loop over its own repo. It does not sweep this
one: its areas are all under that repo's `firmware/src/`, its build-gate runs that repo's
PlatformIO envs, and it commits to a branch there. An issue in this tracker is worked by a
person, or by an agent someone starts deliberately.

## Wayfinding operations

Used by `/wayfinder`. The **map** is a single issue with **child** issues as tickets.

- **Map**: a single issue labelled `wayfinder:map`, holding the Notes / Decisions-so-far / Fog body. `gh issue create --label wayfinder:map`.
- **Child ticket**: an issue linked to the map as a GitHub sub-issue (`gh api` on the sub-issues endpoint). Where sub-issues aren't enabled, add the child to a task list in the map body and put `Part of #<map>` at the top of the child body. Labels: `wayfinder:<type>` (`research`/`prototype`/`grilling`/`task`). Once claimed, the ticket is assigned to the driving dev.
- **Blocking**: GitHub's **native issue dependencies**, the canonical, UI-visible representation. Add an edge with `gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`, where `<blocker-db-id>` is the blocker's numeric **database id** (`gh api repos/<owner>/<repo>/issues/<n> --jq .id`, _not_ the `#number` or `node_id`). GitHub reports `issue_dependencies_summary.blocked_by` (open blockers only, the live gate). Where dependencies aren't available, fall back to a `Blocked by: #<n>, #<n>` line at the top of the child body. A ticket is unblocked when every blocker is closed.
- **Frontier query**: list the map's open children (`gh issue list --state open`, scoped to the map's sub-issues / task list), drop any with an open blocker (`issue_dependencies_summary.blocked_by > 0`, or an open issue in the `Blocked by` line) or an assignee; first in map order wins.
- **Claim**: `gh issue edit <n> --add-assignee @me`, the session's first write.
- **Resolve**: `gh issue comment <n> --body "<answer>"`, then `gh issue close <n>`, then append a context pointer (gist + link) to the map's Decisions-so-far.
