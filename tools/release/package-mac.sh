#!/usr/bin/env bash
# The macOS release: James Pond 2.app, signed with the keychain's Developer ID,
# notarised when NOTARY_KEY_JSON names an App Store Connect API key's JSON
# ({keyId, issuerId}, its AuthKey_<keyId>.p8 beside it), zipped into
# dist/release/JamesPond2-macOS-<version>.zip.
#
#   tools/release/package-mac.sh [build dir]     (default build/mac-release)
#
# The build dir is a Release build against a static, universal SDL3 with
# JP2_RECORD_PLAY=OFF (docs/BUILDING.md). The app is assembled in $TMPDIR: an
# iCloud-synced ~/Documents gives files Finder information that codesign refuses.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${1:-$root/build/mac-release}
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")
[[ -n $version ]] || { echo "no project version in $build" >&2; exit 2; }
identity=${MAC_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)}
[[ -n $identity ]] || { echo "no Developer ID Application identity in the keychain" >&2; exit 2; }

stage=$(mktemp -d "${TMPDIR:-/tmp}/jp2-mac.XXXXXX")
app="$stage/James Pond 2.app"
ditto --norsrc --noextattr "$build/James Pond 2.app" "$app"
mkdir -p "$app/Contents/Resources/licenses"
cp "$root/LICENSE" "$app/Contents/Resources/licenses/JamesPond2Recomp-GPL-3.0.txt"
cp "$root/vendor/musashi/readme.txt" "$app/Contents/Resources/licenses/musashi.txt"
cp "$root/res/fonts/"*.txt "$app/Contents/Resources/licenses/"
cp "$root/.deps/SDL-mac-src/LICENSE.txt" "$app/Contents/Resources/licenses/sdl.txt"
xattr -cr "$app"
codesign --force --timestamp --options runtime -s "$identity" "$app"
codesign --verify --strict --deep "$app"
echo "signed: $identity"

if [[ -n ${NOTARY_KEY_JSON:-} ]]; then
    key_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["keyId"])' "$NOTARY_KEY_JSON")
    issuer=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["issuerId"])' "$NOTARY_KEY_JSON")
    ditto -c -k --norsrc --noextattr --keepParent "$app" "$stage/notary.zip"
    xcrun notarytool submit "$stage/notary.zip" --key "$(dirname "$NOTARY_KEY_JSON")/AuthKey_$key_id.p8" \
        --key-id "$key_id" --issuer "$issuer" --wait --timeout "${NOTARY_TIMEOUT:-90m}" | grep -E "status:|id:" | sort -u
    xcrun stapler staple "$app"
    spctl --assess --type execute -vv "$app" 2>&1 | head -2
fi
zip="$root/dist/release/JamesPond2-macOS-$version.zip"
mkdir -p "$(dirname "$zip")"
rm -f "$zip"
ditto -c -k --norsrc --noextattr --keepParent "$app" "$zip"
rm -rf "$stage"
echo "$zip ($(du -h "$zip" | cut -f1), sha256 $(shasum -a 256 "$zip" | cut -c1-64))"
