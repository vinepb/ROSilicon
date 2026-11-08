#!/bin/bash
set -euo pipefail
x87_jump_root="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -gt 2 || ! "${1:-decoder-near-registers}" =~ ^[A-Za-z0-9-]+$ ]]; then
    echo 'usage: test-decoder-jumps.sh [unique-run-tag] [near-branch|near-tail]' >&2; exit 2
fi
x87_jump_run="$x87_jump_root/runs/${1:-decoder-near-registers}"
mkdir "$x87_jump_run"
export TMPDIR="$x87_jump_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_jump_root/tmp/clang-module-cache"
case "${2:-near-branch}" in
    near-branch) x87_jump_flags=(); x87_jump_binary="$x87_jump_root/bin/decoder-jump-registers" ;;
    near-tail) x87_jump_flags=(-DNEAR_TAIL_OPTIONS); x87_jump_binary="$x87_jump_root/bin/decoder-jump-registers-near-tail" ;;
    *) echo 'unknown owned jump-test variant' >&2; exit 2 ;;
esac
x87_jump_source="$x87_jump_root/source-${2:-near-branch}"
/usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
    "${x87_jump_flags[@]}" \
    -I "$x87_jump_source/rosetta_loader/src" -I "$x87_jump_source/rosetta_core/include" \
    "$x87_jump_root/src/test_decoder_jumps.cpp" \
    "$x87_jump_root/src/test_decoder_jumps_arm64.S" \
    "$x87_jump_source/rosetta_loader/src/stub_asm.cpp" \
    -o "$x87_jump_binary" > "$x87_jump_run/build.log" 2>&1
/usr/bin/codesign --force --sign - "$x87_jump_binary"
/usr/bin/codesign --verify --strict "$x87_jump_binary"
/usr/bin/shasum -a 256 "$x87_jump_binary" \
    "$x87_jump_root/src/test_decoder_jumps.cpp" "$x87_jump_root/src/test_decoder_jumps_arm64.S" \
    > "$x87_jump_run/input-hashes.txt"
"$x87_jump_binary" | /usr/bin/tee "$x87_jump_run/results.log"
