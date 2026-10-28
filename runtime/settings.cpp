#include "settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace settings {

namespace {

// Each setting once: its name in the file, where it lives, and its range.
struct Field {
    const char *name;
    int Settings::*i;
    bool Settings::*b;
    int lo, hi;
};
const Field kFields[] = {
    {"widescreen", nullptr, &Settings::widescreen, 0, 1},
    {"smooth_motion", nullptr, &Settings::smooth, 0, 1},
    {"picture", &Settings::picture, nullptr, 0, 3},
    {"scanlines", &Settings::scanlines, nullptr, 0, 2},
    {"fullscreen", nullptr, &Settings::fullscreen, 0, 1},
    {"volume", &Settings::volume, nullptr, 0, 10},
    {"sound_model", &Settings::sound_model, nullptr, 0, 2},
    {"stereo", &Settings::stereo, nullptr, 0, 3},
    {"up_jumps", nullptr, &Settings::up_jumps, 0, 1},
    {"ring_out", nullptr, &Settings::ring_out, 0, 1},
    {"rumble", nullptr, &Settings::rumble, 0, 1},
    {"rumble_strength", &Settings::rumble_strength, nullptr, 1, 10},
    {"show_fps", nullptr, &Settings::show_fps, 0, 1},
};

} // namespace

bool operator==(const Settings &a, const Settings &b)
{
    for (const Field &f : kFields)
        if (f.i ? a.*f.i != b.*f.i : a.*f.b != b.*f.b)
            return false;
    return true;
}

Settings load(const std::string &path)
{
    Settings s;
    // SDL's files: they take UTF-8 paths on Windows too, where fopen doesn't.
    char *text = static_cast<char *>(SDL_LoadFile(path.c_str(), nullptr));
    if (!text)
        return s;
    for (char *line = text, *next; *line; line = next) {
        next = line + std::strcspn(line, "\n");
        if (*next)
            *next++ = 0;
        char *eq = std::strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        int v = std::atoi(eq + 1);
        for (const Field &fd : kFields) {
            if (std::strcmp(line, fd.name) != 0)
                continue;
            v = std::clamp(v, fd.lo, fd.hi);
            if (fd.i)
                s.*fd.i = v;
            else
                s.*fd.b = v != 0;
        }
    }
    SDL_free(text);
    return s;
}

bool save(const std::string &path, const Settings &s)
{
    std::string tmp = path + ".tmp";
    SDL_IOStream *f = SDL_IOFromFile(tmp.c_str(), "wb");
    if (!f)
        return false;
    for (const Field &fd : kFields)
        SDL_IOprintf(f, "%s=%d\n", fd.name, fd.i ? s.*fd.i : int(s.*fd.b));
    bool ok = SDL_CloseIO(f);
    // Not std::rename: on Windows it won't replace a file that's already there.
    return ok && SDL_RenamePath(tmp.c_str(), path.c_str());
}

} // namespace settings
