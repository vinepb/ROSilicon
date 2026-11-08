#!/bin/bash
set -euo pipefail
x87_observer_root="$(cd "$(dirname "$0")/.." && pwd)"
x87_observer_source="$x87_observer_root/source-owned-recovery-observer"
x87_observer_build="$x87_observer_root/build/helper-owned-recovery-observer"
mkdir -p "$x87_observer_build/objects" "$x87_observer_build/bin" "$x87_observer_root/tmp/clang-module-cache"
export TMPDIR="$x87_observer_root/tmp/"
export CLANG_MODULE_CACHE_PATH="$x87_observer_root/tmp/clang-module-cache"
x87_observer_sdk="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
/usr/bin/xcrun mig -arch arm64 -isysroot "$x87_observer_sdk" \
    -server "$x87_observer_build/mach_excServer.c" -sheader "$x87_observer_build/mach_excServer.h" \
    -user /dev/null -header /dev/null "$x87_observer_sdk/usr/include/mach/mach_exc.defs"
/usr/bin/clang -arch arm64 -mmacosx-version-min=14.0 -O2 \
    -c "$x87_observer_build/mach_excServer.c" -o "$x87_observer_build/objects/mach_excServer.o"
for x87_observer_cpp in "$x87_observer_source"/rosetta_core/src/*.cpp "$x87_observer_source"/rosetta_loader/src/*.cpp; do
    x87_observer_group="$(basename "$(dirname "$(dirname "$x87_observer_cpp")")")"
    x87_observer_stem="$(basename "$x87_observer_cpp" .cpp)"
    /usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
        -I "$x87_observer_source/rosetta_core/include" -I "$x87_observer_build" \
        -c "$x87_observer_cpp" -o "$x87_observer_build/objects/$x87_observer_group-$x87_observer_stem.o"
done
/usr/bin/clang++ -arch arm64 -std=c++23 -O2 -mmacosx-version-min=14.0 \
    "$x87_observer_build"/objects/*.o -framework Security -o "$x87_observer_build/bin/x87sidecar"
/usr/bin/codesign --force --sign - "$x87_observer_build/bin/x87sidecar"
/usr/bin/codesign --verify --strict "$x87_observer_build/bin/x87sidecar"
/usr/bin/shasum -a 256 "$x87_observer_build/bin/x87sidecar"
