#!/bin/bash
# Builds ROSilicon.app from the Swift package, and a .dmg holding it beside a
# shortcut to /Applications to drag it onto. The app carries the Wine runtime,
# DXVK, x87sidecar and the Steam stub, and installs into
# ~/Library/Application Support/ROSilicon, so it downloads nothing but the game
# client and can be moved anywhere once built.
#
# Set APP_OUT to build somewhere other than this folder and WINE_RUNTIME to take
# the Wine tree from somewhere other than .wine-runtime. D9VK points at a folder
# holding a d3d9.dll built by tools/d9vk/build.sh, which is used in preference
# to the checked-in one. Pass --no-dmg to build only the .app, and --no-wine to
# leave the Wine runtime out — quick for working on the UI, but the resulting
# app cannot install.
set -euo pipefail

cd "$(dirname "$0")"
PKG="$(pwd)"

APP_NAME="${APP_NAME:-ROSilicon}"
BUNDLE_ID="${BUNDLE_ID:-com.rosilicon.launcher}"
DATA_ROOT_NAME="${DATA_ROOT_NAME:-ROSilicon}"
ISOLATE_WINE_HOME="${ISOLATE_WINE_HOME:-0}"
APP="${APP_OUT:-$PKG}/$APP_NAME.app"
EXECUTABLE="ROSilicon"
ICON_NAME="ROSilicon"

# These values are interpolated into paths and Info.plist below. Keep the
# supported build interface deliberately narrow rather than trying to escape
# arbitrary XML or path separators supplied by a caller.
if [[ -z "$APP_NAME" || "$APP_NAME" == *['/\\<>&']* ]]; then
    echo "error: invalid APP_NAME: $APP_NAME" >&2
    exit 2
fi
if [[ ! "$BUNDLE_ID" =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]]; then
    echo "error: invalid BUNDLE_ID: $BUNDLE_ID" >&2
    exit 2
fi
if [[ -z "$DATA_ROOT_NAME" || "$DATA_ROOT_NAME" == "." || "$DATA_ROOT_NAME" == ".." \
      || "$DATA_ROOT_NAME" == *['/\\:<>&']* ]]; then
    echo "error: invalid DATA_ROOT_NAME: $DATA_ROOT_NAME" >&2
    exit 2
fi
case "$ISOLATE_WINE_HOME" in
    0) ISOLATED_WINE_HOME_PLIST='<false/>' ;;
    1) ISOLATED_WINE_HOME_PLIST='<true/>' ;;
    *) echo "error: ISOLATE_WINE_HOME must be 0 or 1" >&2; exit 2 ;;
esac

# The one place the version is written down; the bundle and the .dmg name both
# come from here.
[[ -f "$PKG/VERSION" ]] || { echo "error: no VERSION file in $PKG" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < "$PKG/VERSION")"
[[ -n "$VERSION" ]] || { echo "error: $PKG/VERSION is empty" >&2; exit 1; }

MAKE_DMG=1
COPY_WINE=1
for arg in "$@"; do
    case "$arg" in
        --no-dmg) MAKE_DMG=0 ;;
        --no-wine) COPY_WINE=0 ;;
        *) echo "usage: $(basename "$0") [--no-dmg] [--no-wine]" >&2; exit 2 ;;
    esac
done

WINE_RUNTIME="${WINE_RUNTIME:-$PKG/.wine-runtime}"
if [[ "$COPY_WINE" == 1 && ! -x "$WINE_RUNTIME/bin/wine" ]]; then
    echo "error: no Wine runtime at $WINE_RUNTIME" >&2
    echo "       run 'make restore' to fetch the pinned one, or pass --no-wine" >&2
    exit 1
fi

# The Steam stub is built, not checked in, and this script only copies it — so
# it is a missing build product here, the same as the Wine runtime above, and
# not something to cross-compile in the middle of an app build.
STEAM_STUB="${STEAM_STUB:-$PKG/.steam-stub}"
STEAM_STUB_EXE="$STEAM_STUB/steam_stub.exe"
if [[ ! -f "$STEAM_STUB_EXE" ]]; then
    echo "error: no Steam stub at $STEAM_STUB" >&2
    echo "       run 'make steam-stub' to build it" >&2
    exit 1
fi

