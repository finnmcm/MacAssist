#!/usr/bin/env bash
# Build the Swift UI target and wrap it in a minimal MacAssist.app bundle.
# A real bundle (with Info.plist / LSUIElement) is what gives the process a
# bundle identity, a reliable menu-bar status item, and proper activation --
# a bare SwiftPM binary gets none of that. See PLAN.md section 8.
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG="${SWIFT_CONFIG:-debug}"
swift build --package-path app -c "$CONFIG"

BIN="app/.build/$CONFIG/MacAssist"
APP="app/.build/MacAssist.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

cp "$BIN" "$APP/Contents/MacOS/MacAssist"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>          <string>MacAssist</string>
  <key>CFBundleDisplayName</key>   <string>MacAssist</string>
  <key>CFBundleIdentifier</key>    <string>com.finn.macassist</string>
  <key>CFBundleVersion</key>       <string>1</string>
  <key>CFBundleShortVersionString</key> <string>0.1</string>
  <key>CFBundleExecutable</key>    <string>MacAssist</string>
  <key>CFBundlePackageType</key>   <string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <!-- Menu-bar resident, no Dock icon. -->
  <key>LSUIElement</key>           <true/>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

echo "built: $APP"
