#!/usr/bin/env bash
# setup_build_env.sh — the family's idempotent PlatformIO bootstrap.
#
# Runs on a fresh Linux CI runner or sandbox AND on local Windows (Git Bash). Safe to run
# every session: it locates an existing PlatformIO, installs it only if missing, then
# pre-fetches the package set for the environments the product names so the subsequent
# `pio run` / `pio test` build-gate is fast. The last line prints `PIO=<path>` for the caller.
#
# The product supplies both inputs, because the engine must not guess where it is being
# used from — this file lives in a submodule, so resolving anything from its own location
# would point inside the library rather than at the firmware being built:
#
#   PROJECT_DIR   absolute path to the PlatformIO project (the dir holding platformio.ini)
#   PIO_ENVS      space-separated env names to pre-fetch, in build order
#
# A product wraps it:
#
#   export PROJECT_DIR="$ROOT/firmware"
#   export PIO_ENVS="dev release native"
#   exec bash "$ROOT/firmware/lib/pedal-core/tools/setup_build_env.sh"
set -euo pipefail

: "${PROJECT_DIR:?setup_build_env: PROJECT_DIR is not set — the wrapper must export it}"
: "${PIO_ENVS:?setup_build_env: PIO_ENVS is not set — the wrapper must export it}"

log() { printf '[setup-build-env] %s\n' "$*" >&2; }

[ -d "$PROJECT_DIR" ] || { log "ERROR: PROJECT_DIR does not exist: $PROJECT_DIR"; exit 1; }

# Disable PlatformIO telemetry so collector.platformio.org is never contacted — one fewer
# host a sandbox egress allowlist has to permit.
export PLATFORMIO_SETTING_ENABLE_TELEMETRY=false

# 1. Locate pio: PATH first, then the standard penv install locations (Linux, then Windows).
find_pio() {
  if command -v pio >/dev/null 2>&1; then command -v pio; return 0; fi
  local candidates=(
    "$HOME/.platformio/penv/bin/pio"
    "${USERPROFILE:-}/.platformio/penv/Scripts/pio.exe"
    "$HOME/.platformio/penv/Scripts/pio.exe"
  )
  local c
  for c in "${candidates[@]}"; do
    [ -n "$c" ] && [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }
  done
  return 1
}

PIO="$(find_pio || true)"

# 2. Install PlatformIO only if no existing binary was found.
if [ -z "${PIO:-}" ]; then
  log "PlatformIO not found — installing via pip…"
  if command -v python  >/dev/null 2>&1; then PYBIN=python
  elif command -v python3 >/dev/null 2>&1; then PYBIN=python3
  else log "ERROR: no python/python3 on PATH; cannot install PlatformIO"; exit 1
  fi
  "$PYBIN" -m pip install --quiet --upgrade platformio
  PIO="$(find_pio || true)"
  [ -n "${PIO:-}" ] || { log "ERROR: PlatformIO still not found after install"; exit 1; }
else
  log "Found existing PlatformIO: $PIO"
fi

# 3. Pre-fetch packages for the named envs (no-op once warm). Run from the project dir.
#    A native test env installs the PlatformIO native platform + Unity here too.
cd "$PROJECT_DIR"
for env in $PIO_ENVS; do
  log "Pre-fetching packages for env:$env…"
  if ! "$PIO" pkg install -e "$env"; then
    log "ERROR: 'pio pkg install -e $env' failed."
    log "If this is a sandbox egress block (e.g. 'Host not in allowlist: api.registry.platformio.org'),"
    log "allow these hosts in the network egress settings — or set egress mode to 'All domains':"
    log "    *.platformio.org   github.com   *.githubusercontent.com"
    exit 1
  fi
done

log "Build environment ready."
# 4. Machine-readable handoff line (always last).
printf 'PIO=%s\n' "$PIO"