command -v swift >/dev/null 2>&1 || {
    echo "error: swift not found — install Xcode or the command line tools" >&2
    exit 1
}

echo "==> building (release)"
# macOS chooses a window's appearance from the SDK the binary says it was
# linked against, and SwiftPM writes the deployment target into that field
# rather than the real SDK — so the app is handed the pre-26 look: the old
# smaller window buttons, and legacy styling for the standard controls. The
# deployment target is still what Package.swift asks for; only the SDK the
# binary reports is corrected, to the one actually building it.
MIN_MACOS="$(swift package --package-path "$PKG" dump-package | tr -d ' \n' \
    | sed -n 's/.*"platformName":"macos","version":"\([0-9.]*\)".*/\1/p')"
if [[ -z "$MIN_MACOS" ]]; then
    echo "error: could not read the macOS deployment target from Package.swift" >&2
    exit 1
fi
SDK_MACOS="$(xcrun --show-sdk-version --sdk macosx)"
# The interface is built on Liquid Glass, which arrives in the macOS 26 SDK. An
# older Xcode cannot compile this at all — the symbols are not there to refer
# to, guarded by an availability check or not — so say which SDK is missing
# rather than leaving a page of "cannot find 'glassEffect'" to read.
SDK_MAJOR="${SDK_MACOS%%.*}"
if [[ ! "$SDK_MAJOR" =~ ^[0-9]+$ ]] || (( SDK_MAJOR < 26 )); then
    echo "error: the macOS ${SDK_MACOS:-?} SDK cannot build this" >&2
    echo "       the interface needs the macOS 26 SDK or newer — Xcode 26+" >&2
    echo "       (the app still runs on macOS $MIN_MACOS and up)" >&2
    exit 1
fi
swift build -c release --package-path "$PKG" \
    -Xlinker -platform_version -Xlinker macos \
    -Xlinker "$MIN_MACOS" -Xlinker "$SDK_MACOS"
BIN="$(swift build -c release --package-path "$PKG" --show-bin-path)/$EXECUTABLE"

# DXVK, the Steam stub and x87sidecar ride inside the bundle, so the app
# installs and runs without needing tools/ next to it.
#
# DXVK is the one build product with a checked-in fallback. Building it needs a
# Windows cross-compiler and several minutes, so Resources/d9vk/d3d9.dll is kept
# for builds that have neither: if `make d9vk` has produced one, that is what
# ships, and otherwise the checked-in copy does. `make bundle` always builds and
# validates its own, so a release never goes out on the fallback by accident.
D9VK="${D9VK:-$PKG/.d9vk}"
if [[ -f "$D9VK/d3d9.dll" ]]; then
    DXVK="$D9VK/d3d9.dll"
    DXVK_ORIGIN="built"
else
    DXVK="$PKG/Resources/d9vk/d3d9.dll"
    DXVK_ORIGIN="checked in"
fi
X87_SIDECAR="$PKG/Resources/x87sidecar/x87sidecar"
# rosettax87_jit, the x87 hook offered instead of x87sidecar behind ⌥. Its
# loader reads libRuntimeRosettax87 from its own folder, so the two go in
# together.
ROSETTAX87_JIT="$PKG/Resources/rosettax87_jit"
# The icon, as Icon Composer saves it. Both icons are built from it: macOS 26
# reads it compiled, older macOS reads the .icns drawn from the same layer.
ICON_DOCUMENT="$PKG/$ICON_NAME.icon"
for f in "$DXVK" "$X87_SIDECAR" "$ROSETTAX87_JIT/runtime_loader" \
         "$ROSETTAX87_JIT/libRuntimeRosettax87" "$ROSETTAX87_JIT/LICENSE" \
         "$ICON_DOCUMENT/icon.json"; do
    [[ -f "$f" ]] || { echo "error: $f not found" >&2; exit 1; }
done

