#!/bin/bash
set -euo pipefail
x87_nested_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_nested_root/tmp/"
for x87_nested_bits in 32 64; do
    if [[ "$x87_nested_bits" == 32 ]]; then x87_nested_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_nested_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_nested_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_nested_root/bin/nested-exception-$x87_nested_bits.exe" \
        "$x87_nested_root/src/nested_exception_probe.c"
done
/usr/bin/shasum -a 256 "$x87_nested_root/src/nested_exception_probe.c" \
    "$x87_nested_root/bin/nested-exception-32.exe" "$x87_nested_root/bin/nested-exception-64.exe"
