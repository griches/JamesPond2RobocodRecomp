#!/usr/bin/env bash
# James Pond 2 for Android - install and run on the device adb sees.
#
#   android/tools/device.sh install          the APK build.sh made
#   android/tools/device.sh disk [FILE]      a disk image into the game's folder (default gamedata/jp2_sr_aga.adf)
#   android/tools/device.sh run [SECONDS]    start it and follow its log (default 60 s)
#   android/tools/device.sh shot FILE.png    the screen
#   android/tools/device.sh quit             close it
#   android/tools/device.sh forget           remove its saves, settings and disk (a first start again)
#
# The game's folder is the one a computer sees over USB as
# Android/data/mobi.bouncingball.jamespond2/files; the first-run page lists
# disks found there.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
android=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$android")
adb=$(command -v adb || echo "${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb")
package=mobi.bouncingball.jamespond2
folder=/sdcard/Android/data/$package/files

case ${1:?usage: device.sh install|disk|run|shot|quit|forget} in
install)
    apk=$(ls -t "$root"/dist/android/JamesPond2-Android-*.apk | head -1)
    "$adb" install -r "$apk"
    ;;
disk)
    file=${2:-$root/gamedata/jp2_sr_aga.adf}
    [[ -f $file ]] || { echo "no disk image at $file" >&2; exit 2; }
    "$adb" shell mkdir -p "$folder"
    "$adb" push "$file" "$folder/"
    ;;
run)
    "$adb" logcat -c
    "$adb" shell am start -n "$package/org.libsdl.app.SDLActivity" > /dev/null
    "$adb" logcat -v brief SDL:V SDL/APP:V SDL/ERROR:V DEBUG:V AndroidRuntime:E libc:F '*:S' &
    log=$!
    sleep "${2:-60}"
    kill "$log" 2> /dev/null || true
    ;;
shot)
    "$adb" exec-out screencap -p > "${2:?usage: device.sh shot FILE.png}"
    ;;
quit)
    "$adb" shell am force-stop "$package"
    ;;
forget)
    "$adb" shell pm clear "$package"
    ;;
*)
    echo "usage: device.sh install|disk|run|shot|quit|forget" >&2
    exit 2
    ;;
esac