echo "==> assembling $APP_NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/$EXECUTABLE"
cp "$DXVK" "$APP/Contents/Resources/d3d9.dll"
echo "    d3d9.dll ($DXVK_ORIGIN): $DXVK"
cp "$STEAM_STUB_EXE" "$APP/Contents/Resources/steam_stub.exe"
install -m 0755 "$X87_SIDECAR" "$APP/Contents/Resources/x87sidecar"
# Copied, not re-signed: the loader's own ad-hoc signature carries the debugger
# entitlement task_for_pid needs, and the signature below seals it as a
# resource without touching it.
mkdir -p "$APP/Contents/Resources/rosettax87_jit"
install -m 0755 "$ROSETTAX87_JIT/runtime_loader" "$ROSETTAX87_JIT/libRuntimeRosettax87" \
    "$APP/Contents/Resources/rosettax87_jit/"
cp "$ROSETTAX87_JIT/LICENSE" "$APP/Contents/Resources/rosettax87_jit/LICENSE"
# The GPL travels with the binary: whoever gets the app gets the license.
cp "$PKG/LICENSE" "$APP/Contents/Resources/LICENSE"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# The Wine runtime, which the installer copies out of here into the install
# folder. It goes in before the signature, since codesign seals Resources/ —
# adding it afterwards would invalidate the signature it just wrote.
if [[ "$COPY_WINE" == 1 ]]; then
    echo "==> bundling the Wine runtime from $WINE_RUNTIME"
    # ditto, not cp: the tree is full of symlinks and executables whose modes
    # have to survive.
    ditto "$WINE_RUNTIME" "$APP/Contents/Resources/Wine"
    # codesign seals every file under Resources/; the Finder's leftovers have no
    # business in the signature.
    find "$APP/Contents/Resources/Wine" -name .DS_Store -delete
else
    echo "==> skipping the Wine runtime (--no-wine)"
fi

