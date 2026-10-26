// James Pond 2 - Codename RoboCod: desktop host (SDL3).
//
//   jp2 [--adf path] [--headless N] [--shot FRAME:file.png ...] [--input SPEC]
//
// --headless runs N frames with no window or sound, for tests. --input plays
// joystick/key presses inside the program (no keystrokes go to the OS):
//   "120-130:fire,300-400:right+fire,500:key=F1"
// and, for tests, writes and reads of game memory (hex):
//   "300:poke=2552=0001,900:peek=251a+2284"
#include "amiga.h"
#include "cpu.h"
#include "disk.h"
#include "game.h"
#include "glyphs.h"
#include "menu.h"
#include "overlay.h"
#include "png.h"
#include "record.h"
#include "settings.h"
#include "setup.h"
#include "state.h"
#include "trace.h"
#include "ui.h"
#include "wide.h"

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <SDL3/SDL_main.h>   // WinMain for a windowed program, no console
#endif

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
constexpr int kGameW = 288;   // the game's own window: what widescreen off shows

struct Press {
    int from, to;
    Joystick joy;
    int key;   // -1 for none
    uint32_t poke_addr = 0, poke_value = 0;
    int poke_size = 0;              // bytes; 0 for none
    std::vector<uint32_t> peeks;    // words to print at `from`
};

