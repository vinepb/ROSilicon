#!/bin/bash
set -euo pipefail
x87_blob_root="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -gt 1 || ! "${1:-decoder-blob-integrity-v2}" =~ ^[A-Za-z0-9-]+$ ]]; then
    echo 'usage: test-decoder-blobs.sh [unique-run-tag]' >&2; exit 2
fi
x87_blob_run="$x87_blob_root/runs/${1:-decoder-blob-integrity-v2}"
mkdir "$x87_blob_run"
export TMPDIR="$x87_blob_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_blob_root/tmp/clang-module-cache"
for x87_blob_variant in baseline experiment trampoline near-branch install-control near-tail passthrough frame-chain; do
    if [[ "$x87_blob_variant" == baseline ]]; then
        x87_blob_source="$x87_blob_root/source-v1.7.0"
        x87_blob_flags=(-UEXPERIMENT_OPTIONS -UTAIL_OPTIONS -UNEAR_OPTIONS)
    elif [[ "$x87_blob_variant" == experiment ]]; then
        x87_blob_source="$x87_blob_root/source-experiment"
        x87_blob_flags=(-DEXPERIMENT_OPTIONS -UTAIL_OPTIONS -UNEAR_OPTIONS)
    elif [[ "$x87_blob_variant" == trampoline ]]; then
        x87_blob_source="$x87_blob_root/source-trampoline"
        x87_blob_flags=(-DEXPERIMENT_OPTIONS -DTAIL_OPTIONS -UNEAR_OPTIONS)
    elif [[ "$x87_blob_variant" == near-branch ]]; then
        x87_blob_source="$x87_blob_root/source-near-branch"
        x87_blob_flags=(-DEXPERIMENT_OPTIONS -UTAIL_OPTIONS -DNEAR_OPTIONS)
    elif [[ "$x87_blob_variant" == near-tail ]]; then
        x87_blob_source="$x87_blob_root/source-near-tail"
        x87_blob_flags=(-DEXPERIMENT_OPTIONS -UTAIL_OPTIONS -DNEAR_OPTIONS -DNEAR_TAIL_OPTIONS)
    elif [[ "$x87_blob_variant" == passthrough ]]; then
        x87_blob_source="$x87_blob_root/source-passthrough"
        x87_blob_flags=(-DEXPERIMENT_OPTIONS -UTAIL_OPTIONS -DNEAR_OPTIONS -DNEAR_TAIL_OPTIONS -DPASSTHROUGH_OPTIONS)
    elif [[ "$x87_blob_variant" == frame-chain ]]; then
        x87_blob_source="$x87_blob_root/source-frame-chain"
        x87_blob_flags=(-DEXPERIMENT_OPTIONS -UTAIL_OPTIONS -DNEAR_OPTIONS -DNEAR_TAIL_OPTIONS -DPASSTHROUGH_OPTIONS -DFRAME_CHAIN_OPTIONS)
    else
        x87_blob_source="$x87_blob_root/source-install-control"
        x87_blob_flags=(-DEXPERIMENT_OPTIONS -UTAIL_OPTIONS -UNEAR_OPTIONS)
    fi
    /usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
        "${x87_blob_flags[@]}" -I "$x87_blob_source/rosetta_loader/src" \
        -I "$x87_blob_source/rosetta_core/include" \
        "$x87_blob_root/src/test_decoder_blob.cpp" \
        "$x87_blob_source/rosetta_loader/src/stub_asm.cpp" \
        -o "$x87_blob_root/bin/decoder-blob-$x87_blob_variant"
    "$x87_blob_root/bin/decoder-blob-$x87_blob_variant" \
        "$x87_blob_run/$x87_blob_variant.bin"
done
/usr/bin/cmp "$x87_blob_run/baseline.bin" "$x87_blob_run/experiment.bin"
/usr/bin/cmp "$x87_blob_run/baseline.bin" "$x87_blob_run/trampoline.bin"
/usr/bin/cmp "$x87_blob_run/baseline.bin" "$x87_blob_run/near-branch.bin"
/usr/bin/cmp "$x87_blob_run/baseline.bin" "$x87_blob_run/install-control.bin"
/usr/bin/cmp "$x87_blob_run/baseline.bin" "$x87_blob_run/near-tail.bin"
/usr/bin/cmp "$x87_blob_run/baseline.bin" "$x87_blob_run/passthrough.bin"
/usr/bin/cmp "$x87_blob_run/baseline.bin" "$x87_blob_run/frame-chain.bin"
/usr/bin/shasum -a 256 "$x87_blob_run"/*.bin "$x87_blob_root"/bin/decoder-blob-*
echo 'END case=default-blob-byte-identity status=PASS'
