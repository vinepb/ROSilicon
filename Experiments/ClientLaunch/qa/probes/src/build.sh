#!/bin/bash
set -euo pipefail
probe_root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$probe_root/bin" "$probe_root/tmp" "$probe_root/tmp/clang-module-cache"
export TMPDIR="$probe_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$probe_root/tmp/clang-module-cache"
for bits in 32 64; do
    if [[ "$bits" == 32 ]]; then probe_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else probe_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$probe_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$probe_root/bin/probe-$bits.exe" \
        "$probe_root/src/breakpoint_probe.c"
done
/usr/bin/clang -arch x86_64 -mmacosx-version-min=14.0 -std=c11 -O2 \
    -Wall -Wextra -Werror -o "$probe_root/bin/probe-macos-x86_64" \
    "$probe_root/src/breakpoint_probe.c"
/usr/bin/file "$probe_root"/bin/probe-*
/usr/bin/shasum -a 256 "$probe_root/src/breakpoint_probe.c" "$probe_root"/bin/probe-*
