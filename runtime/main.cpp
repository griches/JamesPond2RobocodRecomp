// James Pond 2 - Codename RoboCod: desktop host (SDL3).
//
//   jp2 [--adf path] [--headless N] [--shot FRAME:file.png ...] [--input SPEC]
//
// --headless runs N frames with no window or sound, for tests. --input plays
// joystick/key presses inside the program (no keystrokes go to the OS):
//   "120-130:fire,300-400:right+fire,500:key=F1"
#include "amiga.h"
#include "cpu.h"
#include "game.h"
#include "png.h"
#include "record.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace amiga;

namespace {

// The normal PAL screen inside the framebuffer.
constexpr int kViewX = 0x81 - kFbX0, kViewY = 0x2C - kFbY0;
constexpr int kViewW = 320, kViewH = 256;

struct Shot {
    int frame;
    std::string path;
};

struct Press {
    int from, to;
    Joystick joy;
    int key;   // -1 for none
};

int amiga_key_by_name(const std::string &n)
{
    static const struct { const char *name; int code; } names[] = {
        {"ESC", 0x45}, {"SPACE", 0x40}, {"RETURN", 0x44}, {"F1", 0x50}, {"F2", 0x51},
        {"F3", 0x52}, {"F4", 0x53}, {"F5", 0x54}, {"F6", 0x55}, {"F7", 0x56}, {"F8", 0x57},
        {"F9", 0x58}, {"F10", 0x59}, {"P", 0x19}, {"HELP", 0x5F},
    };
    for (auto &e : names)
        if (n == e.name)
            return e.code;
    return -1;
}

std::vector<Press> parse_input(const char *spec)
{
    std::vector<Press> out;
    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? s.size() : comma + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos)
            continue;
        Press p{};
        p.key = -1;
        std::string range = item.substr(0, colon), what = item.substr(colon + 1);
        size_t dash = range.find('-');
        p.from = std::atoi(range.c_str());
        p.to = dash == std::string::npos ? p.from : std::atoi(range.c_str() + dash + 1);
        size_t q = 0;
        while (q <= what.size()) {
            size_t plus = what.find('+', q);
            std::string t = what.substr(q, plus == std::string::npos ? std::string::npos : plus - q);
            q = plus == std::string::npos ? what.size() + 1 : plus + 1;
            if (t == "up") p.joy.up = true;
            else if (t == "down") p.joy.down = true;
            else if (t == "left") p.joy.left = true;
            else if (t == "right") p.joy.right = true;
            else if (t == "fire") p.joy.fire = true;
            else if (t == "fire2") p.joy.fire2 = true;
            else if (t.rfind("key=", 0) == 0) p.key = amiga_key_by_name(t.substr(4));
        }
        out.push_back(p);
    }
    return out;
}

void apply_script(const std::vector<Press> &script, int frame)
{
    Joystick j{};
    for (const Press &p : script) {
        if (frame < p.from || frame > p.to)
            continue;
        j.up |= p.joy.up;
        j.down |= p.joy.down;
        j.left |= p.joy.left;
        j.right |= p.joy.right;
        j.fire |= p.joy.fire;
        j.fire2 |= p.joy.fire2;
        if (p.key >= 0 && frame == p.from)
            key_event(p.key, true);
        if (p.key >= 0 && frame == p.to)
            key_event(p.key, false);
    }
    joy = j;
}

void save_view(const char *path)
{
    write_png(path, &framebuffer[kViewY * kFbW + kViewX], kViewW, kViewH, kFbW);
}

