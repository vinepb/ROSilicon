#!/bin/bash
set -euo pipefail
x87_rep_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_rep_root/tmp/"
/opt/homebrew/bin/i686-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
    -Wl,--no-insert-timestamp -o "$x87_rep_root/bin/rep-active-page-32.exe" \
    "$x87_rep_root/src/rep_active_page_probe.c"
/usr/bin/shasum -a 256 "$x87_rep_root/src/rep_active_page_probe.c" "$x87_rep_root/bin/rep-active-page-32.exe"
