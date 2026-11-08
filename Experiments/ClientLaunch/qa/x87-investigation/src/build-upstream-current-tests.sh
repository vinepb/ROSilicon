#!/bin/bash
set -euo pipefail
x87_current_root="$(cd "$(dirname "$0")/.." && pwd)"
x87_current_source="$x87_current_root/source-upstream-2026-10-12"
mkdir -p "$x87_current_root/bin/upstream-current" "$x87_current_root/tmp/clang-module-cache"
export TMPDIR="$x87_current_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_current_root/tmp/clang-module-cache"
# Reuse the pinned upstream samples and voluntary handshake exactly. No
# privileged attach variant, dependency installation or game executable.
for x87_current_sample in test_fist_pressure test_fild_fistp_copy test_m80_copy \
    test_flags_handoff test_delphi_move test_x87_signal_storm \
    test_x87_signal_context test_x87_native_state test_x87_trace_threads \
    test_x87_sync_fault; do
    x87_current_flags=(-mmacosx-version-min=14.0)
    case "$x87_current_sample" in
        test_delphi_move|test_x87_signal_context) x87_current_flags+=(-Wl,-pagezero_size,0x4000) ;;
        test_x87_sync_fault) x87_current_flags+=(-Wl,-pagezero_size,0x4000 -framework IOSurface -framework CoreFoundation) ;;
    esac
    /usr/bin/clang -arch x86_64 -O2 "${x87_current_flags[@]}" \
        -I "$x87_current_source/rosetta_loader/src" \
        "$x87_current_source/tests/$x87_current_sample.c" \
        "$x87_current_source/tests/coop_handshake.c" \
        -o "$x87_current_root/bin/upstream-current/$x87_current_sample"
done
/usr/bin/shasum -a 256 "$x87_current_root"/bin/upstream-current/*