// Amiga raw key code for a name: a letter, a digit, F1-F10, or a few others.
int amiga_key_by_name(const std::string &n)
{
    static const struct { const char *name; int code; } names[] = {
        {"ESC", 0x45}, {"SPACE", 0x40}, {"RETURN", 0x44}, {"ENTER", 0x44}, {"KPENTER", 0x43}, {"HELP", 0x5F}, {"DEL", 0x46},
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
        if (what.rfind("peek=", 0) == 0) {   // "F:peek=251a+2248": print those words at frame F
            Press k{};
            k.key = -1;
            k.from = k.to = std::atoi(range.c_str());
            std::string v = what.substr(5);
            size_t q = 0;
            while (q < v.size()) {
                size_t plus = v.find('+', q);
                std::string a = v.substr(q, plus == std::string::npos ? std::string::npos : plus - q);
                k.peeks.push_back(uint32_t(std::strtoul(a.c_str(), nullptr, 16)));
                q = plus == std::string::npos ? v.size() : plus + 1;
            }
            out.push_back(k);
            continue;
        }
        if (what.rfind("poke=", 0) == 0) {   // "F:poke=2552=0001": hex address = hex value (2, 4 or 8 digits)
            Press k{};
            k.key = -1;
            k.from = k.to = std::atoi(range.c_str());
            std::string v = what.substr(5);
            size_t eq = v.find('=');
            if (eq != std::string::npos) {
                k.poke_addr = uint32_t(std::strtoul(v.substr(0, eq).c_str(), nullptr, 16));
                std::string val = v.substr(eq + 1);
                k.poke_value = uint32_t(std::strtoul(val.c_str(), nullptr, 16));
                k.poke_size = int(val.size() / 2);
                out.push_back(k);
            }
            continue;
        }
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
        if (!p.peeks.empty() && frame == p.from) {
            std::printf("peek %d:", frame);
            for (uint32_t a : p.peeks)
                std::printf(" %X=%04X", a, read16(a));
            std::printf("\n");
        }
        if (p.poke_size && frame == p.from) {
            if (p.poke_size == 1)
                write8(p.poke_addr, uint8_t(p.poke_value));
            else if (p.poke_size == 2)
                write16(p.poke_addr, uint16_t(p.poke_value));
            else
                write32(p.poke_addr, p.poke_value);
        }
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

// Saves and settings live with the user's app data, so they outlast rebuilds.
std::string pref_dir()
{
    static std::string dir;
    if (dir.empty() && std::getenv("JP2_PREF_DIR"))   // tests: keep away from the real saves
        dir = std::string(std::getenv("JP2_PREF_DIR")) + "/";
    if (dir.empty()) {
        if (char *d = SDL_GetPrefPath("bouncingball", "James Pond 2")) {
            dir = d;
            SDL_free(d);
        }
    }
    return dir;
}

std::string quicksave_path()
{
    std::string dir = pref_dir();
    return dir.empty() ? dir : dir + "quicksave.jp2s";
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

// The game's explosion sound (a boss going up, for one): the same sample in
// every level. Counted as it starts, for the pad to thump with each boom.
constexpr uint32_t kBoomSample = 0x3CC98;
constexpr uint32_t kBoomCrc = 0x9AAE7089;   // of its first 64 bytes
int booms_heard;
void heard_sample(int, uint32_t lc, uint16_t)
{
    if ((lc & ~1u) == kBoomSample && disk::crc32(&ram[kBoomSample], 64) == kBoomCrc)
        booms_heard++;
}

// What the pad should feel after a frame: 1 Robocod was hit (his energy
// dropped), 2 he lost a life, 3 an explosion (the boom sound, or an enemy
// bursting into a star, which makes no sound of its own). A boss going up
// sets off a boom and a cluster of stars over a few frames: one thump for the
// lot. Quiet after a load or a restart.
struct Feel {
    bool known = false;
    uint16_t energy = 0, lives = 0;
    int booms = 0, stars = 0;
    int ticks = 0, thumped_at = -100;

    int check(bool in_level)
    {
        uint16_t e = read16(0x2286), l = read16(0x2284);   // energy, lives
        bool bang = booms_heard != booms || wide::bursts() != stars;
        int what = 0;
        ticks++;
        if (known && in_level)
            what = l < lives ? 2 : e < energy ? 1 : bang && ticks - thumped_at >= 4 ? 3 : 0;
        if (what == 3)
            thumped_at = ticks;
        energy = e;
        lives = l;
        booms = booms_heard;
        stars = wide::bursts();
        known = in_level;
        return what;
    }

    // How hard the game is shaking the screen now, 0..1: it counts frames
    // down in $AAC (sideways) and $AAE (up and down) through its table of
    // offsets at $FC0A, biggest first (a landing from a long fall, a boss).
    float shake() const
    {
        int most = 0;
        for (uint32_t counter : {0xAACu, 0xAAEu})
            if (int n = read16(counter); n > 0 && n < 48)
                most = std::max(most, std::abs(int(int8_t(read8(0xFC0A + uint32_t(n))))));
        return std::min(1.0f, float(most) / 15.0f);
    }
};

// What the pad's motors should be doing, worked out each frame and sent only
// when it changes. SDL treats a request at the strength already running as
// "keep going" and sends nothing, so back-to-back booms at one strength felt
// like a single long one: each boom is now its own pulse with a gap before
// the next (they queue up when the game sets several off at once), and the
// shake drops away under the pulses so they stand out.
struct Rumble {
    static constexpr uint64_t ms = 1000000ull;
    static constexpr uint64_t kPulse = 90 * ms, kGap = 60 * ms;   // a boom: on, then off before the next
    uint64_t knock_until = 0;   // a hit's or a lost life's buzz
    uint16_t knock_lo = 0, knock_hi = 0;
    int booms_waiting = 0, booms_played = 0;
    uint64_t boom_until = 0, boom_next = 0;
    uint16_t sent_lo = 0, sent_hi = 0;
    uint64_t sent_at = 0;

    void felt(int what, uint64_t now)
    {
        if (what == 3) {
            booms_waiting = std::min(booms_waiting + 1, 3);   // never fall far behind the explosions
        } else if (what) {
            knock_until = now + (what == 2 ? 420 : 170) * ms;
            knock_lo = what == 2 ? 0xD000 : 0x7000;
            knock_hi = what == 2 ? 0x9000 : 0xB000;
        }
    }

    // What to send now, if anything: true with the strengths and how long
    // they hold if not changed again (a lost frame never leaves it running).
    // A motor strength at the player's setting: 5 as designed, each step a
    // fifth of that, as far as the motor goes.
    static uint16_t at_strength(uint32_t v, int strength) { return uint16_t(std::min<uint32_t>(0xFFFF, v * strength / 5)); }

    bool step(bool on, float shake, int strength, uint64_t now, uint16_t &lo, uint16_t &hi, uint32_t &hold_ms)
    {
        lo = hi = 0;
        if (!on) {
            booms_waiting = 0;
        } else if (now < knock_until) {
            lo = knock_lo, hi = knock_hi;
        } else {
            if (now >= boom_until && booms_waiting && now >= boom_next) {
                booms_waiting--;
                booms_played++;
                boom_until = now + kPulse;
                boom_next = boom_until + kGap;
            }
            if (now < boom_until) {   // a deep thump, a hair different from the one before
                lo = uint16_t(0xFFFF - (booms_played & 1));
                hi = uint16_t(0x5000 + (booms_played & 1));
            } else if (shake > 0) {
                float s = now < boom_next || booms_waiting ? shake * 0.3f : shake;   // quiet between booms
                lo = uint16_t(s * 0xE000);
                hi = uint16_t(s * 0x8000);
            }
        }
        lo = at_strength(lo, strength);
        hi = at_strength(hi, strength);
        bool changed = lo != sent_lo || hi != sent_hi;
        bool refresh = (lo || hi) && now >= sent_at + 150 * ms;
        if (!changed && !refresh)
            return false;
        sent_lo = lo, sent_hi = hi, sent_at = now;
        hold_ms = lo || hi ? 300 : 0;
        return true;
    }
};

// 16-bit stereo WAV of what the speakers got.
bool write_wav(const std::string &path, const std::vector<float> &lr)
{
    std::vector<uint8_t> out(44 + lr.size() * 2);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&out[at], &v, 2); };
    std::memcpy(&out[0], "RIFF", 4);
    put32(4, uint32_t(out.size() - 8));
    std::memcpy(&out[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, kAudioRate);
    put32(28, kAudioRate * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(&out[36], "data", 4);
    put32(40, uint32_t(lr.size() * 2));
    for (size_t i = 0; i < lr.size(); i++)
        put16(44 + i * 2, uint16_t(int16_t(std::lround(std::clamp(lr[i], -1.0f, 1.0f) * 32767))));
    return write_file(path, out);
}

void picture_shot(const std::string &spec);
std::vector<std::string> picture_shots;   // --picture-shot, taken after the last frame

// --poke FRAME:ADDR=VALUE (hex address and value; word unless the value has
// more than 4 digits): a memory write before that frame, for tests.
struct Poke {
    int frame;
    uint32_t addr, value;
    bool longword;
};
std::vector<Poke> pokes;
std::vector<std::pair<int, int>> section_jumps;   // --section FRAME:N (hex N), for tests
void go_to_section(int n);

// The menu drawn off screen: "page,row,rows,dialog,width,height,style:file.png"
// (style 0 Steam Deck, 1 Xbox, 2 PlayStation, 3 Nintendo, 4 keyboard).
void menu_shot(const std::string &spec)
{
    size_t colon = spec.find(':');
    if (colon == std::string::npos)
        return;
    int page = 0, row = 0, rows = 0, dialog = 0, w = 1920, h = 1080, style = 0;
    std::sscanf(spec.c_str(), "%d,%d,%d,%d,%d,%d,%d", &page, &row, &rows, &dialog, &w, &h, &style);
    SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *ren = SDL_CreateSoftwareRenderer(surf);
    ui::init(ren);
    static settings::Settings set;
    menu::View v;
    v.settings = &set;
    v.in_level = true;
    v.invincible = true;
    v.slots[0].exists = true;
    v.slots[0].when = "Today 16:42";
    v.slots[2].exists = true;
    v.slots[2].when = "Yesterday 21:05";
    v.score = 240900;
    v.section = 0x05;
    v.lives = 3;
    v.energy = 4;
    v.level_l = 0.62f;
    v.level_r = 0.48f;
    v.pad = style != 4;
    v.style = glyphs::Style(std::min(style, 3));
    menu::show_for_test(page, row, rows != 0, dialog != 0);
    for (int i = 0; i < 3; i++) {
        ui::begin(w, h);
        ui::fill_window(ui::rgb(0x0A101C));
        menu::draw(v, 1.0f);
    }
    SDL_RenderPresent(ren);
    write_png(spec.c_str() + colon + 1, static_cast<uint32_t *>(surf->pixels), w, h, surf->pitch / 4);
    SDL_DestroyRenderer(ren);
    SDL_DestroySurface(surf);
}

// Width of --wide-shot pictures: 16:9, or JP2_SHOT_WIDTH (up to 512).
int shot_width()
{
    const char *w = std::getenv("JP2_SHOT_WIDTH");
    return w ? std::clamp(std::atoi(w) & ~1, 288, wide::kMaxWidth) : kWideW;
}

int run_headless(int frames, const std::vector<Shot> &shots, const std::vector<Press> &script,
                 const std::string &load_from, int save_frame, const std::string &save_to,
                 const std::string &wav_path)
{
    std::vector<float> wav;
    if (!load_from.empty()) {
        std::printf("load %s: %s\n", load_from.c_str(), quick_load(load_from));
        if (std::getenv("JP2_SHOWREGS"))
            std::printf("  pc %06X sr %04X a7 %08X d0 %08X a0 %08X\n", cpu_reg(PC), cpu_reg(SR), cpu_reg(A7), cpu_reg(D0),
                        cpu_reg(A0));
    }
    float peak = 0;
    int loud_frames = 0;
    Feel feel;
    for (int f = 1; f <= frames; f++) {
        apply_script(script, f);
        for (auto [at, n] : section_jumps) {
            if (at == f)
                go_to_section(n);
            if (at + 150 == f)
                std::printf("section %02X asked at frame %d: 150 frames on, section %02X, %s\n", n, at,
                            read16(0x251A), wide::playing() ? "playing" : "not playing");
        }
        for (const Poke &p : pokes)
            if (p.frame == f) {
                if (p.longword)
                    write32(p.addr, p.value);
                else
                    write16(p.addr, uint16_t(p.value));
            }
        trace::frame_begin(f);
        watch_frame = f;
        uint64_t t0 = SDL_GetTicksNS();
        run_frame();
        trace::frame_end();
        uint64_t t1 = SDL_GetTicksNS();
        wide::frame_done();
        if (std::getenv("JP2_PARVIEW")) {   // the in-between pictures smooth motion draws, a quarter tick apart
            static std::vector<uint32_t> px(size_t(wide::kMaxWidth) * wide::kHeight);
            for (float a : {0.25f, 0.5f, 0.75f, 1.0f})
                wide::render(px.data(), shot_width(), a, true);
        }
        if (std::getenv("JP2_FRAMETIME")) {
            // What one tick costs: the game, then the widescreen redraw the app does.
            static std::vector<uint32_t> px(size_t(kWideW) * wide::kHeight);
            uint64_t t2 = SDL_GetTicksNS();
            wide::render(px.data(), kWideW, 0.5f, true);
            uint64_t t3 = SDL_GetTicksNS();
            std::printf("frametime %d: game %.2f ms  bookkeeping %.2f ms  redraw %.2f ms\n", f, (t1 - t0) / 1e6,
                        (t2 - t1) / 1e6, (t3 - t2) / 1e6);
        }
        if (std::getenv("JP2_SHAKELOG"))
            std::printf("shake %d: counters %d,%d  speed %08X  cam %d,%d\n", f, read16(0xAAC), read16(0xAAE),
                        read32(0x22C4), int16_t(read16(0xAB0)), int16_t(read16(0xAB2)));
        if (std::getenv("JP2_RUMBLELOG")) {   // what the pad would be told, on the game's clock
            static Rumble rumble;
            uint64_t at = uint64_t(f) * 20 * Rumble::ms;
            if (int what = feel.check(wide::playing())) {
                std::printf("rumble at frame %d: %s\n", f, what == 2 ? "lost a life" : what == 3 ? "explosion" : "hit");
                rumble.felt(what, at);
            }
            // The window steps once per 5 ms or so; do the same between frames.
            for (uint64_t t = at; t < at + 20 * Rumble::ms; t += 5 * Rumble::ms) {
                uint16_t lo, hi;
                uint32_t hold;
                if (rumble.step(true, feel.shake(), 5, t, lo, hi, hold))
                    std::printf("  pad at %6.3f s: %04X %04X for %u ms\n", t / 1e9, lo, hi, hold);
            }
        }
        if (std::getenv("JP2_WIDECHECK"))
            wide::self_check();
        if (std::getenv("JP2_FLICKER"))
            wide::flicker_check();
        record::frame();
        float buf[4096];
        int n;
        float frame_peak = 0;
        while ((n = audio_take(buf, 2048)) > 0) {
            for (int i = 0; i < n * 2; i++)
                frame_peak = std::max(frame_peak, std::fabs(buf[i]));
            if (!wav_path.empty())
                wav.insert(wav.end(), buf, buf + n * 2);
        }
        peak = std::max(peak, frame_peak);
        if (frame_peak > 0.01f)
            loud_frames++;
        for (const Shot &s : shots) {
            if (s.frame != f)
                continue;
            // "--shot F:name.diff.png": the game's window, the redraw and where they differ.
            if (s.path.size() > 9 && s.path.compare(s.path.size() - 9, 9, ".diff.png") == 0) {
                wide::diff_png(s.path.c_str());
                continue;
            }
            static uint32_t wide_px[wide::kMaxWidth * wide::kHeight];
            if (s.wide && wide::render(wide_px, shot_width(), 1.0f, std::getenv("JP2_FULLSHOT") != nullptr))
                write_png(s.path.c_str(), wide_px, shot_width(), wide::kHeight, shot_width());
            else
                save_view(s.path.c_str());
        }
        if (f == save_frame)
            std::printf("save at frame %d: %s\n", f, quick_save(save_to));
    }
    for (const std::string &p : picture_shots)
        picture_shot(p);
    std::printf("audio: peak %.3f, %d of %d frames with sound\n", peak, loud_frames, frames);
    if (!wav_path.empty())
        std::printf("wav %s: %s\n", wav_path.c_str(), write_wav(wav_path, wav) ? "written" : "FAILED");
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

// ---- save slots ------------------------------------------------------------------

// Where the game was left when it was last quit mid-level.
std::string resume_path() { return pref_dir() + "resume.jp2s"; }

std::string slot_path(int n)
{
    return n == 0 ? quicksave_path() : pref_dir() + "slot" + std::to_string(n) + ".jp2s";
}

// A small picture of the game beside each save, for the menu: 'JP2T',
// width and height (16-bit), then ARGB pixels.
constexpr int kThumbW = 192, kThumbH = 108;

void write_thumb(const std::string &save, const uint32_t *px, int w, int h)
{
    // Cut the middle 16:9 out of the picture and average it down.
    int cw = w, ch = h;
    if (w * 9 > h * 16)
        cw = h * 16 / 9;
    else
        ch = w * 9 / 16;
    int x0 = (w - cw) / 2, y0 = (h - ch) / 2;
    std::vector<uint8_t> out(8 + size_t(kThumbW) * kThumbH * 4);
    std::memcpy(out.data(), "JP2T", 4);
    uint16_t dims[2] = {kThumbW, kThumbH};
    std::memcpy(&out[4], dims, 4);
    uint32_t *dst = reinterpret_cast<uint32_t *>(&out[8]);
    for (int ty = 0; ty < kThumbH; ty++)
        for (int tx = 0; tx < kThumbW; tx++) {
            int sx0 = x0 + tx * cw / kThumbW, sx1 = std::max(sx0 + 1, x0 + (tx + 1) * cw / kThumbW);
            int sy0 = y0 + ty * ch / kThumbH, sy1 = std::max(sy0 + 1, y0 + (ty + 1) * ch / kThumbH);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = sy0; y < sy1; y++)
                for (int x = sx0; x < sx1; x++) {
                    uint32_t p = px[size_t(y) * w + x];
                    r += (p >> 16) & 255;
                    g += (p >> 8) & 255;
                    b += p & 255;
                    n++;
                }
            dst[ty * kThumbW + tx] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | b / n;
        }
    write_file(save + ".thumb", out);
}

SDL_Texture *load_thumb(SDL_Renderer *ren, const std::string &save)
{
    std::vector<uint8_t> in;
    if (!read_file(save + ".thumb", in) || in.size() < 8 || std::memcmp(in.data(), "JP2T", 4) != 0)
        return nullptr;
    uint16_t dims[2];
    std::memcpy(dims, &in[4], 4);
    if (in.size() != 8 + size_t(dims[0]) * dims[1] * 4)
        return nullptr;
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, dims[0], dims[1]);
    if (t) {
        SDL_UpdateTexture(t, nullptr, &in[8], dims[0] * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    return t;
}

// "Today 16:42", "Yesterday 09:10" or "24 Oct 16:42".
std::string when_text(SDL_Time t)
{
    SDL_DateTime d, now;
    SDL_Time cur = 0;
    SDL_GetCurrentTime(&cur);
    if (!SDL_TimeToDateTime(t, &d, true) || !SDL_TimeToDateTime(cur, &now, true))
        return "";
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day_now = SDL_GetDayOfYear(now.year, now.month, now.day), day = SDL_GetDayOfYear(d.year, d.month, d.day);
    char buf[64];
    if (d.year == now.year && day == day_now)
        std::snprintf(buf, sizeof buf, "Today %02d:%02d", d.hour, d.minute);
    else if (d.year == now.year && day == day_now - 1)
        std::snprintf(buf, sizeof buf, "Yesterday %02d:%02d", d.hour, d.minute);
    else
        std::snprintf(buf, sizeof buf, "%d %s %02d:%02d", d.day, months[(d.month - 1) % 12], d.hour, d.minute);
    return buf;
}

void refresh_slots(SDL_Renderer *ren, menu::View &v)
{
    for (int n = 0; n < menu::kSlots; n++) {
        menu::Slot &s = v.slots[n];
        if (s.thumb)
            SDL_DestroyTexture(s.thumb);
        s = menu::Slot{};
        SDL_PathInfo info;
        std::string path = slot_path(n);
        if (SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE) {
            s.exists = true;
            s.when = when_text(info.modify_time);
            s.thumb = load_thumb(ren, path);
        }
    }
}

// ---- the game's own cheats, from the menu ----------------------------------------------

constexpr uint32_t kInvincible = 0x1E76;   // word, $FFFF while on (the cheat's Return key)
constexpr uint32_t kInvincibleTimer = 0x2274, kInvincibleSparkle = 0x1E78;
constexpr uint32_t kLives = 0x2284;        // word
constexpr uint32_t kEnergy = 0x2286;       // word, up to 5
constexpr uint32_t kScore = 0x2276;        // long
constexpr uint32_t kWings = 0x22AE;        // byte, the cheat's F key
constexpr uint32_t kLastKey = 0x1E8C;      // raw code of the last key; the cheat keys read it
constexpr uint32_t kCheatsOn = 0x2552;     // word, set by typing the cheat word

void set_invincible(bool on)
{
    write16(kInvincible, on ? 0xFFFF : 0);
    write16(kInvincibleTimer, on ? 0x7FFF : 0);
    write8(kInvincibleSparkle, on ? 0xFF : 0);
}

// The level cheats (skip to the exit, wings) run from the game's loop: hand
// it the key with cheats switched on, and put the switch back once it has
// been read.
struct CheatKey {
    int frames = 0;
    uint16_t saved = 0;
};
CheatKey cheat_key;

void press_cheat_key(uint8_t code)
{
    if (!cheat_key.frames)
        cheat_key.saved = read16(kCheatsOn);
    write16(kCheatsOn, 1);
    write8(kLastKey, code);
    cheat_key.frames = 10;
}

void cheat_key_frame()
{
    if (cheat_key.frames && (read8(kLastKey) == 0 || --cheat_key.frames == 0)) {
        write16(kCheatsOn, cheat_key.saved);
        cheat_key.frames = 0;
    }
}

// ---- jumping about, unseen --------------------------------------------------------------

// Runs frames with no picture or sound kept, for the moves the player
// shouldn't watch (the story before the title, the selector).
void run_silently(int frames, void (*each)(int frame) = nullptr)
{
    for (int f = 0; f < frames; f++) {
        joy = Joystick{};
        if (each)
            each(f);
        run_frame();
        wide::frame_done();
        cheat_key_frame();
    }
    joy = Joystick{};
    float discard[4096];
    while (audio_take(discard, 2048) > 0) {
    }
}

// Esc does it on the Amiga; then a press of fire skips the story to the title.
void go_to_title()
{
    key_event(0x45, true);
    run_silently(216, [](int f) {
        if (f == 3)
            key_event(0x45, false);
        joy.fire = f >= 195 && f < 199;
    });
    cheat_key = CheatKey{};
}

constexpr uint32_t kSection = 0x251A;      // word: the level (the game calls them sections, $00-$50)
constexpr uint32_t kSelectorStart = 0x594; // word: the section its map selector starts

// A new game at a section, through the game's own map selector: its M cheat
// key, then Return to take the section it offers. That works from a level,
// so from anywhere else start a game from the title first. Either way it is
// a new game (score 0, three lives).
void go_to_section(int n)
{
    if (!wide::playing()) {
        go_to_title();
        run_silently(40);
        run_silently(8, [](int f) { joy.fire = f < 4; });
        for (int waited = 0; waited < 600 && !wide::playing(); waited += 10)
            run_silently(10);
    }
    uint16_t cheats = read16(kCheatsOn);
    write16(kCheatsOn, 1);
    write16(kSelectorStart, uint16_t(n));
    write8(kLastKey, 0x37);   // M
    // The selector replaces the level; Return takes the section it offers.
    for (int f = 0; f < 50 && wide::playing(); f++)
        run_silently(1);
    for (int tries = 0; tries < 40 && !wide::playing(); tries++) {
        key_event(0x44, true);
        run_silently(2);
        key_event(0x44, false);
        run_silently(8);
    }
    write16(kCheatsOn, cheats);
    set_invincible(false);   // with cheats on, Return is also the invincibility key
}

// ---- sound --------------------------------------------------------------------------------

void apply_audio(const settings::Settings &s)
{
    static const float width[] = {1.0f, 0.7f, 0.4f, 0.0f};
    AudioOutput o;
    o.model = AudioOutput::Model(s.sound_model);
    o.separation = width[s.stereo];
    o.volume = 1.5f * std::pow(float(s.volume) / 10.0f, 1.7f);
    o.ring_out = s.ring_out;
    set_audio_output(o);
}

// ---- the picture ----------------------------------------------------------------------------

// Draws the w x h game picture into the window as the Picture setting says,
// with scanlines over it if they are on.
struct Presenter {
    SDL_Renderer *ren;
    SDL_Texture *big = nullptr;   // crisp and soft: the picture blown up by a whole number first
    int big_w = 0, big_h = 0;
    SDL_Texture *lines = nullptr;
    int lines_rows = 0, lines_height = 0, lines_strength = 0;
    SDL_Texture *tall = nullptr, *crt = nullptr, *grille = nullptr;   // the CRT picture's stages
    int tall_w = 0, tall_h = 0, crt_w = 0, crt_h = 0;

    // A render target of this size, made again only when the size changes.
    void target(SDL_Texture *&t, int &tw, int &th, int w, int h)
    {
        if (t && tw == w && th == h)
            return;
        if (t)
            SDL_DestroyTexture(t);
        t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        tw = w;
        th = h;
    }

    void draw(SDL_Texture *src, int w, int h, int picture, int scanlines)
    {
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        float sc = std::min(float(ow) / float(w), float(oh) / float(h));
        if (picture == 1)
            sc = std::max(1.0f, std::floor(sc));
        SDL_FRect dst{std::round((float(ow) - float(w) * sc) / 2), std::round((float(oh) - float(h) * sc) / 2),
                      std::round(float(w) * sc), std::round(float(h) * sc)};
        if (picture == 3) {
            draw_crt(src, w, h, dst, scanlines);
            return;
        }
        // Crisp: each pixel becomes k x k (the next whole number up), then a
        // smooth step down to the window: even, sharp pixels at any size.
        // Soft: only half that first, so the smoothing blends a little.
        int k = picture == 0 ? int(std::ceil(sc - 0.001f)) : picture == 2 ? int(std::ceil(sc / 2 - 0.001f)) : 1;
        SDL_SetTextureScaleMode(src, picture == 2 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        if (k > 1 && float(k) != sc) {
            SDL_SetTextureScaleMode(src, SDL_SCALEMODE_NEAREST);
            target(big, big_w, big_h, w * k, h * k);
            SDL_SetRenderTarget(ren, big);
            SDL_RenderTexture(ren, src, nullptr, nullptr);
            SDL_SetRenderTarget(ren, nullptr);
            SDL_RenderTexture(ren, big, nullptr, &dst);
        } else {
            SDL_RenderTexture(ren, src, nullptr, &dst);
        }
        draw_lines(h, dst, scanlines);
    }

    // Worked out for each screen row of the picture: a smooth wave, one per
    // game row, darkest between rows. A pure wave has no finer ripples to
    // fold into a slow beat when a game row covers a fractional number of
    // screen rows (3.85 on a Steam Deck).
    void draw_lines(int h, const SDL_FRect &dst, int scanlines)
    {
        if (!scanlines)
            return;
        int lh = int(dst.h);
        if (!lines || lines_rows != h || lines_height != lh || lines_strength != scanlines) {
            if (lines)
                SDL_DestroyTexture(lines);
            float peak = scanlines == 1 ? 0.26f : 0.50f;
            std::vector<uint32_t> px(size_t(std::max(1, lh)));
            for (int y = 0; y < lh; y++) {
                float a = 0;
                for (int k = 0; k < 8; k++) {   // averaged over the pixel
                    float p = (float(y) + (float(k) + 0.5f) / 8) * float(h) / float(lh);
                    a += 0.5f + 0.5f * std::cos(6.2831853f * (p - std::floor(p)));
                }
                px[size_t(y)] = uint32_t(std::lround(peak * a / 8 * 255)) << 24;
            }
            lines = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, std::max(1, lh));
            SDL_UpdateTexture(lines, nullptr, px.data(), 4);
            SDL_SetTextureBlendMode(lines, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(lines, SDL_SCALEMODE_NEAREST);
            lines_rows = h;
            lines_height = lh;
            lines_strength = scanlines;
        }
        SDL_RenderTexture(ren, lines, nullptr, &dst);
    }

    // A television of the time: each line soft along its length (the beam)
    // but sharp between lines, a glow round bright parts, scanlines, the
    // red-green-blue stripes of an aperture grille, and the picture on gently
    // curved glass, darker towards its edges.
    void draw_crt(SDL_Texture *src, int w, int h, const SDL_FRect &dst, int scanlines)
    {
        int cw = std::max(1, int(dst.w)), ch = std::max(1, int(dst.h));
        // Rows stretched by a whole number with no smoothing, then smoothed to
        // the screen: the smoothing is then (almost) only along the lines.
        int k = std::max(1, int(std::ceil(dst.h / float(h) - 0.001f)));
        SDL_SetTextureScaleMode(src, SDL_SCALEMODE_NEAREST);
        target(tall, tall_w, tall_h, w, h * k);
        SDL_SetRenderTarget(ren, tall);
        SDL_RenderTexture(ren, src, nullptr, nullptr);
        target(crt, crt_w, crt_h, cw, ch);
        SDL_SetRenderTarget(ren, crt);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tall, nullptr, nullptr);
        // The glow: the picture lightly blurred and added round bright parts
        // (faintly: adding more would push full colours out of their hue).
        SDL_Texture *blur = shrink(src, w, h, 1);
        SDL_SetTextureBlendMode(blur, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaModFloat(blur, 0.13f);
        SDL_RenderTexture(ren, blur, nullptr, nullptr);
        SDL_SetTextureBlendMode(blur, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaModFloat(blur, 1);
        SDL_FRect all{0, 0, float(cw), float(ch)};
        draw_lines(h, all, std::max(scanlines, 1));
        // The grille: three screen columns, each holding back the other two colours a little.
        if (!grille) {
            const uint32_t stripes[3] = {0xFFFFE2E2u, 0xFFE2FFE2u, 0xFFE2E2FFu};
            grille = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 3, 1);
            SDL_UpdateTexture(grille, nullptr, stripes, 12);
            SDL_SetTextureBlendMode(grille, SDL_BLENDMODE_MOD);
            SDL_SetTextureScaleMode(grille, SDL_SCALEMODE_NEAREST);
        }
        SDL_RenderTextureTiled(ren, grille, nullptr, 1.0f, &all);
        SDL_SetRenderTarget(ren, nullptr);
        // The glass: the picture on a gently bulging mesh, its edges bowed
        // and its corners darker.
        constexpr int nx = 32, ny = 20;
        constexpr float bend = 0.035f;
        std::vector<SDL_Vertex> v;
        std::vector<int> idx;
        float cx = dst.x + dst.w / 2, cy = dst.y + dst.h / 2;
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                float u = float(i) / nx, t = float(j) / ny, x = u * 2 - 1, y = t * 2 - 1;
                float px = x * (1 - bend * y * y), py = y * (1 - bend * x * x);
                float shade = (1 - 0.14f * x * x * x * x) * (1 - 0.18f * y * y * y * y);
                v.push_back({{cx + px * dst.w / 2, cy + py * dst.h / 2}, {shade, shade, shade, 1}, {u, t}});
            }
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                int a = j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
                for (int q : {a, b, d, a, d, c})
                    idx.push_back(q);
            }
        SDL_RenderGeometry(ren, crt, v.data(), int(v.size()), idx.data(), int(idx.size()));
    }

    // The picture shrunk (once a light blur, for the CRT's glow; twice a
    // heavy one, for the frosted glass behind the menu).
    SDL_Texture *small[2] = {};
    int small_w[2] = {}, small_h[2] = {};
    SDL_Texture *shrink(SDL_Texture *src, int w, int h, int steps = 2)
    {
        const int div[2] = {4, 12};
        SDL_Texture *from = src;
        SDL_Texture *was = SDL_GetRenderTarget(ren);
        SDL_SetTextureScaleMode(src, SDL_SCALEMODE_LINEAR);
        for (int i = 0; i < steps; i++) {
            target(small[i], small_w[i], small_h[i], std::max(2, w / div[i]), std::max(2, h / div[i]));
            SDL_SetRenderTarget(ren, small[i]);
            SDL_RenderTexture(ren, from, nullptr, nullptr);
            from = small[i];
        }
        SDL_SetRenderTarget(ren, was);
        return from;
    }

    // The picture blurred and stretched over the whole window: a cheap
    // frosted glass for behind the menu.
    void draw_frosted(SDL_Texture *src, int w, int h, float alpha)
    {
        SDL_Texture *from = shrink(src, w, h);
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        float sc = std::max(float(ow) / float(w), float(oh) / float(h)) * 1.06f;   // cover, edges off screen
        SDL_FRect dst{(float(ow) - float(w) * sc) / 2, (float(oh) - float(h) * sc) / 2, float(w) * sc,
                      float(h) * sc};
        SDL_SetTextureAlphaModFloat(from, alpha);
        SDL_RenderTexture(ren, from, nullptr, &dst);
        SDL_SetTextureAlphaModFloat(from, 1);
    }
};

// Which buttons to draw: the pad's own, or on a Steam Deck the Deck's.
glyphs::Style pad_style(SDL_Gamepad *pad)
{
    static const bool deck = [] {
        if (const char *e = std::getenv("SteamDeck"); e && *e == '1')
            return true;
        char name[64] = {};
        if (FILE *f = std::fopen("/sys/devices/virtual/dmi/id/product_name", "r")) {
            if (!std::fgets(name, sizeof name, f))
                name[0] = 0;
            std::fclose(f);
        }
        return std::strncmp(name, "Jupiter", 7) == 0 || std::strncmp(name, "Galileo", 7) == 0;
    }();
    if (pad) {
        switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return glyphs::Style::playstation;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return glyphs::Style::nintendo;
        default: break;
        }
    }
#ifdef __PROSPERO__
    (void)deck;
    return glyphs::Style::playstation;
#else
    return deck ? glyphs::Style::deck : glyphs::Style::xbox;
#endif
}

// The last frame through the Picture setting, off screen:
// "picture,scanlines,width,height[,widescreen]:file.png".
void picture_shot(const std::string &spec)
{
    size_t colon = spec.find(':');
    if (colon == std::string::npos)
        return;
    int picture = 0, scan = 0, w = 1280, h = 800, widescreen = 1;
    std::sscanf(spec.c_str(), "%d,%d,%d,%d,%d", &picture, &scan, &w, &h, &widescreen);
    SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *ren = SDL_CreateSoftwareRenderer(surf);
    int ww = widescreen ? wide_width_for(w, h) : kGameW;
    wide::set_view_extra((ww - kGameW) / 2);
    std::vector<uint32_t> px(size_t(ww) * wide::kHeight);
    wide::render(px.data(), ww, 1.0f, true);
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ww, wide::kHeight);
    SDL_UpdateTexture(t, nullptr, px.data(), ww * 4);
    Presenter pr{ren};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    pr.draw(t, ww, wide::kHeight, picture, scan);
    SDL_RenderPresent(ren);
    write_png(spec.c_str() + colon + 1, static_cast<uint32_t *>(surf->pixels), w, h, surf->pitch / 4);
    SDL_DestroyRenderer(ren);
    SDL_DestroySurface(surf);
}

// ---- the window -------------------------------------------------------------------------------

// For tests with no one at the keyboard (SDL_VIDEODRIVER=offscreen):
// JP2_SCRIPT="1.0:key=Escape,1.5:key=Down,2.0:shot=/tmp/a.png,3:quit", times
// in seconds from the start. Keys arrive as SDL events inside the program.
struct ScriptStep {
    double t;
    std::string what;
};

std::vector<ScriptStep> parse_script(const char *spec)
{
    std::vector<ScriptStep> out;
    std::string s = spec ? spec : "";
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? s.size() : comma + 1;
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            out.push_back({std::atof(item.c_str()), item.substr(colon + 1)});
    }
    return out;
}

