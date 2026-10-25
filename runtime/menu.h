// The pause menu: a rail of pages on the left (Game, Save, Load, Display,
// Sound, Cheats, Controls) and the open page in a panel on the right.
// Settings are edited in place; everything else goes back to the host as a
// Request.
#pragma once
#include "glyphs.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

namespace menu {

enum class Action { up, down, left, right, confirm, back, prev_page, next_page, close };

enum class Cmd { none, resume, quit_to_title, exit_game, load, save, skip_exit, invincible, infinite_lives, wings };
struct Request {
    Cmd cmd = Cmd::none;
    int arg = 0;
};

constexpr int kSlots = 4;   // 0 is the quick slot (L1 saves, R1 loads)
struct Slot {
    bool exists = false;
    std::string when;          // "Today 16:42"
    SDL_Texture *thumb = nullptr;
};

// What the menu shows. `settings` is edited in place.
struct View {
    settings::Settings *settings = nullptr;
    Slot slots[kSlots];
    bool in_level = false;
    bool invincible = false, infinite_lives = false, wings = false;
    uint32_t score = 0;
    int lives = 0, energy = 0;
    float level_l = 0, level_r = 0;   // the sound preview's loudness, 0..1
    bool pad = true;                  // the last input came from a gamepad: show its buttons
    glyphs::Style style = glyphs::Style::xbox;
    bool can_fullscreen = true;
};

void open();
bool is_open();
void close();
Request input(Action a, const View &v);
void draw(const View &v, float dt);
void toast(const char *text);

// 0: the page covers the game; 1: it has faded to let the game show through
// (while a picture setting has the focus).
float preview();
// On the Sound page: play the game's last few seconds through the settings.
bool sound_preview();

void show_for_test(int page, int row, bool in_rows, bool dialog);   // --menu-shot

} // namespace menu
