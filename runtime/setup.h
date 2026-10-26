// The first-run page: the game plays from the player's own Amiga disk, so
// until one has been given it asks for it. It lists disks it finds in the
// usual places, opens the system's file and folder pickers, and takes a file
// dropped on the window.
#pragma once
#include "glyphs.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace setup {

// Tries a disk; on failure says why in words for the player.
using TryDisk = bool (*)(const std::string &path, std::string *error);

// Runs until a disk has been accepted (returns its path) or the player quits
// (returns ""). `why` is shown first if an earlier disk didn't work.
std::string run(SDL_Window *win, SDL_Renderer *ren, TryDisk try_disk, const std::string &why, glyphs::Style style,
                const std::string &store_hint);

// Disks found in the usual places (Downloads, Desktop, Documents, EmuDeck's
// Amiga folder, SD cards), checked with try_disk. For tests too.
std::vector<std::string> scan(TryDisk try_disk);

// Draws the page off screen, for --setup-shot.
void draw_for_test(int w, int h, const std::vector<std::string> &found, const std::string &why, glyphs::Style style,
                   const std::string &store_hint);

} // namespace setup
