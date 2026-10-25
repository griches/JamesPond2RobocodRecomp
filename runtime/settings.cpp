#include "settings.h"

#include <algorithm>
#include <cstdio>
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
    {"picture", &Settings::picture, nullptr, 0, 2},
    {"scanlines", &Settings::scanlines, nullptr, 0, 2},
    {"fullscreen", nullptr, &Settings::fullscreen, 0, 1},
    {"volume", &Settings::volume, nullptr, 0, 10},
    {"sound_model", &Settings::sound_model, nullptr, 0, 2},
    {"stereo", &Settings::stereo, nullptr, 0, 3},
    {"up_jumps", nullptr, &Settings::up_jumps, 0, 1},
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
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
        return s;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
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
    std::fclose(f);
    return s;
}

bool save(const std::string &path, const Settings &s)
{
    std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    for (const Field &fd : kFields)
        std::fprintf(f, "%s=%d\n", fd.name, fd.i ? s.*fd.i : int(s.*fd.b));
    bool ok = std::fclose(f) == 0;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace settings
