#!/bin/bash
set -euo pipefail
x87_cold_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_cold_root/tmp/"
for x87_cold_bits in 32 64; do
    if [[ "$x87_cold_bits" == 32 ]]; then x87_cold_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_cold_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_cold_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_cold_root/bin/cold-block-$x87_cold_bits.exe" \
        "$x87_cold_root/src/cold_block_probe.c"
done
/usr/bin/shasum -a 256 "$x87_cold_root/src/cold_block_probe.c" "$x87_cold_root"/bin/cold-block-*.exe
