#!/usr/bin/env bash
# James Pond 2 for the PlayStation 5 - build the title into dist/<TITLE_ID>/.
#
#   ps5/tools/build.sh [ninja arguments]
#
# Configures the recomp's own build with the payload SDK's CMake toolchain
# (once) and ps5/cmake/ps5-game.cmake, which gives it SDL3 for the console and
# the title's link (tools/link-title.sh). The 68000 code is recompiled from the
# player's disk on the Mac (build/mac/generated, made by the Mac build's
# jp2-recomp); this build compiles those sources for the console. The disk
# itself is never part of the title: the player copies theirs to the console.
#
#   PS5_BUILD_DIR    the build tree (default build/ps5; the Linux VM's own disk
#                    is several times faster than the shared folder)
#   PS5_VULKAN_DIR   the PS5_Vulkan checkout (default ../PS5_Vulkan)
#   JP2_GENERATED    the recompiled sources (default build/mac/generated)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
generated=${JP2_GENERATED:-$root/build/mac/generated}
[[ -f $generated/tables.cpp ]] || {
    echo "no recompiled code in $generated: build the Mac version first (it recompiles the disk)" >&2
    exit 2
}
sdl=$(bash "$ps5/tools/setup-sdl.sh")
sdk=$(bash "$ps5/tools/setup-sdk.sh")
build=${PS5_BUILD_DIR:-$root/build/ps5}
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

if [[ ! -f $build/build.ninja ]]; then
    mkdir -p "$build"
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DCMAKE_PROJECT_JamesPond2Recomp_INCLUDE="$ps5/cmake/ps5-game.cmake" \
        -DPS5_PORT_DIR="$root" -DPS5_VULKAN_DIR="$vulkan" -DSDL_DIR="$sdl" -DJP2_GENERATED_DIR="$generated" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF > "$build/configure.log" 2>&1 \
        || { tail -60 "$build/configure.log" >&2; exit 1; }
fi
"$ninja" -C "$build" "${@:-jp2}"
