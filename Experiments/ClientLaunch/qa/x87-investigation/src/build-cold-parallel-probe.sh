#!/bin/bash
set -euo pipefail
x87_cold_root="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$x87_cold_root/tmp/"
/opt/homebrew/bin/i686-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
    -Wl,--no-insert-timestamp -o "$x87_cold_root/bin/cold-parallel-32.exe" "$x87_cold_root/src/cold_parallel_probe.c"
/usr/bin/shasum -a 256 "$x87_cold_root/src/cold_parallel_probe.c" "$x87_cold_root/bin/cold-parallel-32.exe"
