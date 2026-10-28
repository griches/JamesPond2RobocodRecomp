# James Pond 2 for Android - what Android adds to the recomp's own build.
#
# tools/build.sh configures the recomp with the NDK's toolchain, once per ABI,
# and names this file as CMAKE_PROJECT_JamesPond2Recomp_INCLUDE: CMake includes
# it right after the project() call. It provides:
#
#   SDL3    SDL_DIR, the release the desktop builds use, as libSDL3.so; its
#           Java side (in the APK) loads it, then the game, and calls SDL_main
#   code    the recompiled 68000 code, made on the Mac from the player's disk
#           (JP2_GENERATED_DIR): only the game itself is built here
#
# The game is libmain.so (CMakeLists.txt).
#
# SPDX-License-Identifier: GPL-3.0-or-later

foreach(required SDL_DIR JP2_GENERATED_DIR)
	if(NOT ${required})
		message(FATAL_ERROR "android-game.cmake: ${required} is not set (use android/tools/build.sh)")
	endif()
endforeach()

# The game and the interpreter's archive end up in a shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
foreach(off SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_CAMERA)
	set(${off} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/sdl EXCLUDE_FROM_ALL)
