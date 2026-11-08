#!/bin/bash
set -euo pipefail
x87_cold_signal_root="$(cd "$(dirname "$0")/.." && pwd)"
x87_cold_signal_source="$x87_cold_signal_root/source-upstream-2026-10-12"
export TMPDIR="$x87_cold_signal_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_cold_signal_root/tmp/clang-module-cache"
/usr/bin/clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -mmacosx-version-min=14.0 \
    -I "$x87_cold_signal_source/rosetta_loader/src" \
    "$x87_cold_signal_root/src/cold_signal_probe.c" \
    "$x87_cold_signal_source/tests/coop_handshake.c" \
    -o "$x87_cold_signal_root/bin/cold-signal-x86_64"
/usr/bin/codesign --force --sign - "$x87_cold_signal_root/bin/cold-signal-x86_64"
/usr/bin/codesign --verify --strict "$x87_cold_signal_root/bin/cold-signal-x86_64"
/usr/bin/clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -mmacosx-version-min=14.0 \
    "$x87_cold_signal_root/src/resolve_own_symbol.c" -o "$x87_cold_signal_root/bin/resolve-own-symbol-x86_64"
/usr/bin/shasum -a 256 "$x87_cold_signal_root/src/cold_signal_probe.c" \
    "$x87_cold_signal_source/tests/coop_handshake.c" "$x87_cold_signal_root/bin/cold-signal-x86_64" \
    "$x87_cold_signal_root/src/resolve_own_symbol.c" "$x87_cold_signal_root/bin/resolve-own-symbol-x86_64"
