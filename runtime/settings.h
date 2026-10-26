// The player's choices from the menu, kept in settings.ini beside the saves.
#pragma once
#include <string>

namespace settings {

struct Settings {
    bool widescreen = true;
    bool smooth = true;       // in-between frames at the display's rate
    int picture = 0;          // 0 crisp, 1 pixel perfect, 2 soft, 3 CRT
    int scanlines = 0;        // 0 off, 1 light, 2 strong
    bool fullscreen = false;
    int volume = 8;           // 0..10
    int sound_model = 0;      // 0 Amiga 500, 1 Amiga 1200, 2 unfiltered
    int stereo = 1;           // 0 Amiga (hard), 1 wide, 2 narrow, 3 mono
    bool up_jumps = false;    // the pad's up jumps too, as the Amiga joystick's did
    bool ring_out = true;     // sounds the game cuts off fade out instead
    bool rumble = true;       // the pad buzzes when Robocod is hit or loses a life
    int rumble_strength = 5;  // 1..10; 5 is as designed, each step a fifth of that
    bool show_fps = false;    // a frame rate counter in the corner
};

bool operator==(const Settings &a, const Settings &b);
inline bool operator!=(const Settings &a, const Settings &b) { return !(a == b); }

Settings load(const std::string &path);
bool save(const std::string &path, const Settings &s);

} // namespace settings