// The player's disk, as saved by the first-run page.
std::string stored_disk_path() { return pref_dir().empty() ? std::string() : pref_dir() + "disk.adf"; }

// Boots from a disk the player chose and keeps it (as a plain .adf) in the
// game's own folder for next time.
bool boot_and_keep(const std::string &path, std::string *error)
{
    if (!game::boot(path.c_str(), error))
        return false;
    std::string store = stored_disk_path();
    if (!store.empty() && path != store && !disk::save_image(store))
        std::fprintf(stderr, "couldn't keep the disk at %s\n", store.c_str());
    return true;
}

int run_window(bool force_fullscreen, const std::string &load_from, const std::vector<std::string> &disks,
               void (*after_boot)())
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    settings::Settings set = settings::load(pref_dir() + "settings.ini");
    if (force_fullscreen)
        set.fullscreen = true;
    settings::Settings applied = set, written = set;

    int win_w = kViewW * 3, win_h = kViewH * 3;
    if (const char *ws = std::getenv("JP2_WINDOW"))   // tests and screenshots: "1920x1080"
        std::sscanf(ws, "%dx%d", &win_w, &win_h);
    SDL_Window *win = SDL_CreateWindow("James Pond 2 - Codename RoboCod", win_w, win_h,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    if (set.fullscreen)
        SDL_SetWindowFullscreen(win, true);
    SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    ui::init(ren);

    // The disk: the first that boots, else ask for one.
    bool booted = false;
    std::string why;
    for (const std::string &d : disks) {
        std::string err;
        SDL_PathInfo info;
        if (d.empty() || !SDL_GetPathInfo(d.c_str(), &info))
            continue;
        if (game::boot(d.c_str(), &err)) {
            std::fprintf(stderr, "disk: booted from %s, %.2f s after start\n", d.c_str(), SDL_GetTicks() / 1000.0);
            booted = true;
            break;
        }
        if (why.empty())
            why = d + ": " + err;
    }
    if (!booted) {
        std::string dir = pref_dir();
        if (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\'))
            dir.pop_back();
        if (setup::run(win, ren, boot_and_keep, why, pad_style(nullptr), dir).empty()) {
            SDL_Quit();
            return 0;
        }
    }
    if (after_boot)
        after_boot();
    Presenter presenter{ren};
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kViewW, kViewH);
    int wide_w = 0;
    SDL_Texture *wide_tex = nullptr;
    // With widescreen off, gameplay still comes from the redraw (so smooth
    // motion works), at the game's own width and the same height.
    SDL_Texture *narrow_tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kGameW, wide::kHeight);
    int pic_w = kGameW;   // the width of the redraw in wide_px
    std::vector<uint32_t> wide_px;
    bool showing_wide = false;

    apply_audio(set);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    // About 50 ms queued: enough that a late frame never runs the sound dry,
    // held there by playing a hair faster or slower.
    const int target_frames = kAudioRate / 20;
    float audio_ratio = 1.0f;
    auto prefill = [&]() {
        if (!audio)
            return;
        int queued = SDL_GetAudioStreamQueued(audio) / 8;
        if (queued < target_frames) {
            std::vector<float> silence(size_t(target_frames - queued) * 2, 0.0f);
            SDL_PutAudioStreamData(audio, silence.data(), int(silence.size() * sizeof(float)));
        }
    };
    if (audio) {
        prefill();
        SDL_ResumeAudioStreamDevice(audio);
    }

    SDL_Gamepad *pad = nullptr;
    Joystick keys{}, padj{};
    bool running = true;
    bool want_save = false, want_load = false;
    bool digits_to_game = false;
    bool pad_wait_release = false;   // after the menu: ignore buttons still held from it
    bool infinite_lives = false;
    uint16_t lives_floor = 3;
    Feel feel;
    Rumble rumble;
    std::vector<uint64_t> shown_at;   // when each recent picture reached the screen, for the counter
    const char *message = nullptr;
    uint64_t message_until = 0;
    std::vector<uint32_t> present(size_t(kViewW) * kViewH);
    long frames_run = 0;
    menu::View view;
    view.settings = &set;
    view.style = pad_style(nullptr);
