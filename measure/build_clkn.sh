#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The measurement core clkn (measure/clkn_core.c) as a loadable module, and its self test. Studio branch only.
#
#   measure/build_clkn.sh            build build-clkn/clkn.fm1core (macOS: a Mach-O bundle exporting only
#                                           fm1core_get, ad-hoc signed, as cmake/FM1Cores.cmake makes the other cores;
#                                           Linux: a shared object, to try it out)
#   measure/build_clkn.sh selftest   build and run clkn_selftest (a Device, no JUCE, any host rate): the bursts
#                                           each message makes, at the right frequency and no earlier than the message
#   measure/build_clkn.sh install    (macOS) build, then copy to ~/Library/Application Support/fm1emu/cores/
#
# A new file only: no CMakeLists.txt change, so merging upstream into this branch cannot conflict with it.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
out="$root/build-clkn"
mkdir -p "$out"
cmd="${1:-build}"
os="$(uname -s)"

build_core() {
    if [ "$os" = Darwin ]; then
        cc -std=c11 -O2 -fvisibility=hidden -bundle -Wl,-exported_symbol,_fm1core_get -I"$root/core-api" \
            -o "$out/clkn.fm1core" "$here/clkn_core.c" -lm
        codesign --force -s - "$out/clkn.fm1core"
    else
        cc -std=c11 -O2 -fPIC -fvisibility=hidden -shared -I"$root/core-api" -o "$out/clkn.fm1core" "$here/clkn_core.c" -lm
    fi
    echo "built $out/clkn.fm1core"
}

case "$cmd" in
build) build_core ;;
selftest)
    cc -std=c11 -O1 -fPIC -fvisibility=hidden -Wall -Wextra -I"$root/core-api" -c "$here/clkn_core.c" -o "$out/clkn_core.o"
    c++ -std=c++17 -O1 -Wall -Wextra -I"$root/engine" -I"$root/core-api" "$here/clkn_selftest.cpp" "$root/engine/device.cpp" \
        "$out/clkn_core.o" -o "$out/clkn_selftest" -lm
    "$out/clkn_selftest"
    ;;
install)
    [ "$os" = Darwin ] || { echo "install: macOS only" >&2; exit 1; }
    build_core
    dst="$HOME/Library/Application Support/fm1emu/cores"
    mkdir -p "$dst"
    cp "$out/clkn.fm1core" "$dst/clkn.fm1core"
    echo "installed $dst/clkn.fm1core"
    ;;
*) echo "usage: $0 [build|selftest|install]" >&2; exit 2 ;;
esac
