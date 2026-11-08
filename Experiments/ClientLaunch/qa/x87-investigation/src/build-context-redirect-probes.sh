#!/bin/bash
set -euo pipefail
x87_redirect_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_redirect_root/tmp/"
for x87_redirect_bits in 32 64; do
    if [[ "$x87_redirect_bits" == 32 ]]; then x87_redirect_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_redirect_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_redirect_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_redirect_root/bin/context-redirect-$x87_redirect_bits.exe" \
        "$x87_redirect_root/src/context_redirect_probe.c"
done
/usr/bin/shasum -a 256 "$x87_redirect_root/src/context_redirect_probe.c" \
    "$x87_redirect_root/bin/context-redirect-32.exe" "$x87_redirect_root/bin/context-redirect-64.exe"
