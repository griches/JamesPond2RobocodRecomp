# Building

The game's code is not in this repository. The build reads your disk image,
recompiles the game's 68000 code into C++ (`build/<dir>/generated/`), and
compiles that with the runtime. Never commit the disk, anything unpacked from
it, or the generated code: `gamedata/` and `build/` are ignored for that
reason.

## How it fits together

| Part | What it is |
|---|---|
| `tools/recomp/` | The recompiler. It unpacks the game's main program from the disk, follows its code from the entry point (helped by the recorded coverage in `coverage/`) and writes every instruction as C++ in `generated/code_*.cpp`, with a table of entry points and, for the test, one case per instruction. |
| `runtime/` | The Amiga around the game: memory, the custom chips (copper, display, blitter, Paula sound), the CIAs, the disk loader, plus the widescreen redraw, the menus and the presentation. |
| `vendor/musashi/` | A 68000 interpreter. It runs any code the recompiler didn't reach, and is the whole CPU of `jp2-interp`. |
| `android/` | The Android layer: the manifest, the icon and the APK's build, around SDL3's own Android support. |
| `ps5/` | The PlayStation 5 layer: SDL3's console drivers, start-up, link and packaging. |

The build makes:

- `jp2`: the game, recompiled;
- `jp2-interp`: the same game on the interpreter alone, the reference for tests;
- `jp2-difftest`: runs every translated instruction against the interpreter
  from random states and compares the results;
- `jp2-recomp`: the recompiler;
- on macOS, `James Pond 2.app`.

## macOS

You need Xcode's command line tools, CMake 3.20 or later, Ninja and SDL3. For
playing and development, Homebrew's SDL3 is enough:

```sh
brew install cmake ninja sdl3
cmake -S . -B build/mac -G Ninja -DJP2_ADF="$PWD/gamedata/jp2.adf"
ninja -C build/mac
build/mac/jp2-difftest --adf gamedata/jp2.adf
open "build/mac/James Pond 2.app"
```

`JP2_ADF` is the disk the code is recompiled from (any of the supported
releases). Without it, only `jp2-interp` and the tools are built.

The release app is universal (Apple silicon and Intel) and carries SDL3 inside
it: build SDL3 statically for both, then the game against it, with play
recording off, and package it (`tools/release/package-mac.sh` signs it with
your Developer ID and, given `NOTARY_KEY_JSON`, notarises it):

```sh
git clone --depth 1 -b release-3.4.18 https://github.com/libsdl-org/SDL.git .deps/SDL-mac-src
cmake -S .deps/SDL-mac-src -B .deps/sdl-mac-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_INSTALL_PREFIX="$PWD/.deps/sdl-mac-static"
ninja -C .deps/sdl-mac-build install
cmake -S . -B build/mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
      -DSDL3_DIR="$PWD/.deps/sdl-mac-static/lib/cmake/SDL3" -DJP2_RECORD_PLAY=OFF \
      -DJP2_ADF="$PWD/gamedata/jp2.adf"
ninja -C build/mac-release
NOTARY_KEY_JSON=~/.blitz/asc-credentials.json tools/release/package-mac.sh
```

## Windows