// SDL scancode -> Amiga raw key code, for keys the game might read.
int amiga_key(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10)
        return 0x50 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 0x01 + (sc - SDL_SCANCODE_1);
    static const struct { SDL_Scancode sc; int code; } map[] = {
        {SDL_SCANCODE_0, 0x0A}, {SDL_SCANCODE_ESCAPE, 0x45}, {SDL_SCANCODE_SPACE, 0x40},
        {SDL_SCANCODE_RETURN, 0x44}, {SDL_SCANCODE_BACKSPACE, 0x41}, {SDL_SCANCODE_TAB, 0x42},
        {SDL_SCANCODE_DELETE, 0x46}, {SDL_SCANCODE_LSHIFT, 0x60}, {SDL_SCANCODE_RSHIFT, 0x61},
        {SDL_SCANCODE_LCTRL, 0x63}, {SDL_SCANCODE_LALT, 0x64}, {SDL_SCANCODE_RALT, 0x65},
        {SDL_SCANCODE_Q, 0x10}, {SDL_SCANCODE_W, 0x11}, {SDL_SCANCODE_E, 0x12}, {SDL_SCANCODE_R, 0x13},
        {SDL_SCANCODE_T, 0x14}, {SDL_SCANCODE_Y, 0x15}, {SDL_SCANCODE_U, 0x16}, {SDL_SCANCODE_I, 0x17},
        {SDL_SCANCODE_O, 0x18}, {SDL_SCANCODE_P, 0x19}, {SDL_SCANCODE_A, 0x20}, {SDL_SCANCODE_S, 0x21},
        {SDL_SCANCODE_D, 0x22}, {SDL_SCANCODE_F, 0x23}, {SDL_SCANCODE_G, 0x24}, {SDL_SCANCODE_H, 0x25},
        {SDL_SCANCODE_J, 0x26}, {SDL_SCANCODE_K, 0x27}, {SDL_SCANCODE_L, 0x28}, {SDL_SCANCODE_Z, 0x31},
        {SDL_SCANCODE_X, 0x32}, {SDL_SCANCODE_C, 0x33}, {SDL_SCANCODE_V, 0x34}, {SDL_SCANCODE_B, 0x35},
        {SDL_SCANCODE_N, 0x36}, {SDL_SCANCODE_M, 0x37},
    };
    for (auto &e : map)
        if (e.sc == sc)
            return e.code;
    return -1;
}

int run_headless(int frames, const std::vector<Shot> &shots, const std::vector<Press> &script)
{
    float peak = 0;
    int loud_frames = 0;
    for (int f = 1; f <= frames; f++) {
        apply_script(script, f);
        run_frame();
        record::frame();
        float buf[4096];
        int n;
        float frame_peak = 0;
        while ((n = audio_take(buf, 2048)) > 0)
            for (int i = 0; i < n * 2; i++)
                frame_peak = std::max(frame_peak, std::fabs(buf[i]));
        peak = std::max(peak, frame_peak);
        if (frame_peak > 0.01f)
            loud_frames++;
        for (const Shot &s : shots)
            if (s.frame == f)
                save_view(s.path.c_str());
    }
    std::printf("audio: peak %.3f, %d of %d frames with sound\n", peak, loud_frames, frames);
    record::finish();
    cpu_report();
    return 0;
}

