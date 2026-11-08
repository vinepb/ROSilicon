#!/bin/bash
set -euo pipefail
x87_probe_root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$x87_probe_root/bin" "$x87_probe_root/tmp/clang-module-cache"
export TMPDIR="$x87_probe_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_probe_root/tmp/clang-module-cache"
for bits in 32 64; do
    if [[ "$bits" == 32 ]]; then x87_probe_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_probe_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_probe_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_probe_root/bin/x87-breakpoint-$bits.exe" \
        "$x87_probe_root/src/x87_breakpoint_probe.c"
done
x87_source="$x87_probe_root/source-v1.7.0"
/usr/bin/clang -arch x86_64 -mmacosx-version-min=14.0 -std=c11 -O2 \
    -Wall -Wextra -Werror -I "$x87_source/rosetta_loader/src" \
    "$x87_probe_root/src/x87_breakpoint_probe.c" "$x87_source/tests/coop_handshake.c" \
    -o "$x87_probe_root/bin/x87-breakpoint-macos"
/usr/bin/file "$x87_probe_root"/bin/x87-breakpoint-*.exe
/usr/bin/shasum -a 256 "$x87_probe_root/src/x87_breakpoint_probe.c" "$x87_probe_root"/bin/x87-breakpoint-*.exe