# One .lproj per language; macOS picks the reader's and falls back to English.
LANGUAGES=()
for lproj in "$PKG"/Resources/Localizations/*.lproj; do
    [[ -d "$lproj" ]] || { echo "error: no translations in Resources/Localizations" >&2; exit 1; }
    cp -R "$lproj" "$APP/Contents/Resources/"
    LANGUAGES+=("$(basename "$lproj" .lproj)")
done
echo "==> translations: ${LANGUAGES[*]}"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>$APP_NAME</string>
    <key>CFBundleDisplayName</key>       <string>$APP_NAME</string>
    <key>CFBundleIdentifier</key>        <string>$BUNDLE_ID</string>
    <key>CFBundleExecutable</key>        <string>$EXECUTABLE</string>
    <key>CFBundleIconFile</key>          <string>$ICON_NAME</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key>           <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>    <string>14.0</string>
    <key>CFBundleDevelopmentRegion</key>  <string>en</string>
    <key>CFBundleLocalizations</key>
    <array>
$(printf '        <string>%s</string>\n' "${LANGUAGES[@]}")
    </array>
    <key>LSApplicationCategoryType</key> <string>public.app-category.games</string>
    <key>NSHumanReadableCopyright</key>  <string>GNU General Public License v3.0 or later</string>
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSSupportsAutomaticTermination</key><false/>
    <key>ROSiliconDataDirectoryName</key><string>$DATA_ROOT_NAME</string>
    <key>ROSiliconIsolatedWineHome</key>$ISOLATED_WINE_HOME_PLIST
</dict>
</plist>
PLIST

echo "==> drawing the icon"
swift "$PKG/makeicon.swift" "$APP/Contents/Resources/$ICON_NAME.icns" \
    "$ICON_DOCUMENT" >/dev/null

# macOS 26 takes the icon from the compiled document and renders it as Liquid
# Glass — the shape, the shadow and the specular are the system's, which is why
# the document holds the creature and nothing else. 14 through 25 ignore
# CFBundleIconName and read the .icns above, so the key goes in only when the
# catalogue was actually built. actool comes with Xcode; without it the .icns
# carries the whole job and the build still finishes.
if ACTOOL="$(xcrun --find actool 2>/dev/null)"; then
    echo "==> compiling $ICON_NAME.icon"
    GLASS="$(mktemp -d)"
    "$ACTOOL" "$ICON_DOCUMENT" --compile "$GLASS" --platform macosx \
        --minimum-deployment-target 26.0 --app-icon "$ICON_NAME" \
        --output-partial-info-plist "$GLASS/partial.plist" \
        --output-format human-readable-text >/dev/null
    # actool writes an .icns of its own beside the catalogue, but only at two
    # sizes; the one drawn above is the one that ships.
    if [[ -f "$GLASS/Assets.car" ]]; then
        cp "$GLASS/Assets.car" "$APP/Contents/Resources/Assets.car"
        /usr/libexec/PlistBuddy -c "Add :CFBundleIconName string $ICON_NAME" \
            "$APP/Contents/Info.plist" >/dev/null
    else
        echo "warning: actool built no catalogue; the icon will not be Liquid Glass" >&2
    fi
    rm -rf "$GLASS"
else
    echo "==> no actool; skipping the Liquid Glass icon"
fi

# Ad-hoc signature: enough for Gatekeeper to run it locally, and it must not
# use the hardened runtime — the launcher passes DYLD_LIBRARY_PATH down to Wine.
echo "==> signing (ad-hoc)"
codesign --force --sign - "$APP"

# The Finder caches icons per bundle path; touching the app nudges it.
touch "$APP"

echo
echo "Built $APP"

if [[ "$MAKE_DMG" == 1 ]]; then
    DMG="${APP_OUT:-$PKG}/$APP_NAME-$VERSION.dmg"
    echo "==> packing $(basename "$DMG")"

    STAGE="$(mktemp -d)"
    SCRATCH="$(mktemp -d)"
    RW_DMG="$SCRATCH/rw.dmg"
    MOUNT=""
    cleanup() {
        if [[ -n "$MOUNT" ]]; then
            hdiutil detach "$MOUNT" -force -quiet 2>/dev/null || true
        fi
        rm -rf "$STAGE" "$SCRATCH"
    }
    trap cleanup EXIT

    # What the window holds: the app, and the drop target next to it. ditto
    # rather than cp, so the signature and extended attributes survive.
    ditto "$APP" "$STAGE/$APP_NAME.app"
    ln -s /Applications "$STAGE/Applications"

    # Room for the Finder to write its .DS_Store into the image.
    SIZE_MB=$(( $(du -sm "$STAGE" | cut -f1) + 32 ))
    hdiutil create -quiet -srcfolder "$STAGE" -volname "$APP_NAME" -fs HFS+ \
        -format UDRW -size "${SIZE_MB}m" -ov "$RW_DMG"

    # Mounted wherever hdiutil puts it: a stale ROSilicon volume from an
    # earlier run would otherwise be detached out from under someone.
    ATTACHED="$(hdiutil attach "$RW_DMG" -readwrite -noverify -noautoopen)"
    MOUNT="$(printf '%s\n' "$ATTACHED" | sed -n 's|.*\(/Volumes/.*\)$|\1|p' | tail -1)"
    [[ -n "$MOUNT" ]] || { echo "error: could not mount $RW_DMG" >&2; exit 1; }
    VOLUME="$(basename "$MOUNT")"

    # Icon positions live in the volume's .DS_Store, which only the Finder
    # writes. It needs permission to be driven by this terminal, so a refusal
    # leaves the layout at the Finder's default rather than failing the build.
    osascript <<APPLESCRIPT >/dev/null || echo "    (skipped the window layout — the Finder said no)"
tell application "Finder"
    tell disk "$VOLUME"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set bounds of container window to {200, 150, 800, 570}
        set viewOptions to icon view options of container window
        set arrangement of viewOptions to not arranged
        set icon size of viewOptions to 128
        set text size of viewOptions to 13
        set position of item "$APP_NAME.app" of container window to {150, 180}
        set position of item "Applications" of container window to {450, 180}
        close
        open
        update without registering applications
        delay 1
    end tell
end tell
APPLESCRIPT

    # The icon the volume wears on the desktop and in the sidebar. It goes on
    # after the Finder is done: refreshing the window with the custom-icon flag
    # already set makes the Finder delete the .icns and clear the flag again.
    cp "$APP/Contents/Resources/$ICON_NAME.icns" "$MOUNT/.VolumeIcon.icns"
    if command -v SetFile >/dev/null 2>&1; then SetFile -a C "$MOUNT"; fi

    sync
    hdiutil detach "$MOUNT" -quiet || hdiutil detach "$MOUNT" -force -quiet
    MOUNT=""

    rm -f "$DMG"
    hdiutil convert "$RW_DMG" -quiet -format UDZO -imagekey zlib-level=9 -o "$DMG"
    echo "Built $DMG"
fi
