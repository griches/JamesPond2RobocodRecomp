#!/usr/bin/env bash
# The Linux and Steam Deck release: dist/release/JamesPond2-Linux-<version>.tar.gz,
# a JamesPond2/ folder with the program, a full-screen launcher and the licences.
#
#   tools/release/package-linux.sh JP2_BINARY VERSION
#
# JP2_BINARY is a Release build linked statically against SDL3 and the C++
# library (docs/BUILDING.md), so it needs only the C library.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
binary=${1:?usage: package-linux.sh JP2_BINARY VERSION}
version=${2:?usage: package-linux.sh JP2_BINARY VERSION}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
dir="$work/JamesPond2"
mkdir -p "$dir/licenses"
cp "$binary" "$dir/jp2"
chmod 755 "$dir/jp2"
printf '#!/bin/sh\n# James Pond 2, full screen (add this to Steam as a non-Steam game).\nexec "$(dirname "$0")/jp2" --fullscreen "$@"\n' > "$dir/run.sh"
chmod 755 "$dir/run.sh"
cp "$root/LICENSE" "$dir/licenses/JamesPond2Recomp-GPL-3.0.txt"
cp "$root/vendor/musashi/readme.txt" "$dir/licenses/musashi.txt"
cp "$root/res/fonts/"*.txt "$dir/licenses/"
cp "$root/.deps/SDL-mac-src/LICENSE.txt" "$dir/licenses/sdl.txt"
cat > "$dir/README.txt" <<TXT
James Pond 2: Codename RoboCod $version for Linux and Steam Deck

Run ./run.sh (full screen) or ./jp2. The first time, the game asks for your
own Amiga disk image of the game: put it next to jp2 named jp2.adf, or choose
it on that page (it looks in Downloads, Desktop, Documents, EmuDeck's Amiga
folders and SD cards). Saves and settings: ~/.local/share/bouncingball/James Pond 2/

Steam Deck: in Desktop Mode, Steam > Add a Game > Add a Non-Steam Game, and
add run.sh. Set the controller layout to Gamepad if the buttons don't respond.

Instructions and help: https://github.com/griches/JamesPond2RobocodRecomp
TXT
out="$root/dist/release/JamesPond2-Linux-$version.tar.gz"
mkdir -p "$(dirname "$out")"
COPYFILE_DISABLE=1 tar -czf "$out" -C "$work" JamesPond2
echo "$out ($(du -h "$out" | cut -f1), sha256 $(shasum -a 256 "$out" | cut -c1-64))"
