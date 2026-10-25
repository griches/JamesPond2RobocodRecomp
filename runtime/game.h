#pragma once
#include <string>

namespace game {

// Load the program from the player's ADF and point the CPU at its entry.
bool boot(const char *adf_path, std::string *error);

// Turn on the game's own cheat keys (F fly, M map select, ...), as the
// WHDLoad trainer does.
void enable_cheat();

} // namespace game
