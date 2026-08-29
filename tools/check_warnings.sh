#!/usr/bin/env bash
# check_warnings.sh — the library compiles clean, so a consumer can turn -Werror on.
#
# Every product inherits this library's warnings and cannot fix them from its side, so one
# that accumulates here costs each consumer a suppression. The consumers scope -Werror away
# from the library's sources for exactly that reason.
#
# TWO COMPILERS, BECAUSE NEITHER SEES EVERYTHING
#
# The products build with GNU Arm Embedded 7.2.1 (2017). -Wstringop-truncation did not exist
# until GCC 8, so that toolchain cannot see a whole family of warnings a modern host reports
# -- and GCC 7's -Wformat-truncation is far more eager than a modern one's, so it reports
# things the host does not. Checking one compiler leaves the other's family free to
# accumulate unseen.
#
# BOTH NEED THE OPTIMISER. The truncation warnings come out of value-range propagation, so
# at -O0 they cannot be computed at all and the build is silent whatever the code says. The
# native test env builds at -O0, so `pio test -e native` says nothing about any of this.
#
# The ARM leg is skipped, loudly, where no arm-none-eabi-g++ is installed: it is the
# consumers' toolchain, not a requirement for working on the library. CI has none, so the
# pre-push hook is what covers that family on a machine that builds products.
#
# THIS ONE IS NOT A SHARED ENGINE. Every other script in tools/ is driven by a product
# through PROJECT_DIR and must not resolve anything from its own location, because this
# directory lives inside a submodule. This script compiles THIS library and is this
# repository's own, so it finds the repository the way tools/install_hooks.sh does.
#
#     bash tools/check_warnings.sh
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel)" || exit 1
cd "$ROOT" || exit 1

say() { printf '[check-warnings] %s\n' "$*" >&2; }

SOURCES=(src/*.cpp src/ui/*.cpp)
FAILED=0

# A configuration the reference one does not exercise. The EEPROM driver picks its mirror
# span differently when a product mirrors its whole store, and the branch not taken under
# the reference config is exactly where a dead comparison can hide. Generated rather than
# committed, because it is one line different from the real one.
#
# The WHOLE config set is copied and one line patched, and the sweep uses it INSTEAD of
# test/support rather than ahead of it. Shadowing a single header does not work: the UI
# config includes the parameter config by quoted path, which resolves beside itself, so a
# shadowed copy earlier on the include path is a second definition of every constant
# rather than a replacement for one.
PROBE="$(mktemp -d)"
trap 'rm -rf "$PROBE"' EXIT
cp test/support/*.hpp "$PROBE/"
sed -i 's/EEPROM_MIRROR_TAIL_BASE  = 0x7A00u/EEPROM_MIRROR_TAIL_BASE  = 0u/' \
    "$PROBE/pedal_core_config.hpp"

# Run one compiler over one include path set, and report anything it says.
#
# -c to a throwaway object rather than -fsyntax-only: the analysis these warnings come from
# runs in the optimiser, and -fsyntax-only stops before it.
sweep() {
    local label="$1" cxx="$2"; shift 2
    # -c with several inputs and one -o is refused, so compile them one at a time.
    local out="" rc=0 f one
    for f in "${SOURCES[@]}"; do
        one="$("$cxx" "$@" -c "$f" -o "$PROBE/$(basename "$f").o" 2>&1)" || rc=1
        out+="$one"
    done
    # A source that does not compile is reported as itself rather than as a clean sweep:
    # a gate that passes a build which never built is worse than no gate.
    if [ "$rc" != "0" ]; then
        say "FAILED  $label — a source did not compile"
        printf '%s\n' "$out" | sed 's/^/          /' >&2
        FAILED=1
    elif printf '%s' "$out" | grep -q 'warning:'; then
        say "FAILED  $label"
        printf '%s\n' "$out" | grep -E 'warning:|^ ' | sed 's/^/          /' >&2
        FAILED=1
    else
        say "clean   $label"
    fi
}

# --- the host, at -O2 --------------------------------------------------------
HOST_CXX="${CXX:-g++}"
if command -v "$HOST_CXX" >/dev/null 2>&1; then
    sweep "host $("$HOST_CXX" -dumpversion), -O2" "$HOST_CXX" \
        -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/support
    sweep "host, -O2, whole-store mirror" "$HOST_CXX" \
        -std=c++17 -O2 -Wall -Wextra -Iinclude -I"$PROBE"
else
    say "SKIPPED host — no $HOST_CXX on PATH"
fi

# --- the consumers' toolchain, at -Os ----------------------------------------
find_arm() {
    if command -v arm-none-eabi-g++ >/dev/null 2>&1; then command -v arm-none-eabi-g++; return 0; fi
    local c
    for c in "$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-g++"* ; do
        [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}

ARM_CXX="$(find_arm || true)"
if [ -n "${ARM_CXX:-}" ]; then
    ARM_FLAGS=(-std=c++17 -Os -Wall -Wextra -mcpu=cortex-m4 -mthumb)
    sweep "arm $("$ARM_CXX" -dumpversion), -Os" "$ARM_CXX" \
        "${ARM_FLAGS[@]}" -Iinclude -Itest/support
    sweep "arm, -Os, whole-store mirror" "$ARM_CXX" \
        "${ARM_FLAGS[@]}" -Iinclude -I"$PROBE"
else
    say "SKIPPED arm — no arm-none-eabi-g++ found."
    say "        It is the consumers' toolchain; PlatformIO puts one under"
    say "        ~/.platformio/packages/toolchain-gccarmnoneeabi when a product is built."
fi

if [ "$FAILED" != "0" ]; then
    say ""
    say "The library does not compile clean. A consumer inherits every line above and"
    say "cannot fix it from its side."
    exit 1
fi

say "check_warnings: PASS"
exit 0
