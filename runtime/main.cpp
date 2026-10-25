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
#include "overlay.h"
#include "png.h"
#include "record.h"
#include "state.h"
#include "trace.h"
#include "wide.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
    bool wide;
};

constexpr int kWideW = 368;   // 16:9 at the 208-line gameplay window

struct Press {
    int from, to;
    Joystick joy;
    int key;   // -1 for none
};

// Amiga raw key code for a name: a letter, a digit, F1-F10, or a few others.
int amiga_key_by_name(const std::string &n)
{
    static const struct { const char *name; int code; } names[] = {
        {"ESC", 0x45}, {"SPACE", 0x40}, {"RETURN", 0x44}, {"ENTER", 0x44}, {"HELP", 0x5F}, {"DEL", 0x46},
        {"BACKSPACE", 0x41}, {"TAB", 0x42},
    };
    for (auto &e : names)
        if (n == e.name)
            return e.code;
    if (n.size() >= 2 && n[0] == 'F') {
        int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 10)
            return 0x4F + f;
    }
    if (n.size() == 1) {
        static const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        static const int first[] = {0x01, 0x10, 0x20, 0x31};
        char c = char(std::toupper(static_cast<unsigned char>(n[0])));
        for (int r = 0; r < 4; r++)
            if (const char *p = std::strchr(rows[r], c))
                return first[r] + int(p - rows[r]);
        if (c == ' ')
            return 0x40;
    }
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
        if (what.rfind("type=", 0) == 0) {
            int at = std::atoi(range.c_str());
            std::string text = what.substr(5);
            for (size_t i = 0; i < text.size(); i++) {
                Press k{};
                k.from = at + int(i) * 4;
                k.to = k.from + 2;
                k.key = amiga_key_by_name(std::string(1, text[i]));
                if (k.key >= 0)
                    out.push_back(k);
            }
            continue;
        }
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

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool write_file(const std::string &path, const std::vector<uint8_t> &data)
{
    std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

// The quick save lives with the user's app data, so it outlasts rebuilds.
std::string quicksave_path()
{
    static std::string path;
    if (path.empty()) {
        if (char *dir = SDL_GetPrefPath("bouncingball", "James Pond 2")) {
            path = std::string(dir) + "quicksave.jp2s";
            SDL_free(dir);
        }
    }
    return path;
}

// Returns the message to show.
const char *quick_save(const std::string &path)
{
    std::vector<uint8_t> data;
    return save_state(data) && !path.empty() && write_file(path, data) ? "QUICK SAVED" : "SAVE FAILED";
}

const char *quick_load(const std::string &path)
{
    std::vector<uint8_t> data;
    if (path.empty() || !read_file(path, data))
        return "NO QUICK SAVE YET";
    std::string err;
    if (!load_state(data, &err)) {
        std::fprintf(stderr, "quick load: %s\n", err.c_str());
        return "LOAD FAILED";
    }
    return "LOADED";
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

int run_headless(int frames, const std::vector<Shot> &shots, const std::vector<Press> &script,
                 const std::string &load_from, int save_frame, const std::string &save_to)
{
    if (!load_from.empty()) {
        std::printf("load %s: %s\n", load_from.c_str(), quick_load(load_from));
        if (std::getenv("JP2_SHOWREGS"))
            std::printf("  pc %06X sr %04X a7 %08X d0 %08X a0 %08X\n", cpu_reg(PC), cpu_reg(SR), cpu_reg(A7), cpu_reg(D0),
                        cpu_reg(A0));
    }
    float peak = 0;
    int loud_frames = 0;
    for (int f = 1; f <= frames; f++) {
        apply_script(script, f);
        trace::frame_begin(f);
        run_frame();
        trace::frame_end();
        wide::frame_done();
        if (std::getenv("JP2_WIDECHECK"))
            wide::self_check();
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
        for (const Shot &s : shots) {
            if (s.frame != f)
                continue;
            static uint32_t wide_px[kWideW * wide::kHeight];
            if (s.wide && wide::render(wide_px, kWideW, 1.0f, std::getenv("JP2_FULLSHOT") != nullptr))
                write_png(s.path.c_str(), wide_px, kWideW, wide::kHeight, kWideW);
            else
                save_view(s.path.c_str());
        }
        if (f == save_frame)
            std::printf("save at frame %d: %s\n", f, quick_save(save_to));
    }
    std::printf("audio: peak %.3f, %d of %d frames with sound\n", peak, loud_frames, frames);
    record::finish();
    cpu_report();
    return 0;
}

// Widescreen width that fills a window of this shape (the game window is 208
// lines tall): 334 on the Steam Deck's 16:10, 370 at 16:9.
int wide_width_for(int pw, int ph)
{
    if (pw <= 0 || ph <= 0)
        return kWideW;
    int w = int(208.0 * pw / ph + 0.5) & ~1;
    return w < 288 ? 288 : w > wide::kMaxWidth ? wide::kMaxWidth : w;
}

int run_window(bool fullscreen, const std::string &load_from)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("James Pond 2 - Codename RoboCod", kViewW * 3, kViewH * 3,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (fullscreen)
        SDL_SetWindowFullscreen(win, true);
    SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderLogicalPresentation(ren, kViewW, kViewH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kViewW, kViewH);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    int wide_w = 0;
    SDL_Texture *wide_tex = nullptr;
    std::vector<uint32_t> wide_px;
    bool widescreen = true, showing_wide = false;
    bool smooth = true;   // redraw between 50 Hz ticks at the display's rate


    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio)
        SDL_ResumeAudioStreamDevice(audio);

    SDL_Gamepad *pad = nullptr;
    Joystick keys{}, padj{};
    bool running = true;
    bool want_save = false, want_load = false;
    bool digits_to_game = false;
    const char *message = nullptr;
    uint64_t message_until = 0;
    std::vector<uint32_t> present(size_t(kViewW) * kViewH);
    long frames_run = 0;
    if (!load_from.empty()) {   // start from a save (--load)
        message = quick_load(load_from);
        message_until = SDL_GetTicksNS() + 2000000000ull;
    }
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
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                // L: quick save. R: quick load. Y: smooth motion on / off.
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                    want_save = true;
                } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                    want_load = true;
                } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) {
                    smooth = !smooth;
                    message = smooth ? "SMOOTH MOTION ON" : "SMOOTH MOTION OFF";
                    message_until = SDL_GetTicksNS() + 2000000000ull;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = e.type == SDL_EVENT_KEY_DOWN;
                SDL_Scancode sc = e.key.scancode;
                // The cheat map selector (M) wants a section number typed on the
                // number keys: hand them to the game until Return or Esc.
                if (down && !e.key.repeat) {
                    if (sc == SDL_SCANCODE_M)
                        digits_to_game = true;
                    else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_ESCAPE)
                        digits_to_game = false;
                }
                // Quick save 1 (or F5), quick load 2 (or F8): ours, not the game's.
                // Its cheat mode uses letters, Enter, F6, F7, F9 and F10.
                if (sc == SDL_SCANCODE_4 && !digits_to_game) {   // smooth motion on / off
                    if (down && !e.key.repeat) {
                        smooth = !smooth;
                        message = smooth ? "SMOOTH MOTION ON" : "SMOOTH MOTION OFF";
                        message_until = SDL_GetTicksNS() + 2000000000ull;
                    }
                    break;
                }
                if (sc == SDL_SCANCODE_3 && !digits_to_game) {   // widescreen on / off
                    if (down && !e.key.repeat) {
                        widescreen = !widescreen;
                        wide::set_view_extra(widescreen ? (wide_w - 288) / 2 : 0);
                        message = widescreen ? "WIDESCREEN ON" : "WIDESCREEN OFF";
                        message_until = SDL_GetTicksNS() + 2000000000ull;
                    }
                    break;
                }
                bool digit = sc == SDL_SCANCODE_1 || sc == SDL_SCANCODE_2;
                if ((digit && !digits_to_game) || sc == SDL_SCANCODE_F5 || sc == SDL_SCANCODE_F8) {
                    if (down && !e.key.repeat)
                        (sc == SDL_SCANCODE_1 || sc == SDL_SCANCODE_F5 ? want_save : want_load) = true;
                    break;
                }
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
            // D-pad and left stick move. In a level A jumps (the joystick's up)
            // and B or X stretches (the fire button); on titles and menus A
            // is fire too, so it starts the game.
            bool a = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
            bool bx = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
            bool in_level = wide::playing();
            padj.left = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead;
            padj.right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead;
            padj.up = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead || (a && in_level);
            padj.down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead;
            padj.fire = bx || (a && !in_level);
            padj.fire2 = false;
        }
        joy.up = keys.up || padj.up;
        joy.down = keys.down || padj.down;
        joy.left = keys.left || padj.left;
        joy.right = keys.right || padj.right;
        joy.fire = keys.fire || padj.fire;
        joy.fire2 = keys.fire2 || padj.fire2;

        uint64_t now = SDL_GetTicksNS();
        // Between frames, so the machine is in a steady state.
        if (want_save || want_load) {
            message = want_save ? quick_save(quicksave_path()) : quick_load(quicksave_path());
            message_until = now + 2000000000ull;
            want_save = want_load = false;
        }
        int ran = 0;
        while (now >= next && ran < 3) {
            run_frame();
            wide::frame_done();
            record::frame();
            if (++frames_run % 3000 == 0)
                record::flush();   // keep the play recording even if the game is killed
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
        }
        // Fit the widescreen picture to the window's shape.
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (int want = wide_width_for(pw, ph); want != wide_w) {
            wide_w = want;
            if (wide_tex)
                SDL_DestroyTexture(wide_tex);
            wide_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, wide_w,
                                         wide::kHeight);
            SDL_SetTextureScaleMode(wide_tex, SDL_SCALEMODE_NEAREST);
            wide_px.assign(size_t(wide_w) * wide::kHeight, 0);
            wide::set_view_extra(widescreen ? (wide_w - 288) / 2 : 0);
        }
        // Where we are between the last game tick and the next (0..1).
        float alpha = 1.0f - float(int64_t(next - now)) / float(frame_ns);
        alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
        if (ran || message || (smooth && showing_wide)) {
            bool show_message = message && now < message_until;
            if (!show_message)
                message = nullptr;
            // Always the full redraw (it matches the game's own picture), moved
            // between ticks only with smoothing on.
            showing_wide = widescreen && wide::render(wide_px.data(), wide_w, smooth ? alpha : 1.0f, true);
            if (showing_wide) {
                if (show_message)
                    draw_message(wide_px.data(), wide_w, wide::kHeight, message);
                SDL_UpdateTexture(wide_tex, nullptr, wide_px.data(), wide_w * 4);
            } else {
                for (int y = 0; y < kViewH; y++)
                    std::memcpy(&present[size_t(y) * kViewW], &framebuffer[(kViewY + y) * kFbW + kViewX], kViewW * 4);
                if (show_message)
                    draw_message(present.data(), kViewW, kViewH, message);
                SDL_UpdateTexture(tex, nullptr, present.data(), kViewW * 4);
            }
        }
        // Widescreen play fills a 16:9 window; everything else keeps the 4:3 picture.
        SDL_SetRenderLogicalPresentation(ren, showing_wide ? wide_w : kViewW, showing_wide ? wide::kHeight : kViewH,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, showing_wide ? wide_tex : tex, nullptr, nullptr);
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
    std::string load_from, save_to;
    int save_frame = -1;
    bool fullscreen = std::getenv("JP2_FULLSCREEN") != nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--adf") && i + 1 < argc) {
            adf = argv[++i];
        } else if (!std::strcmp(argv[i], "--headless") && i + 1 < argc) {
            headless = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) {
            const char *s = argv[++i];
            const char *colon = std::strchr(s, ':');
            if (colon)
                shots.push_back({std::atoi(s), colon + 1, false});
        } else if (!std::strcmp(argv[i], "--wide-shot") && i + 1 < argc) {
            const char *s = argv[++i];
            if (const char *colon = std::strchr(s, ':'))
                shots.push_back({std::atoi(s), colon + 1, true});
        } else if (!std::strcmp(argv[i], "--input") && i + 1 < argc) {
            script = parse_input(argv[++i]);
        } else if (!std::strcmp(argv[i], "--record") && i + 1 < argc) {
            record_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--cheat")) {
            cheat = true;
        } else if (!std::strcmp(argv[i], "--fullscreen")) {
            fullscreen = true;
        } else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) {
            const char *v = argv[++i];
            if (const char *colon = std::strchr(v, ':'))
                trace::arm(std::atoi(v), colon + 1);
        } else if (!std::strcmp(argv[i], "--load") && i + 1 < argc) {
            load_from = argv[++i];
        } else if (!std::strcmp(argv[i], "--save-at") && i + 1 < argc) {
            const char *v = argv[++i];
            const char *colon = std::strchr(v, ':');
            if (colon) {
                save_frame = std::atoi(v);
                save_to = colon + 1;
            }
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
    if (std::getenv("JP2_WIDE") || std::any_of(shots.begin(), shots.end(), [](const Shot &s) { return s.wide; }))
        wide::set_view_extra((kWideW - 288) / 2);
    if (record_dir)
        record::start(record_dir);
    if (cheat)
        game::enable_cheat();
    if (headless)
        return run_headless(headless, shots, script, load_from, save_frame, save_to);
    return run_window(fullscreen, load_from);
}
