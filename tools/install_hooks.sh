#!/usr/bin/env bash
# install_hooks.sh — point this clone's git at the hooks committed in .githooks/.
#
# THIS ONE IS NOT A SHARED ENGINE. Every other script in tools/ is driven by a
# product through PROJECT_DIR and must not resolve anything from its own location,
# because this directory lives inside a submodule. This script is the exception: it
# configures the repository it is run in, and the hooks it installs are pedal-core's
# own. A product wanting hooks commits its own .githooks/ and runs the same one-liner.
#
# .git/hooks is not versioned, so a hook that is only there is a hook that exists on
# one machine and nowhere else. core.hooksPath points git at a directory that IS
# versioned, which is what makes the gate travel with the repo.
#
# Idempotent. Run it after cloning:
#
#     bash tools/install_hooks.sh
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

[ -d .githooks ] || { echo "install_hooks: no .githooks/ in $ROOT" >&2; exit 1; }

current="$(git config --get core.hooksPath || true)"
if [ "$current" = ".githooks" ]; then
    echo "install_hooks: already pointing at .githooks"
else
    git config core.hooksPath .githooks
    echo "install_hooks: core.hooksPath = .githooks"
fi

# The executable bit matters on Linux and macOS; on Windows git runs the hook through
# sh regardless, and the bit is carried in the index rather than the filesystem.
for h in .githooks/*; do
    [ -f "$h" ] || continue
    chmod +x "$h" 2>/dev/null || true
done

echo "install_hooks: pre-push will run the full gate. Bypass one push with --no-verify."