#ifdef __PROSPERO__
    view.can_fullscreen = false;   // the console always fills the screen
#endif
    struct Held {
        bool on;
        uint64_t next;
    } held[4]{};   // menu: up, down, left, right, for repeats
    if (!load_from.empty()) {   // start from a save (--load)
        message = quick_load(load_from);
        message_until = SDL_GetTicksNS() + 2000000000ull;
    }
    uint64_t next = SDL_GetTicksNS(), last_draw = next;
    const uint64_t frame_ns = 1000000000ull / 50;
    std::vector<ScriptStep> script = parse_script(std::getenv("JP2_SCRIPT"));
    size_t script_at = 0;
    const uint64_t started = next;
    std::string shot_path;

    auto show = [&](const char *text) {
        message = text;
        message_until = SDL_GetTicksNS() + 2000000000ull;
    };
    // The picture last shown, for save thumbnails.
    auto save_slot = [&](int n) {
        const char *result = quick_save(slot_path(n));
        if (showing_wide)
            write_thumb(slot_path(n), wide_px.data(), pic_w, wide::kHeight);
        else
            write_thumb(slot_path(n), present.data(), kViewW, kViewH);
        return result;
    };
    bool previewing_sound = false;
    auto stop_sound_preview = [&]() {
        if (audio && previewing_sound) {
            SDL_ClearAudioStream(audio);
            SDL_PauseAudioStreamDevice(audio);
        }
        previewing_sound = false;
        view.level_l = view.level_r = 0;
    };
    auto open_menu = [&]() {
        menu::open();
        audio_preview_begin();
        refresh_slots(ren, view);
        joy = Joystick{};
        keys = Joystick{};
        for (int i = 0; i < 4; i++)
            held[i] = {true, ~0ull};   // a direction held when it opens isn't a press
        if (audio)
            SDL_PauseAudioStreamDevice(audio);
    };
    auto close_menu = [&]() {
        menu::close();
        stop_sound_preview();
        pad_wait_release = true;
        next = SDL_GetTicksNS();
        if (audio) {
            prefill();
            SDL_ResumeAudioStreamDevice(audio);
        }
    };
    // Esc does it on the Amiga; then a press of fire skips the story to the title.
    // After a jump made out of sight: start the sound and the clock afresh.
    auto after_jump = [&]() {
        if (audio)
            SDL_ClearAudioStream(audio);
        prefill();
        infinite_lives = false;
        feel = Feel{};
        next = SDL_GetTicksNS();
    };
    auto handle = [&](menu::Request r) {
        switch (r.cmd) {
        case menu::Cmd::resume: close_menu(); break;
        case menu::Cmd::exit_game: running = false; break;
        case menu::Cmd::quit_to_title:
            close_menu();
            go_to_title();
            after_jump();
            break;
        case menu::Cmd::play_section:
            close_menu();
            go_to_section(r.arg);
            after_jump();
            break;
        case menu::Cmd::load:
            close_menu();
            feel = Feel{};
            show(quick_load(slot_path(r.arg)));
            prefill();
            break;
        case menu::Cmd::save:
            menu::toast(std::strcmp(save_slot(r.arg), "QUICK SAVED") == 0 ? "Saved" : "Couldn't save");
            refresh_slots(ren, view);
            break;
        case menu::Cmd::skip_exit:
            close_menu();
            press_cheat_key(0x32);   // X
            break;
        case menu::Cmd::wings: write8(kWings, r.arg ? 0xFF : 0); break;   // what the cheat's F key flips
        case menu::Cmd::continue_game:
            close_menu();
            feel = Feel{};
            show(quick_load(resume_path()));
            prefill();
            break;
        case menu::Cmd::start_fresh:
            close_menu();
            std::remove(resume_path().c_str());
            std::remove((resume_path() + ".thumb").c_str());
            break;
        case menu::Cmd::invincible: set_invincible(r.arg != 0); break;
        case menu::Cmd::infinite_lives:
            infinite_lives = r.arg != 0;
            lives_floor = std::max<uint16_t>(3, read16(kLives));
            break;
        default: break;
        }
    };

    // Quit mid-level last time: offer to carry on from there.
    SDL_Texture *continue_thumb = nullptr;
    if (SDL_PathInfo info; load_from.empty() && SDL_GetPathInfo(resume_path().c_str(), &info)) {
        continue_thumb = load_thumb(ren, resume_path());
        menu::open_continue(continue_thumb, when_text(info.modify_time));
        if (audio)
            SDL_PauseAudioStreamDevice(audio);
    }

    while (running) {
        uint64_t now = SDL_GetTicksNS();
        while (script_at < script.size() && double(now - started) / 1e9 >= script[script_at].t) {
            const std::string &w = script[script_at++].what;
            if (w.rfind("key=", 0) == 0) {
                SDL_Event k{};
                k.type = SDL_EVENT_KEY_DOWN;
                k.key.scancode = SDL_GetScancodeFromName(w.c_str() + 4);
                k.key.down = true;
                SDL_PushEvent(&k);
                k.type = SDL_EVENT_KEY_UP;
                k.key.down = false;
                SDL_PushEvent(&k);
            } else if (w.rfind("shot=", 0) == 0) {
                shot_path = w.substr(5);
            } else if (w == "quit") {
                running = false;
            }
        }
        bool in_menu = menu::is_open();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) {
                    pad = SDL_OpenGamepad(e.gdevice.which);
                    view.style = pad_style(pad);
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                    SDL_CloseGamepad(pad);
                    pad = nullptr;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                view.pad = true;
                int b = e.gbutton.button;
                if (menu::is_open()) {
                    menu::Action a;
                    switch (b) {
                    case SDL_GAMEPAD_BUTTON_SOUTH: a = menu::Action::confirm; break;
                    case SDL_GAMEPAD_BUTTON_EAST: a = menu::Action::back; break;
                    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: a = menu::Action::prev_page; break;
                    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: a = menu::Action::next_page; break;
                    case SDL_GAMEPAD_BUTTON_START:
                    case SDL_GAMEPAD_BUTTON_BACK: a = menu::Action::close; break;
                    default: continue;
                    }
                    handle(menu::input(a, view));
                    break;
                }
                // Menu: the menu button. L: quick save. R: quick load. Y: smooth motion.
                if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_BACK) {
                    open_menu();
                } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                    want_save = true;
                } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                    want_load = true;
                } else if (b == SDL_GAMEPAD_BUTTON_NORTH) {
                    set.smooth = !set.smooth;
                    show(set.smooth ? "SMOOTH MOTION ON" : "SMOOTH MOTION OFF");
                }
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = e.type == SDL_EVENT_KEY_DOWN;
                SDL_Scancode sc = e.key.scancode;
                if (down)
                    view.pad = false;
                if (menu::is_open()) {
                    if (!down)
                        break;
                    menu::Action a;
                    bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                    switch (sc) {
                    case SDL_SCANCODE_UP: a = menu::Action::up; break;
                    case SDL_SCANCODE_DOWN: a = menu::Action::down; break;
                    case SDL_SCANCODE_LEFT: a = menu::Action::left; break;
                    case SDL_SCANCODE_RIGHT: a = menu::Action::right; break;
                    case SDL_SCANCODE_RETURN:
                    case SDL_SCANCODE_KP_ENTER:
                    case SDL_SCANCODE_SPACE: a = menu::Action::confirm; break;
                    case SDL_SCANCODE_ESCAPE:
                    case SDL_SCANCODE_BACKSPACE: a = menu::Action::back; break;
                    case SDL_SCANCODE_TAB: a = shift ? menu::Action::prev_page : menu::Action::next_page; break;
                    default: continue;
                    }
                    if (e.key.repeat && a != menu::Action::up && a != menu::Action::down &&
                        a != menu::Action::left && a != menu::Action::right)
                        break;
                    handle(menu::input(a, view));
                    break;
                }
                if (sc == SDL_SCANCODE_ESCAPE) {   // the menu (the game's own Esc is Quit to title)
                    if (down && !e.key.repeat)
                        open_menu();
                    break;
                }
                // The cheat map selector (M) wants a section number typed on the
                // number keys: hand them to the game until Return.
                if (down && !e.key.repeat) {
                    if (sc == SDL_SCANCODE_M)
                        digits_to_game = true;
                    else if (sc == SDL_SCANCODE_RETURN)
                        digits_to_game = false;
                }
                // Quick save 1 (or F5), quick load 2 (or F8): ours, not the game's.
                // Its cheat mode uses letters, Enter, F6, F7, F9 and F10.
                if (sc == SDL_SCANCODE_4 && !digits_to_game) {   // smooth motion on / off
                    if (down && !e.key.repeat) {
                        set.smooth = !set.smooth;
                        show(set.smooth ? "SMOOTH MOTION ON" : "SMOOTH MOTION OFF");
                    }
                    break;
                }
                if (sc == SDL_SCANCODE_3 && !digits_to_game) {   // widescreen on / off
                    if (down && !e.key.repeat) {
                        set.widescreen = !set.widescreen;
                        show(set.widescreen ? "WIDESCREEN ON" : "WIDESCREEN OFF");
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
        in_menu = menu::is_open();

        const int dead = 12000;
        Sint16 lx = pad ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) : 0;
        Sint16 ly = pad ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) : 0;
        auto button = [&](SDL_GamepadButton b) { return pad && SDL_GetGamepadButton(pad, b); };
        if (in_menu) {
            // D-pad and stick move the focus, repeating while held.
            bool dirs[4] = {button(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -20000,
                            button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 20000,
                            button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -20000,
                            button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 20000};
            static const menu::Action acts[4] = {menu::Action::up, menu::Action::down, menu::Action::left,
                                                 menu::Action::right};
            for (int i = 0; i < 4 && menu::is_open(); i++) {
                if (!dirs[i]) {
                    held[i] = {false, 0};
                    continue;
                }
                if (!held[i].on || now >= held[i].next) {
                    view.pad = true;
                    handle(menu::input(acts[i], view));
                    held[i].next = now + (held[i].on ? 80000000ull : 350000000ull);
                    held[i].on = true;
                }
            }
            joy = Joystick{};
        } else {
            padj = Joystick{};
            if (pad) {
                // D-pad and left stick move. In a level A jumps (the joystick's up)
                // and B or X stretches (the fire button); on titles and menus A
                // is fire too, so it starts the game.
                bool a = button(SDL_GAMEPAD_BUTTON_SOUTH);
                bool bx = button(SDL_GAMEPAD_BUTTON_EAST) || button(SDL_GAMEPAD_BUTTON_WEST);
                bool in_level = wide::playing();
                padj.left = button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead;
                padj.right = button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead;
                // Up on the D-pad or stick jumps only if asked (Controls > Up also jumps);
                // off a level it always works, for the game's own menus.
                bool up = button(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead;
                padj.up = (up && (!in_level || set.up_jumps)) || (a && in_level);
                padj.down = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead;
                padj.fire = bx || (a && !in_level);
                // The button that closed the menu is still down: let go first.
                if (pad_wait_release) {
                    if (a || bx || padj.up || padj.down || padj.left || padj.right)
                        padj = Joystick{};
                    else
                        pad_wait_release = false;
                }
            }
            joy.up = keys.up || padj.up;
            joy.down = keys.down || padj.down;
            joy.left = keys.left || padj.left;
            joy.right = keys.right || padj.right;
            joy.fire = keys.fire || padj.fire;
            joy.fire2 = keys.fire2 || padj.fire2;
        }
        if (!running)
            break;

        // Settings the menu (or a shortcut) changed.
        if (set != applied) {
            if (set.fullscreen != applied.fullscreen)
                SDL_SetWindowFullscreen(win, set.fullscreen);
            if (set.widescreen != applied.widescreen)
                wide::set_view_extra(set.widescreen ? (wide_w - 288) / 2 : 0);
            if (set.sound_model != applied.sound_model || set.stereo != applied.stereo ||
                set.volume != applied.volume || set.ring_out != applied.ring_out)
                apply_audio(set);
            if ((set.rumble_strength != applied.rumble_strength || set.rumble != applied.rumble) && set.rumble &&
                pad)   // feel the new strength
                SDL_RumbleGamepad(pad, Rumble::at_strength(0xD000, set.rumble_strength),
                                  Rumble::at_strength(0x9000, set.rumble_strength), 200);
            applied = set;
        }
        if (!in_menu && set != written) {
            settings::save(pref_dir() + "settings.ini", set);
            written = set;
        }

        // Between frames, so the machine is in a steady state.
        if (!in_menu && (want_save || want_load)) {
            if (want_load)
                feel = Feel{};
            show(want_save ? save_slot(0) : quick_load(quicksave_path()));
            if (want_load)
                prefill();
            want_save = want_load = false;
        }
        int ran = 0;
        if (!in_menu) {
            while (now >= next && ran < 3) {
                run_frame();
                wide::frame_done();
                cheat_key_frame();
                // The pad feels Robocod's knocks, and shakes with the screen.
                if (int what = feel.check(wide::playing()))
                    rumble.felt(what, now);
                if (infinite_lives && read16(kLives) < lives_floor)
                    write16(kLives, lives_floor);
                record::frame();
                if (++frames_run % 3000 == 0)
                    record::flush();   // keep the play recording even if the game is killed
                next += frame_ns;
                ran++;
            }
            if (now > next + frame_ns * 5)
                next = now;
        }
        if (pad) {
            uint16_t lo, hi;
            uint32_t hold;
            if (rumble.step(set.rumble && !in_menu, feel.shake(), set.rumble_strength, now, lo, hi, hold))
                SDL_RumbleGamepad(pad, lo, hi, hold);
        }
        if (ran) {
            float buf[4096];
            int n;
            while ((n = audio_take(buf, 2048)) > 0)
                if (audio)
                    SDL_PutAudioStreamData(audio, buf, n * 2 * int(sizeof(float)));
            if (audio) {
                int queued = SDL_GetAudioStreamQueued(audio) / 8;
                if (queued > target_frames * 4) {   // far behind (a long stall): start afresh
                    SDL_ClearAudioStream(audio);
                    prefill();
                } else {
                    float err = float(queued - target_frames) / float(target_frames);
                    float want = 1.0f + std::clamp(err * 0.004f, -0.003f, 0.003f);
                    audio_ratio += (want - audio_ratio) * 0.05f;
                    SDL_SetAudioStreamFrequencyRatio(audio, audio_ratio);
                }
            }
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
            wide_px.assign(size_t(wide_w) * wide::kHeight, 0);
            wide::set_view_extra(set.widescreen ? (wide_w - 288) / 2 : 0);
        }
        float dt = float(now - last_draw) / 1e9f;
        last_draw = now;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (in_menu) {
            // The game behind the menu, drawn as the settings now make it so a
            // picture setting shows at once; frosted over unless one has the focus.
            pic_w = set.widescreen ? wide_w : kGameW;
            showing_wide = wide::render(wide_px.data(), pic_w, 1.0f, true);
            SDL_Texture *redrawn = set.widescreen ? wide_tex : narrow_tex;
            SDL_Texture *game = showing_wide ? redrawn : tex;
            int gw = showing_wide ? pic_w : kViewW, gh = showing_wide ? wide::kHeight : kViewH;
            if (showing_wide) {
                SDL_UpdateTexture(redrawn, nullptr, wide_px.data(), pic_w * 4);
            } else {
                for (int y = 0; y < kViewH; y++)
                    std::memcpy(&present[size_t(y) * kViewW], &framebuffer[(kViewY + y) * kFbW + kViewX],
                                kViewW * 4);
                SDL_UpdateTexture(tex, nullptr, present.data(), kViewW * 4);
            }
            float clear = menu::preview();
            if (menu::asking_continue()) {   // behind the question: where the game was left
                if (continue_thumb)
                    presenter.draw_frosted(continue_thumb, kThumbW, kThumbH, 1);
            } else {
                presenter.draw(game, gw, gh, set.picture, set.scanlines);
                if (clear < 0.999f)
                    presenter.draw_frosted(game, gw, gh, 1 - clear);
            }
            int ow = 0, oh = 0;
            SDL_GetRenderOutputSize(ren, &ow, &oh);
            ui::begin(ow, oh);
            ui::fill_window(ui::rgb(0x0A101C, 0.74f * (1 - clear)));

            // The Sound page plays the game's last few seconds through its settings.
            if (audio && menu::sound_preview()) {
                if (!previewing_sound) {
                    SDL_ClearAudioStream(audio);
                    SDL_ResumeAudioStreamDevice(audio);
                    previewing_sound = true;
                }
                int queued = SDL_GetAudioStreamQueued(audio) / 8;
                if (queued < target_frames) {
                    std::vector<float> chunk(size_t(target_frames - queued) * 2);
                    int n = audio_preview_take(chunk.data(), target_frames - queued);
                    SDL_PutAudioStreamData(audio, chunk.data(), n * 2 * int(sizeof(float)));
                    float peak[2] = {0, 0};
                    for (int i = 0; i < n * 2; i++)
                        peak[i & 1] = std::max(peak[i & 1], std::fabs(chunk[size_t(i)]));
                    // -40 dB to 0 dB across the meter, falling back slowly.
                    float *lv[2] = {&view.level_l, &view.level_r};
                    for (int c = 0; c < 2; c++) {
                        float to = std::clamp((20 * std::log10(peak[c] + 1e-6f) + 40) / 40, 0.0f, 1.0f);
                        *lv[c] = std::max(to, *lv[c] - dt * 1.5f);
                    }
                }
            } else if (previewing_sound) {
                stop_sound_preview();
            }

            view.in_level = wide::playing();
            view.invincible = read16(kInvincible) != 0;
            view.infinite_lives = infinite_lives;
            view.wings = read8(kWings) != 0;
            view.score = read32(kScore);
            view.lives = read16(kLives);
            view.energy = read16(kEnergy);
            view.section = wide::playing() ? read16(kSection) : -1;
            menu::draw(view, std::min(dt, 0.1f));
        } else {
            // Where we are between the last game tick and the next (0..1).
            float alpha = 1.0f - float(int64_t(next - now)) / float(frame_ns);
            alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
            if (ran || message || (set.smooth && showing_wide)) {
                bool show_message = message && now < message_until;
                if (!show_message)
                    message = nullptr;
                // Always the full redraw (it matches the game's own picture), moved
                // between ticks only with smoothing on.
                pic_w = set.widescreen ? wide_w : kGameW;
                showing_wide = wide::render(wide_px.data(), pic_w, set.smooth ? alpha : 1.0f, true);
                if (showing_wide) {
                    if (show_message)
                        draw_message(wide_px.data(), pic_w, wide::kHeight, message);
                    SDL_UpdateTexture(set.widescreen ? wide_tex : narrow_tex, nullptr, wide_px.data(), pic_w * 4);
                } else {
                    for (int y = 0; y < kViewH; y++)
                        std::memcpy(&present[size_t(y) * kViewW], &framebuffer[(kViewY + y) * kFbW + kViewX],
                                    kViewW * 4);
                    if (show_message)
                        draw_message(present.data(), kViewW, kViewH, message);
                    SDL_UpdateTexture(tex, nullptr, present.data(), kViewW * 4);
                }
            }
            // Gameplay fills the window's height (widescreen also its width); title
            // and menu screens show the whole Amiga screen.
            if (showing_wide)
                presenter.draw(set.widescreen ? wide_tex : narrow_tex, pic_w, wide::kHeight, set.picture, set.scanlines);
            else
                presenter.draw(tex, kViewW, kViewH, set.picture, set.scanlines);
            if (set.show_fps) {
                shown_at.push_back(now);
                while (!shown_at.empty() && now - shown_at.front() > 1000000000ull)
                    shown_at.erase(shown_at.begin());
                char fps[32];
                std::snprintf(fps, sizeof fps, "%zu FPS", shown_at.size());
                int ow = 0, oh = 0;
                SDL_GetRenderOutputSize(ren, &ow, &oh);
                ui::begin(ow, oh);
                float tw = ui::text_width(ui::semibold, 20, fps) + 32;
                ui::rect(24, 24, tw, 40, 20, ui::rgb(0x0A101C, 0.72f));
                ui::text(ui::semibold, 20, 24 + tw / 2, 44, fps, ui::rgb(0xEEF2F8), ui::center);
            }
        }
        if (shot_path.empty() && ui::console_shot_asked())
            shot_path = "/app0/userdata/shot.png";
        if (!shot_path.empty()) {
            ui::save_screen(ren, shot_path);
            shot_path.clear();
        }
        SDL_RenderPresent(ren);
    }
    if (set != written)
        settings::save(pref_dir() + "settings.ini", set);
    // Leaving mid-level: keep the place for next time. Anywhere else (or
    // without having answered the question at launch), leave things be.
    if (!menu::asking_continue()) {
        if (wide::playing()) {
            quick_save(resume_path());
            if (showing_wide)
                write_thumb(resume_path(), wide_px.data(), pic_w, wide::kHeight);
            else
                write_thumb(resume_path(), present.data(), kViewW, kViewH);
        } else {
            std::remove(resume_path().c_str());
            std::remove((resume_path() + ".thumb").c_str());
        }
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
    if (const char *w = std::getenv("JP2_WATCH"))
        data_watch = uint32_t(std::strtoul(w, nullptr, 16)) & ~1u;
    int headless = 0;
    std::vector<Shot> shots;
    std::vector<Press> script;
    const char *record_dir = nullptr;
    bool cheat = false;
    std::string load_from, save_to, wav_path;
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
        } else if (!std::strcmp(argv[i], "--setup-shot") && i + 1 < argc) {
            // "width,height,found,error:file.png": the first-run page off screen
            const char *spec = argv[++i];
            const char *colon = std::strchr(spec, ':');
            int w = 1280, h = 800, found = 1, err = 0;
            std::sscanf(spec, "%d,%d,%d,%d", &w, &h, &found, &err);
            SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
            SDL_Renderer *ren = SDL_CreateSoftwareRenderer(surf);
            ui::init(ren);
            std::vector<std::string> list;
            if (found)
                list = {"/Users/you/Downloads/James Pond 2 - Codename RoboCod (1991)(Millennium)[cr SR].adf"};
            setup::draw_for_test(w, h, list, err ? "x.dms: DMS images aren't supported: turn it into an ADF first" : "",
                                 glyphs::Style::deck, "~/.local/share/bouncingball/James Pond 2");
            SDL_RenderPresent(ren);
            if (colon)
                write_png(colon + 1, static_cast<uint32_t *>(surf->pixels), w, h, surf->pitch / 4);
            return 0;
        } else if (!std::strcmp(argv[i], "--menu-shot") && i + 1 < argc) {
            menu_shot(argv[++i]);
            return 0;
        } else if (!std::strcmp(argv[i], "--section") && i + 1 < argc) {
            const char *v = argv[++i];
            if (const char *colon = std::strchr(v, ':'))
                section_jumps.push_back({std::atoi(v), int(std::strtoul(colon + 1, nullptr, 16))});
        } else if (!std::strcmp(argv[i], "--poke") && i + 1 < argc) {
            const char *v = argv[++i];
            const char *colon = std::strchr(v, ':'), *eq = std::strchr(v, '=');
            if (colon && eq)
                pokes.push_back({std::atoi(v), uint32_t(std::strtoul(colon + 1, nullptr, 16)),
                                 uint32_t(std::strtoul(eq + 1, nullptr, 16)), std::strlen(eq + 1) > 4});
        } else if (!std::strcmp(argv[i], "--picture-shot") && i + 1 < argc) {
            picture_shots.push_back(argv[++i]);
        } else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--audio") && i + 1 < argc) {
            // --audio a500|a1200|clean[,separation][,noring]: for comparing the output stages
            AudioOutput o;
            std::string v = argv[++i];
            o.model = v.rfind("a1200", 0) == 0 ? AudioOutput::a1200 : v.rfind("clean", 0) == 0 ? AudioOutput::clean
                                                                                                : AudioOutput::a500;
            if (size_t c = v.find(','); c != std::string::npos)
                o.separation = float(std::atof(v.c_str() + c + 1));
            o.ring_out = v.find("noring") == std::string::npos;
            set_audio_output(o);
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
    // Where the disk can be: given (--adf or JP2_ADF), kept by the first-run
    // page, or beside the program (a development install).
    std::vector<std::string> disks;
    if (adf)
        disks.push_back(adf);
    disks.push_back(stored_disk_path());
    if (const char *base = SDL_GetBasePath())
        disks.push_back(std::string(base) + "jp2.adf");
#ifdef __PROSPERO__
    // The console: whatever disk image the player copied into the game's folder.
    for (const std::string &d :
         setup::scan([](const std::string &p, std::string *err) { return disk::open(p.c_str(), err); }))
        disks.push_back(d);
    fullscreen = true;
#endif
    if (headless) {
        std::string err = "no disk image (use --adf)";
        bool ok = false;
        for (const std::string &d : disks)
            if (!d.empty() && (ok = game::boot(d.c_str(), &err)))
                break;
        if (!ok) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
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
        wide::set_view_extra((shot_width() - 288) / 2);
    // After the machine has booted from the disk.
    static const char *s_record_dir;
    static bool s_cheat;
    s_record_dir = record_dir;
    s_cheat = cheat;
    auto after_boot = []() {
        paula_on_start(heard_sample);
        if (s_record_dir)
            record::start(s_record_dir);
        if (s_cheat)
            game::enable_cheat();
    };
    if (headless) {
        after_boot();
        return run_headless(headless, shots, script, load_from, save_frame, save_to, wav_path);
    }
    return run_window(fullscreen, load_from, disks, after_boot);
}
