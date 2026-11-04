#!/usr/bin/env bash
# Build a self-contained, ad-hoc-signed MacAssist.app and package it for a
# GitHub release (.zip + .dmg). Unlike scripts/make-app.sh (a dev convenience
# that bundles only the Swift binary), this produces the real distributable:
# the C++ daemon rides along in Contents/Resources/macassistd, and the app
# installs it as a launchd agent on first launch (see DaemonInstaller.swift).
#
# Free/ad-hoc signing only — no Apple Developer ID, no notarization. That's
# enough for a technical GitHub audience; users clear the quarantine flag once
# (see the "Install" notes printed at the end). Apple Silicon only.
#
# Usage: scripts/make-release.sh [VERSION]   (VERSION default: 0.1)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-0.1}"
BUILD_NUMBER="${BUILD_NUMBER:-1}"
DIST="dist"
APP="$DIST/MacAssist.app"
DAEMON_BUILD="build-release"

echo "==> Building daemon (Release, arm64) …"
# Force arm64: MacAssist is Apple Silicon only, and an x86_64 cmake (e.g. the
# Rosetta Homebrew one) would otherwise drive clang with an x86_64 personality
# and emit the wrong arch for the bundled daemon.
cmake -S . -B "$DAEMON_BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
cmake --build "$DAEMON_BUILD" --target macassistd -j"$(sysctl -n hw.ncpu)" >/dev/null

echo "==> Building app (release) …"
swift build --package-path app -c release >/dev/null

APP_BIN="app/.build/release/MacAssist"
DAEMON_BIN="$DAEMON_BUILD/daemon/macassistd"
for f in "$APP_BIN" "$DAEMON_BIN"; do
  [[ -x "$f" ]] || { echo "error: missing build product $f" >&2; exit 1; }
done

echo "==> Assembling $APP …"
rm -rf "$DIST"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$APP_BIN" "$APP/Contents/MacOS/MacAssist"
# The daemon lives in Resources so Bundle.main.url(forResource:"macassistd")
# resolves it (DaemonInstaller.bundledDaemonPath). cp preserves the exec bit.
cp "$DAEMON_BIN" "$APP/Contents/Resources/macassistd"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>          <string>MacAssist</string>
  <key>CFBundleDisplayName</key>   <string>MacAssist</string>
  <key>CFBundleIdentifier</key>    <string>com.finn.macassist</string>
  <key>CFBundleVersion</key>       <string>$BUILD_NUMBER</string>
  <key>CFBundleShortVersionString</key> <string>$VERSION</string>
  <key>CFBundleExecutable</key>    <string>MacAssist</string>
  <key>CFBundlePackageType</key>   <string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <!-- Menu-bar resident, no Dock icon. -->
  <key>LSUIElement</key>           <true/>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

echo "==> Ad-hoc signing (inside-out) …"
# Sign the nested daemon first, then the bundle (which signs Contents/MacOS/
# MacAssist too). Ad-hoc identity "-"; TCC still identifies the daemon by its
# cdhash, so Full Disk Access granted to it sticks across launches.
# Pin Apple's codesign: Anaconda (and others) can shadow it on PATH with a
# sigtool shim that chokes on Mach-O bundles.
CODESIGN=/usr/bin/codesign
"$CODESIGN" --force --sign - "$APP/Contents/Resources/macassistd"
"$CODESIGN" --force --sign - "$APP"
"$CODESIGN" --verify --strict "$APP"
echo "    signed: $("$CODESIGN" -dv "$APP" 2>&1 | grep -E 'Signature|Identifier' | tr '\n' ' ')"

STEM="MacAssist-$VERSION"

echo "==> Packaging $STEM.zip …"
# ditto preserves the bundle structure, symlinks, and signature.
( cd "$DIST" && ditto -c -k --keepParent "MacAssist.app" "$STEM.zip" )

echo "==> Packaging $STEM.dmg …"
# A tiny staging tree: the app plus an /Applications symlink, so the DMG opens
# to a drag-to-install layout. UDZO = compressed, read-only.
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DIST/$STEM.dmg"
hdiutil create -volname "MacAssist" -srcfolder "$STAGE" \
  -ov -format UDZO -quiet "$DIST/$STEM.dmg"
rm -rf "$STAGE"

echo
echo "built:"
echo "  $APP"
echo "  $DIST/$STEM.zip"
echo "  $DIST/$STEM.dmg"
echo
echo "Install (ad-hoc signed, so Gatekeeper needs one nudge):"
echo "  1. Drag MacAssist.app to /Applications"
echo "  2. Clear the download quarantine once:"
echo "       xattr -dr com.apple.quarantine /Applications/MacAssist.app"
echo "  3. Launch it. Grant the daemon Full Disk Access when prompted."
