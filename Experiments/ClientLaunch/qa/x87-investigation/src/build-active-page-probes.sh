#!/bin/bash
set -euo pipefail
x87_active_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_active_root/tmp/"
for x87_active_bits in 32 64; do
    if [[ "$x87_active_bits" == 32 ]]; then x87_active_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_active_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_active_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_active_root/bin/active-page-$x87_active_bits.exe" \
        "$x87_active_root/src/active_page_probe.c"
done
/usr/bin/shasum -a 256 "$x87_active_root/src/active_page_probe.c" \
    "$x87_active_root/bin/active-page-32.exe" "$x87_active_root/bin/active-page-64.exe"
