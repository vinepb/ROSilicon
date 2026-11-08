#!/bin/bash
set -euo pipefail
x87_build_root="$(cd "$(dirname "$0")/.." && pwd)"
case "${1:-baseline}" in
    baseline) x87_helper_source="$x87_build_root/source-v1.7.0" ;;
    experiment) x87_helper_source="$x87_build_root/source-experiment" ;;
    trampoline) x87_helper_source="$x87_build_root/source-trampoline" ;;
    near-branch) x87_helper_source="$x87_build_root/source-near-branch" ;;
    install-control) x87_helper_source="$x87_build_root/source-install-control" ;;
    near-tail) x87_helper_source="$x87_build_root/source-near-tail" ;;
    passthrough) x87_helper_source="$x87_build_root/source-passthrough" ;;
    frame-chain) x87_helper_source="$x87_build_root/source-frame-chain" ;;
    upstream-current) x87_helper_source="$x87_build_root/source-upstream-2026-10-12" ;;
    upstream-oct26) x87_helper_source="$x87_build_root/source-upstream-2026-10-26" ;;
    *) echo 'usage: build-helper.sh baseline|experiment|trampoline|near-branch|install-control|near-tail|passthrough|frame-chain|upstream-current|upstream-oct26' >&2; exit 2 ;;
esac
x87_helper_build="$x87_build_root/build/helper-${1:-baseline}"
mkdir -p "$x87_helper_build/objects" "$x87_helper_build/bin" "$x87_build_root/tmp/clang-module-cache"
export TMPDIR="$x87_build_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_build_root/tmp/clang-module-cache"
x87_sdk="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
# The same core/loader source lists, language standard, deployment target and
# MIG generation as the cached upstream CMake target. Cooperative mode uses a
# flat signature; no new CMake/Rust dependency or privileged entitlement.
/usr/bin/xcrun mig -arch arm64 -isysroot "$x87_sdk" \
    -server "$x87_helper_build/mach_excServer.c" \
    -sheader "$x87_helper_build/mach_excServer.h" -user /dev/null -header /dev/null \
    "$x87_sdk/usr/include/mach/mach_exc.defs"
/usr/bin/clang -arch arm64 -mmacosx-version-min=14.0 -O2 \
    -c "$x87_helper_build/mach_excServer.c" -o "$x87_helper_build/objects/mach_excServer.o"
for x87_cpp in "$x87_helper_source"/rosetta_core/src/*.cpp \
    "$x87_helper_source"/rosetta_loader/src/*.cpp; do
    x87_group="$(basename "$(dirname "$(dirname "$x87_cpp")")")"
    x87_stem="$(basename "$x87_cpp" .cpp)"
    /usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
        -I "$x87_helper_source/rosetta_core/include" -I "$x87_helper_build" \
        -c "$x87_cpp" -o "$x87_helper_build/objects/$x87_group-$x87_stem.o"
done
/usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
    "$x87_helper_build"/objects/*.o -framework Security \
    -o "$x87_helper_build/bin/x87sidecar"
/usr/bin/codesign --force --sign - "$x87_helper_build/bin/x87sidecar"
/usr/bin/codesign --verify --strict "$x87_helper_build/bin/x87sidecar"
/usr/bin/shasum -a 256 "$x87_helper_build/bin/x87sidecar"
