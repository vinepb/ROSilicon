#!/bin/bash
set -euo pipefail
x87_test_root="$(cd "$(dirname "$0")/.." && pwd)"
x87_source="$x87_test_root/source-v1.7.0"
mkdir -p "$x87_test_root/bin/upstream" "$x87_test_root/tmp/clang-module-cache"
export TMPDIR="$x87_test_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_test_root/tmp/clang-module-cache"
# Use upstream's exact sample compilation and voluntary handshake shim.
# No CMake/tool installation is required to build these standalone samples.
for sample in test_x87_signal_storm test_x87_signal_context test_x87_native_state test_x87_trace_threads; do
    x87_link_flags=(-mmacosx-version-min=14.0)
    if [[ "$sample" == test_x87_signal_context ]]; then
        x87_link_flags+=(-Wl,-pagezero_size,0x4000)
    fi
    /usr/bin/clang -arch x86_64 -O2 "${x87_link_flags[@]}" \
        -I "$x87_source/rosetta_loader/src" -o "$x87_test_root/bin/upstream/$sample" \
        "$x87_source/tests/$sample.c" "$x87_source/tests/coop_handshake.c"
done
/usr/bin/shasum -a 256 "$x87_test_root"/bin/upstream/*
