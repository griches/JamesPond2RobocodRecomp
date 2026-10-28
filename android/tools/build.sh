#!/usr/bin/env bash
# James Pond 2 for Android - build the APK into dist/android/.
#
#   android/tools/build.sh    -> dist/android/JamesPond2-Android-<version>.apk
#
# Compiles the code the Mac build recompiled from the player's disk
# (build/mac/generated) with the NDK, as libmain.so beside SDL3's libSDL3.so,
# and packs them with SDL's Java side into an APK signed with the game's own
# key. The SDK's build tools do the packing; there is no Gradle. The disk is
# never in the APK: the player chooses theirs on the device. Build a release
# after committing: SOURCE.txt in the APK names the commit.
#
#   ANDROID_HOME     the SDK (default ~/Library/Android/sdk): an NDK, the build
#                    tools and the android-35 platform
#   ANDROID_ABIS     the ABIs to build (default arm64-v8a, what Android
#                    handhelds and TV boxes run)
#   JP2_GENERATED    the recompiled sources (default build/mac/generated)
#   JP2_KEYSTORE     the signing key (default ~/.android/jamespond2.jks, made on
#                    first use, its password beside it in .pass). Android
#                    updates an install only with an APK signed by the same key.
#
# Home-screen art made from your own copy of the game: gamedata/android-art/
# icon.png (square) and banner.png (Android TV's, cut to 16:9), else the PS5's
# icon0.png and Steam's capsule.png from gamedata/; with none, SDL's icon.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

