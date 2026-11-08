#!/bin/bash
set -euo pipefail
x87_contract_root="$(cd "$(dirname "$0")/.." && pwd)"
x87_contract_run="$x87_contract_root/runs/decoder-passthrough-return-contract"
mkdir "$x87_contract_run"
export TMPDIR="$x87_contract_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_contract_root/tmp/clang-module-cache"
x87_contract_source="$x87_contract_root/source-passthrough"
x87_contract_binary="$x87_contract_root/bin/decoder-return-contract"
/usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
    -I "$x87_contract_source/rosetta_loader/src" -I "$x87_contract_source/rosetta_core/include" \
    "$x87_contract_root/src/test_decoder_return_contract.cpp" \
    "$x87_contract_source/rosetta_loader/src/stub_asm.cpp" \
    -o "$x87_contract_binary" > "$x87_contract_run/build.log" 2>&1
/usr/bin/codesign --force --sign - "$x87_contract_binary"
/usr/bin/codesign --verify --strict "$x87_contract_binary"
/usr/bin/shasum -a 256 "$x87_contract_binary" \
    "$x87_contract_root/src/test_decoder_return_contract.cpp" \
    "$x87_contract_source/rosetta_loader/src/stub_asm.cpp" > "$x87_contract_run/input-hashes.txt"
"$x87_contract_binary" | /usr/bin/tee "$x87_contract_run/results.log"
