#!/bin/bash
set -euo pipefail
x87_fetch_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_fetch_root/tmp/"
for x87_fetch_bits in 32 64; do
    if [[ "$x87_fetch_bits" == 32 ]]; then x87_fetch_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_fetch_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_fetch_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_fetch_root/bin/fetch-boundary-$x87_fetch_bits.exe" \
        "$x87_fetch_root/src/fetch_boundary_probe.c"
done
/usr/bin/shasum -a 256 "$x87_fetch_root/src/fetch_boundary_probe.c" \
    "$x87_fetch_root/bin/fetch-boundary-32.exe" "$x87_fetch_root/bin/fetch-boundary-64.exe"