You need the Visual Studio Build Tools (for the Windows SDK and linker), LLVM
(for clang-cl), CMake and Ninja, and SDL3's Visual C++ development package
(`SDL3-devel-<version>-VC.zip` from SDL's releases). In a Visual Studio x64
command prompt with LLVM on the path:

```bat
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link ^
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DJP2_RECORD_PLAY=OFF ^
      -DSDL3_DIR=C:/path/to/SDL3-<version>/cmake -DJP2_ADF=C:/path/to/jp2.adf
ninja -C build-win
copy C:\path\to\SDL3-<version>\lib\x64\SDL3.dll build-win\
build-win\jp2-difftest.exe --adf C:\path\to\jp2.adf
```

`jp2.exe` is a windowed program: it has no console, so the test options
below print nothing on Windows (their screenshots still work).

## Linux and Steam Deck

The release build is linked statically against SDL3 and the C++ library, so
the program needs only the C library and runs on most distributions. On a
Steam Deck, build inside a distrobox (for example Debian 13) rather than on
SteamOS itself.

```sh
# SDL3, static, once
git clone --depth 1 -b release-3.4.10 https://github.com/libsdl-org/SDL.git sdl3-src
cmake -S sdl3-src -B sdl3-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DCMAKE_INSTALL_PREFIX="$HOME/sdl3-static"
ninja -C sdl3-build install

# the game
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_PREFIX_PATH="$HOME/sdl3-static" \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
      -DJP2_RECORD_PLAY=OFF -DJP2_ADF="$PWD/gamedata/jp2.adf"
ninja -C build-linux jp2
tools/release/package-linux.sh build-linux/jp2 <version>
```

A launcher for full screen is one line, `exec "$(dirname "$0")/jp2" --fullscreen "$@"`.

## Android

The APK is built on the Mac, with the Android SDK's NDK and build tools and no
Gradle. Like the console's, it compiles the code the Mac build generated, so
build for macOS first. You need the SDK (Android Studio puts it in
`~/Library/Android/sdk`) with an NDK, the build tools and the Android 15
(API 35) platform, and a JDK.

```sh
ninja -C build/mac                       # generates build/mac/generated
android/tools/build.sh                   # dist/android/JamesPond2-Android-<version>.apk
android/tools/device.sh install          # onto the device adb sees
android/tools/device.sh disk gamedata/jp2.adf
android/tools/device.sh run 60           # start it and follow its log
```

The game is `libmain.so`, beside SDL3's `libSDL3.so`, and SDL's own Java
activity runs it; both come from the SDL checkout the desktop builds use
(`.deps/SDL-mac-src`, cloned if it isn't there). The APK is for 64-bit ARM
(`ANDROID_ABIS` adds others, such as `x86_64` for an Intel emulator).

The first build makes the game's signing key, `~/.android/jamespond2.jks`,
with its password in `~/.android/jamespond2.jks.pass`. Keep both: Android
updates an installed game only from an APK signed with the same key
(`JP2_KEYSTORE` names another). Home-screen art made from your own copy of the
game can go in `gamedata/android-art/` (`icon.png`, and `banner.png` for
Android TV); without it the build uses the PlayStation 5's `icon0.png` and
Steam's `capsule.png` from `gamedata/`, if they're there.

`device.sh disk` copies a disk into the game's folder on the device
(`Android/data/mobi.bouncingball.jamespond2/files`), where the first-run page
finds it; `device.sh forget` clears the game's data for a first start again.

## PlayStation 5

The console build cross-compiles in a Linux VM (Lima, named `ps5`) that has
the PS5 payload SDK fork and the PS5_Vulkan stack (Mesa's RADV) in `~/ps5`,
with this folder mounted at the same path; `ps5/tools/vm.sh` runs the build
there. It compiles the code the Mac build generated, so build for macOS first.

```sh
ninja -C build/mac                         # generates build/mac/generated
ps5/tools/vm.sh build                      # the title, in dist/PPSA99323/
PS5_HOST=<console address> ps5/tools/console.sh deploy
PS5_HOST=<console address> ps5/tools/console.sh disk gamedata/jp2.adf
PS5_HOST=<console address> ps5/tools/console.sh run 120
ps5/tools/package-release.sh <version>    # after committing
```

`console.sh run` starts the title and follows the console's log (into
`klog/`); `console.sh quit` closes it. Deploying needs PS5_Vulkan's `ps5vkctl`
running on the console, sent to its payload loader on port 9021. Home-screen
art made from your own copy of the game can go in `gamedata/ps5-art/`
(`icon0.png`, `pic0.dds`); the link copies it into the title.

For a test run, `console.sh env KEY=VALUE` and `console.sh args ARGUMENT...`
write `env.txt` and `args.txt` beside the title, which it reads at start. An
empty file named `devloop-shot` sent to the title's folder makes it save the
screen as `userdata/shot.png`.

## Testing

`jp2` runs without a window for tests:

```sh
build/mac/jp2 --adf gamedata/jp2.adf --headless 470 --input "200-206:fire" --shot 465:title.png
```

| Option | What it does |
|---|---|
| `--headless N` | run N frames without a window |
| `--input "F-G:keys,..."` | hold `up`, `down`, `left`, `right`, `fire` (joined with `+`) from frame F to G |
| `--shot F:file.png`, `--wide-shot F:file.png` | the game's own picture, or the widescreen redraw, at frame F |
| `--load save.jp2s`, `--save-at F:save.jp2s` | start from a save, or save at frame F |
| `--section F:N` | go to level N (hexadecimal) at frame F |
| `--poke F:ADDR=VALUE` | write to the game's memory at frame F |
| `--trace F:dir` | every custom register write and blit in frame F, and a memory dump |
| `--wav out.wav` | what the speakers would have played |
| `--menu-shot`, `--setup-shot`, `--picture-shot` | the menus, the disk page and the presentation, drawn off screen |

Environment variables for tests and diagnostics include `JP2_PREF_DIR` (a
separate saves folder), `JP2_SCRIPT` (key presses and screenshots for the
window, with `SDL_VIDEODRIVER=dummy`), `JP2_WINDOW=1920x1080`,
`JP2_FRAMETIME`, `JP2_WIDECHECK`, `JP2_FLICKER`, `JP2_RUMBLELOG`,
`JP2_AUDIOLOG` and `JP2_WATCH=<address>` (logs every write to that word).
The comments in `runtime/main.cpp` describe each.