android=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$android")
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
ndk=$(ls -d "$sdk"/ndk/* 2>/dev/null | sort -V | tail -1)
tools=$(ls -d "$sdk"/build-tools/* 2>/dev/null | sort -V | tail -1)
min_sdk=24 target_sdk=35
jar=$sdk/platforms/android-$target_sdk/android.jar
for need in "$ndk/build/cmake/android.toolchain.cmake" "$tools/aapt2" "$jar"; do
    [[ -f $need ]] || { echo "build.sh: not in the Android SDK ($sdk): $need" >&2; exit 2; }
done
generated=${JP2_GENERATED:-$root/build/mac/generated}
[[ -f $generated/tables.cpp ]] || {
    echo "no recompiled code in $generated: build the Mac version first (it recompiles the disk)" >&2
    exit 2
}
# The desktop builds' SDL (docs/BUILDING.md): its Java side and its C must match
sdl=${SDL_DIR:-$root/.deps/SDL-mac-src}
[[ -d $sdl/android-project ]] || git clone --depth 1 -b release-3.4.18 https://github.com/libsdl-org/SDL.git "$sdl"

version=$(sed -n 's/^project(JamesPond2Recomp VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
IFS=. read -r major minor patch <<< "$version"
build=${ANDROID_BUILD_DIR:-$root/build/android}
stage=$build/stage
rm -rf "$stage" "$build/res" "$build/classes" "$build/dex"
mkdir -p "$stage" "$build/classes" "$build/dex"

# ---- the game, per ABI
strip=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -1)
for abi in ${ANDROID_ABIS:-arm64-v8a}; do
    dir=$build/$abi
    if [[ ! -f $dir/build.ninja ]]; then
        mkdir -p "$dir"
        cmake -S "$root" -B "$dir" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$min_sdk" -DANDROID_STL=c++_static \
            -DCMAKE_PROJECT_JamesPond2Recomp_INCLUDE="$android/cmake/android-game.cmake" \
            -DSDL_DIR="$sdl" -DJP2_GENERATED_DIR="$generated" -DJP2_RECORD_PLAY=OFF \
            -DCMAKE_BUILD_TYPE=Release > "$dir/configure.log" 2>&1 \
            || { tail -40 "$dir/configure.log" >&2; exit 1; }
    fi
    ninja -C "$dir" jp2 SDL3-shared
    mkdir -p "$stage/lib/$abi"
    "$strip" --strip-unneeded -o "$stage/lib/$abi/libmain.so" "$dir/libmain.so"
    "$strip" --strip-unneeded -o "$stage/lib/$abi/libSDL3.so" "$(find "$dir/sdl" -name libSDL3.so | head -1)"
done

# ---- resources: the icon and the TV banner
res=$build/res
cp -R "$android/res" "$res"
manifest=$build/AndroidManifest.xml
cp "$android/AndroidManifest.xml" "$manifest"
art=$root/gamedata
icon= banner=
for f in "$art/android-art/icon.png" "$art/ps5-art/icon0.png"; do [[ -f $f ]] && { icon=$f; break; }; done
for f in "$art/android-art/banner.png" "$art/steam-art/capsule.png"; do [[ -f $f ]] && { banner=$f; break; }; done
command -v sips > /dev/null || icon= banner=   # (macOS's image tool does the resizing)
if [[ -n $icon ]]; then
    mkdir -p "$res/mipmap-xxxhdpi"
    sips -s format png -z 192 192 "$icon" --out "$res/mipmap-xxxhdpi/ic_launcher.png" > /dev/null
    sips -s format png -z 432 432 "$icon" --out "$res/mipmap-xxxhdpi/ic_launcher_art.png" > /dev/null
else
    echo "no icon art in gamedata/: SDL's icon" >&2
    rm -rf "$res/mipmap-anydpi-v26"
    for d in "$sdl"/android-project/app/src/main/res/mipmap-*; do cp -R "$d" "$res/"; done
fi
if [[ -n $banner ]]; then
    # The middle 16:9 of it, at 320x180 (160x90 dp, xhdpi)
    mkdir -p "$res/drawable-xhdpi"
    w=$(sips -g pixelWidth "$banner" | awk '/pixelWidth/ {print $2}')
    h=$(sips -g pixelHeight "$banner" | awk '/pixelHeight/ {print $2}')
    if ((w * 9 > h * 16)); then w=$((h * 16 / 9)); else h=$((w * 9 / 16)); fi
    sips -s format png --cropToHeightWidth "$h" "$w" "$banner" --out "$build/banner-crop.png" > /dev/null
    sips -z 180 320 "$build/banner-crop.png" --out "$res/drawable-xhdpi/banner.png" > /dev/null
else
    echo "no banner art in gamedata/: none for Android TV" >&2
    sed -i '' '/android:banner=/d' "$manifest"
fi

# ---- the licences, as every release carries them, and where the source is
assets=$build/assets
rm -rf "$assets"
mkdir -p "$assets/licenses"
cp "$root/LICENSE" "$assets/licenses/JamesPond2Recomp-GPL-3.0.txt"
cp "$root/vendor/musashi/readme.txt" "$assets/licenses/musashi.txt"
cp "$root/res/fonts/"*.txt "$assets/licenses/"
cp "$sdl/LICENSE.txt" "$assets/licenses/sdl.txt"
commit=$(git -C "$root" rev-parse --short HEAD)
[[ -z $(git -C "$root" status --porcelain) ]] || commit="$commit, with changes"
cat > "$assets/SOURCE.txt" <<TXT
James Pond 2: Codename RoboCod for Android $version
Source: https://github.com/griches/JamesPond2RobocodRecomp (commit $commit)
Licences: licenses/
TXT

"$tools/aapt2" compile --dir "$res" -o "$build/res.zip"
"$tools/aapt2" link -o "$build/unsigned.apk" -I "$jar" --manifest "$manifest" -A "$assets" \
    --min-sdk-version "$min_sdk" --target-sdk-version "$target_sdk" \
    --version-code $((major * 10000 + minor * 100 + patch)) --version-name "$version" "$build/res.zip"

# ---- SDL's Java side
find "$sdl/android-project/app/src/main/java" -name '*.java' > "$build/java-sources.txt"
javac -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$jar" -d "$build/classes" @"$build/java-sources.txt"
find "$build/classes" -name '*.class' > "$build/classes.txt"
"$tools/d8" --release --min-api "$min_sdk" --lib "$jar" --output "$build/dex" @"$build/classes.txt"

# ---- the APK: native libraries stored and page-aligned, so they load in place
cp "$build/unsigned.apk" "$build/packed.apk"
(cd "$build/dex" && zip -q -X "$build/packed.apk" classes.dex)
(cd "$stage" && zip -q -X -0 -r "$build/packed.apk" lib)
"$tools/zipalign" -f -P 16 4 "$build/packed.apk" "$build/aligned.apk"

keystore=${JP2_KEYSTORE:-$HOME/.android/jamespond2.jks}
if [[ ! -f $keystore ]]; then
    mkdir -p "$(dirname "$keystore")"
    (umask 077 && openssl rand -hex 24 > "$keystore.pass")
    keytool -genkeypair -keystore "$keystore" -storepass:file "$keystore.pass" -alias jamespond2 \
        -keyalg RSA -keysize 4096 -validity 36500 -dname "CN=James Pond 2 Recompiled, O=bouncingball" 2> /dev/null
    echo "made a signing key, $keystore (password: $keystore.pass): keep both, updates need it" >&2
fi
out=$root/dist/android/JamesPond2-Android-$version.apk
mkdir -p "$(dirname "$out")"
"$tools/apksigner" sign --ks "$keystore" --ks-pass "file:$keystore.pass" --out "$out" "$build/aligned.apk"
rm -f "$out.idsig"
echo "$out ($(du -h "$out" | cut -f1), sha256 $(shasum -a 256 "$out" | cut -c1-64))"
