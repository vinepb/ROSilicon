#!/bin/bash
set -euo pipefail
x87_focused_root="$(cd "$(dirname "$0")/.." && pwd)"
x87_focused_source="$x87_focused_root/source-upstream-2026-10-26"
x87_focused_build="$x87_focused_root/build/helper-upstream-oct26"
export TMPDIR="$x87_focused_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_focused_root/tmp/clang-module-cache"
mkdir -p "$x87_focused_root/bin/upstream-oct26"
for x87_focused_sample in test_x87_cross_reply_exact test_x87_signal_context \
    test_decoder_arpl test_decoder_fcomp_st; do
    x87_focused_flags=(-mmacosx-version-min=14.0)
    case "$x87_focused_sample" in
        test_x87_signal_context|test_decoder_arpl|test_decoder_fcomp_st)
            x87_focused_flags+=(-Wl,-pagezero_size,0x4000) ;;
    esac
    /usr/bin/clang -arch x86_64 -O2 "${x87_focused_flags[@]}" \
        -I "$x87_focused_source/rosetta_loader/src" \
        "$x87_focused_source/tests/$x87_focused_sample.c" \
        "$x87_focused_source/tests/coop_handshake.c" \
        -o "$x87_focused_root/bin/upstream-oct26/$x87_focused_sample"
done
/usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
    -I "$x87_focused_source/rosetta_core/include" \
    "$x87_focused_source/tests/host/test_opcode_host_table.cpp" \
    "$x87_focused_build"/objects/rosetta_core-*.o \
    -o "$x87_focused_root/bin/upstream-oct26/test_opcode_host_table"
/usr/bin/shasum -a 256 "$x87_focused_root"/bin/upstream-oct26/*
