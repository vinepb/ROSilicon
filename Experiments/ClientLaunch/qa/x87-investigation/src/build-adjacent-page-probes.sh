#!/bin/bash
set -euo pipefail
x87_adjacent_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_adjacent_root/tmp/"
for x87_adjacent_bits in 32 64; do
    if [[ "$x87_adjacent_bits" == 32 ]]; then x87_adjacent_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_adjacent_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_adjacent_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_adjacent_root/bin/adjacent-page-$x87_adjacent_bits.exe" \
        "$x87_adjacent_root/src/adjacent_page_probe.c"
done
/usr/bin/shasum -a 256 "$x87_adjacent_root/src/adjacent_page_probe.c" \
    "$x87_adjacent_root/bin/adjacent-page-32.exe" "$x87_adjacent_root/bin/adjacent-page-64.exe"
