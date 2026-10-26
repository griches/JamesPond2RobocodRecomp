#!/bin/sh
# Wraps the built game into a double-clickable macOS app.
#   make_mac_app.sh <jp2 binary> <output .app> [version]
# The app carries no disk: on first run it asks for the player's own.
set -e
bin="$1"
app="$2"
version="${3:-1.0.0}"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/jp2"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>James Pond 2</string>
    <key>CFBundleDisplayName</key>
    <string>James Pond 2</string>
    <key>CFBundleIdentifier</key>
    <string>mobi.bouncingball.jamespond2</string>
    <key>CFBundleExecutable</key>
    <string>jp2</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$version</string>
    <key>CFBundleVersion</key>
    <string>$version</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF
codesign --force --sign - "$app" >/dev/null 2>&1 || true
