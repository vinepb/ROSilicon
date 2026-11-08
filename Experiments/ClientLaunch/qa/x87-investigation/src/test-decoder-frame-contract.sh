#!/bin/bash
set -euo pipefail
x87_frame_root="$(cd "$(dirname "$0")/.." && pwd)"
case "${1:-passthrough}" in
    passthrough) x87_frame_tag="decoder-frame-contract"; x87_frame_flags=(-UFRAME_CHAIN_OPTIONS); x87_frame_binary="$x87_frame_root/bin/decoder-frame-contract" ;;
    frame-chain) x87_frame_tag="decoder-frame-helper"; x87_frame_flags=(-DFRAME_CHAIN_OPTIONS); x87_frame_binary="$x87_frame_root/bin/decoder-frame-contract-frame-chain" ;;
    *) echo 'usage: test-decoder-frame-contract.sh [passthrough|frame-chain] [unique-run-tag]' >&2; exit 2 ;;
esac
[[ $# -le 2 && "${2:-$x87_frame_tag}" =~ ^[A-Za-z0-9-]+$ ]] || exit 2
x87_frame_run="$x87_frame_root/runs/${2:-$x87_frame_tag}"
mkdir "$x87_frame_run"
export TMPDIR="$x87_frame_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_frame_root/tmp/clang-module-cache"
x87_frame_source="$x87_frame_root/source-${1:-passthrough}"
/usr/bin/clang++ -arch arm64 -std=c++23 -O2 -fno-omit-frame-pointer -mmacosx-version-min=14.0 \
    "${x87_frame_flags[@]}" \
    -I "$x87_frame_source/rosetta_loader/src" -I "$x87_frame_source/rosetta_core/include" \
    "$x87_frame_root/src/test_decoder_frame_contract.cpp" \
    "$x87_frame_root/src/test_decoder_frame_contract_arm64.S" \
    "$x87_frame_source/rosetta_loader/src/stub_asm.cpp" \
    -o "$x87_frame_binary" > "$x87_frame_run/build.log" 2>&1
/usr/bin/codesign --force --sign - "$x87_frame_binary"
/usr/bin/codesign --verify --strict "$x87_frame_binary"
/usr/bin/shasum -a 256 "$x87_frame_binary" \
    "$x87_frame_root/src/test_decoder_frame_contract.cpp" \
    "$x87_frame_root/src/test_decoder_frame_contract_arm64.S" \
    "$x87_frame_source/rosetta_loader/src/stub_asm.cpp" > "$x87_frame_run/input-hashes.txt"
"$x87_frame_binary" | /usr/bin/tee "$x87_frame_run/results.log"
