#!/usr/bin/env bash
# The Windows release: dist/release/JamesPond2-Windows-<version>.zip, a
# JamesPond2\ folder with "James Pond 2.exe", SDL3.dll and the licences.
#
#   tools/release/package-windows.sh BUILD_DIR VERSION
#
# BUILD_DIR holds jp2.exe and SDL3.dll from a Release build with clang-cl and
# the static C runtime (docs/BUILDING.md), copied from the Windows machine.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${1:?usage: package-windows.sh BUILD_DIR VERSION}
version=${2:?usage: package-windows.sh BUILD_DIR VERSION}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
dir="$work/JamesPond2"
mkdir -p "$dir/licenses"
cp "$build/jp2.exe" "$dir/James Pond 2.exe"
cp "$build/SDL3.dll" "$dir/SDL3.dll"
cp "$root/LICENSE" "$dir/licenses/JamesPond2Recomp-GPL-3.0.txt"
cp "$root/vendor/musashi/readme.txt" "$dir/licenses/musashi.txt"
cp "$root/res/fonts/"*.txt "$dir/licenses/"
cp "$root/.deps/SDL-mac-src/LICENSE.txt" "$dir/licenses/sdl.txt"
printf '%s\r\n' "James Pond 2: Codename RoboCod $version for Windows" "" \
    "Start \"James Pond 2.exe\". The first time, the game asks for your own Amiga" \
    "disk image of the game: choose it on that page, or drop the file on the window." \
    "Saves and settings: %APPDATA%\\bouncingball\\James Pond 2\\" "" \
    "Instructions and help: https://github.com/griches/JamesPond2RobocodRecomp" > "$dir/README.txt"
out="$root/dist/release/JamesPond2-Windows-$version.zip"
mkdir -p "$(dirname "$out")"
rm -f "$out"
(cd "$work" && COPYFILE_DISABLE=1 zip -q -r -X "$out" JamesPond2)
echo "$out ($(du -h "$out" | cut -f1), sha256 $(shasum -a 256 "$out" | cut -c1-64))"
