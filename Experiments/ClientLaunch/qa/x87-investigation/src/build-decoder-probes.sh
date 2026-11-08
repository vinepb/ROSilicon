#!/bin/bash
set -euo pipefail
x87_probe_root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$x87_probe_root/bin/decoder-upstream" "$x87_probe_root/tmp/clang-module-cache"
export TMPDIR="$x87_probe_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_probe_root/tmp/clang-module-cache"
for bits in 32 64; do
    if [[ "$bits" == 32 ]]; then x87_probe_cc=/opt/homebrew/bin/i686-w64-mingw32-gcc;
    else x87_probe_cc=/opt/homebrew/bin/x86_64-w64-mingw32-gcc; fi
    "$x87_probe_cc" -std=c11 -O2 -Wall -Wextra -Werror -static-libgcc \
        -Wl,--no-insert-timestamp -o "$x87_probe_root/bin/decoder-$bits.exe" \
        "$x87_probe_root/src/decoder_probe.c"
done
x87_source="$x87_probe_root/source-v1.7.0"
for sample in test_decoder_arpl test_decoder_fcomp_st; do
    /usr/bin/clang -arch x86_64 -O2 -mmacosx-version-min=14.0 -Wl,-pagezero_size,0x4000 \
        -I "$x87_source/rosetta_loader/src" \
        "$x87_source/tests/$sample.c" "$x87_source/tests/coop_handshake.c" \
        -o "$x87_probe_root/bin/decoder-upstream/$sample"
done
/usr/bin/clang -arch x86_64 -O2 -mmacosx-version-min=14.0 -Wl,-pagezero_size,0x4000 \
    -I "$x87_source/rosetta_loader/src" \
    "$x87_probe_root/src/decoder_signal_probe.c" "$x87_source/tests/coop_handshake.c" \
    -o "$x87_probe_root/bin/decoder-signal-x86_64"
/usr/bin/shasum -a 256 "$x87_probe_root/src/decoder_probe.c" \
    "$x87_probe_root"/bin/decoder-*.exe "$x87_probe_root"/bin/decoder-upstream/* \
    "$x87_probe_root/bin/decoder-signal-x86_64"
