#!/usr/bin/env bash
# James Pond 2 for the PlayStation 5 - zip dist/<TITLE_ID>/ as a ready-made release.
#
#   ps5/tools/package-release.sh VERSION    -> dist/release/JamesPond2-PS5-<VERSION>.zip
#
# The title as it goes on the console. The player adds their own disk image to
# the folder; nothing from a disk or a player goes in: this checks that before
# zipping. Package after committing: SOURCE.txt names the commit the build came
# from.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
version=${1:?usage: package-release.sh VERSION (e.g. 1.0.0)}
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
dist="$root/dist/$title"
[[ -f $dist/eboot.bin ]] || { echo "package-release.sh: build first (no $dist/eboot.bin)" >&2; exit 2; }
[[ -z $(git -C "$root" status --porcelain) ]] ||
    { echo "package-release.sh: commit first (the tree has changes)" >&2; exit 2; }

found=$(find "$dist" \( -iname '*.adf' -o -iname '*.adz' -o -iname '*.zip' -o -iname '*.off' -o -name userdata \
    -o -name cache -o -name env.txt -o -name args.txt -o -name devloop-shot \) -print)
[[ -z $found ]] || { echo "package-release.sh: disk or player files in $dist:" >&2; echo "$found" >&2; exit 2; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -R "$dist" "$work/$title"
cat > "$work/$title/PUT YOUR DISK HERE.txt" <<'TXT'
Copy your own Amiga disk image of James Pond 2: Codename RoboCod into this
folder (/data/homebrew/PPSA99323 on the console): an .adf, or an .adz or .zip
with one inside. Any file name works. Then start the game.

Supported: the Kixx re-release and the common cracked disks, and WHDLoad
installs (as a folder in here). Not yet: the original Millennium release,
raw-track (extended ADF), DMS and IPF images.
TXT
cat > "$work/$title/SOURCE.txt" <<TXT
James Pond 2: Codename RoboCod for PlayStation 5 $version
Source: https://github.com/griches/JamesPond2RobocodRecomp (commit $(git -C "$root" rev-parse --short HEAD))
Licences: licenses/
TXT
zip_path="$root/dist/release/JamesPond2-PS5-$version.zip"
mkdir -p "$(dirname "$zip_path")"
rm -f "$zip_path"
(cd "$work" && COPYFILE_DISABLE=1 zip -q -r -X "$zip_path" "$title")
echo "$zip_path ($(du -h "$zip_path" | cut -f1), sha256 $(shasum -a 256 "$zip_path" | cut -d' ' -f1))"