int run_window()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("James Pond 2 - Codename RoboCod", kViewW * 3, kViewH * 3,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderLogicalPresentation(ren, kViewW, kViewH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kViewW, kViewH);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio)
        SDL_ResumeAudioStreamDevice(audio);

    SDL_Gamepad *pad = nullptr;
    Joystick keys{}, padj{};
    bool running = true;
    uint64_t next = SDL_GetTicksNS();
    const uint64_t frame_ns = 1000000000ull / 50;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad)
                    pad = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = e.type == SDL_EVENT_KEY_DOWN;
                SDL_Scancode sc = e.key.scancode;
                if (sc == SDL_SCANCODE_UP) keys.up = down;
                else if (sc == SDL_SCANCODE_DOWN) keys.down = down;
                else if (sc == SDL_SCANCODE_LEFT) keys.left = down;
                else if (sc == SDL_SCANCODE_RIGHT) keys.right = down;
                else if (sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_SPACE) keys.fire = down;
                if (sc != SDL_SCANCODE_UP && sc != SDL_SCANCODE_DOWN && sc != SDL_SCANCODE_LEFT &&
                    sc != SDL_SCANCODE_RIGHT && !e.key.repeat) {
                    int k = amiga_key(sc);
                    if (k >= 0)
                        key_event(k, down);
                }
                break;
            }
            }
        }
        if (pad) {
            const int dead = 12000;
            Sint16 lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            Sint16 ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            padj.left = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead;
            padj.right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead;
            padj.up = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead;
            padj.down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead;
            padj.fire = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
            padj.fire2 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
        }
        joy.up = keys.up || padj.up;
        joy.down = keys.down || padj.down;
        joy.left = keys.left || padj.left;
        joy.right = keys.right || padj.right;
        joy.fire = keys.fire || padj.fire;
        joy.fire2 = keys.fire2 || padj.fire2;

        uint64_t now = SDL_GetTicksNS();
        int ran = 0;
        while (now >= next && ran < 3) {
            run_frame();
            record::frame();
            next += frame_ns;
            ran++;
        }
        if (now > next + frame_ns * 5)
            next = now;
        if (ran) {
            float buf[4096];
            int n;
            while ((n = audio_take(buf, 2048)) > 0)
                if (audio)
                    SDL_PutAudioStreamData(audio, buf, n * 2 * int(sizeof(float)));
            SDL_UpdateTexture(tex, nullptr, &framebuffer[kViewY * kFbW + kViewX], kFbW * 4);
        }
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
    record::finish();
    cpu_report();
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    const char *adf = std::getenv("JP2_ADF");
    int headless = 0;
    std::vector<Shot> shots;
    std::vector<Press> script;
    const char *record_dir = nullptr;
    bool cheat = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--adf") && i + 1 < argc) {
            adf = argv[++i];
        } else if (!std::strcmp(argv[i], "--headless") && i + 1 < argc) {
            headless = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) {
            const char *s = argv[++i];
            const char *colon = std::strchr(s, ':');
            if (colon)
                shots.push_back({std::atoi(s), colon + 1});
        } else if (!std::strcmp(argv[i], "--input") && i + 1 < argc) {
            script = parse_input(argv[++i]);
        } else if (!std::strcmp(argv[i], "--record") && i + 1 < argc) {
            record_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--cheat")) {
            cheat = true;
        }
    }
    // Double-clicked app: the disk image sits beside the program (inside the
    // .app bundle that is Contents/Resources).
    std::string bundled;
    if (!adf) {
        if (const char *base = SDL_GetBasePath()) {
            bundled = std::string(base) + "jp2.adf";
            adf = bundled.c_str();
        }
    }
    if (!adf) {
        std::fprintf(stderr, "usage: %s --adf <James Pond 2 disk image>\n", argv[0]);
        return 1;
    }
    std::string err;
    if (!game::boot(adf, &err)) {
        std::fprintf(stderr, "%s: %s\n", adf, err.c_str());
        if (!headless) {
            std::string msg = std::string("Couldn't start from the disk image:\n") + adf + "\n\n" + err;
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "James Pond 2", msg.c_str(), nullptr);
        }
        return 1;
    }
#ifdef JP2_COVERAGE_DIR
    // Development builds: every play session saves the code that had to be
    // interpreted, so the next recompile can translate it too.
    std::string session_dir;
    if (!record_dir && !headless) {
        char stamp[32];
        std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
        session_dir = std::string(JP2_COVERAGE_DIR) + "/play-" + stamp;
        if (SDL_CreateDirectory(session_dir.c_str()))
            record_dir = session_dir.c_str();
    }
#endif
    if (record_dir)
        record::start(record_dir);
    if (cheat)
        game::enable_cheat();
    if (headless)
        return run_headless(headless, shots, script);
    return run_window();
}
