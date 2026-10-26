#!/usr/bin/env bash
# James Pond 2 for the PlayStation 5 - SDL3, patched for the console.
#
#   ps5/tools/setup-sdl.sh       fetch it into .deps/SDL (once a release), print the path
#
# The release is the one Geometry Wars 2's console port uses. ps5/patches/sdl.patch
# registers the console's video, audio and joystick drivers, which live in
# ps5/sdl/, and builds them when the toolchain says PS5.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdl="$root/.deps/SDL"
tag=release-3.4.18
patch="$root/ps5/patches/sdl.patch"
stamp="$tag $(sha256sum "$patch" | cut -d' ' -f1)"

if [[ ! -f $sdl/.ps5-sdl || $(<"$sdl/.ps5-sdl") != "$stamp" ]]; then
    {
        if [[ ! -d $sdl/.git ]]; then
            git clone -q --depth 1 -b "$tag" https://github.com/libsdl-org/SDL.git "$sdl"
        elif [[ $(git -C "$sdl" describe --tags --exact-match 2>/dev/null) != "$tag" ]]; then
            git -C "$sdl" fetch -q --depth 1 origin "refs/tags/$tag:refs/tags/$tag"
            git -C "$sdl" checkout -q -f "$tag"
        fi
        git -C "$sdl" checkout -q -f HEAD -- .
        git -C "$sdl" apply --whitespace=nowarn "$patch"
    } >&2
    echo "$stamp" > "$sdl/.ps5-sdl"
fi
echo "$sdl"
