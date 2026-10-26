#include "menu.h"

#include "theme.h"
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace menu {

namespace {

using glyphs::Button;
using ui::Color;
namespace icon = ui::icon_id;

// ---- the look ------------------------------------------------------------------

using namespace theme;

constexpr float kRailX = kMargin, kRailY = 236, kRailW = 372, kRailH = 66, kRailPitch = 76;
constexpr float kPanelX = 504, kPanelY = 236, kPanelH = 692;
constexpr float kRowX = kPanelX + 24, kRowsY = kPanelY + 104;
constexpr float kRowH = 72, kRowPitch = 80;
constexpr float kSlotH = 104, kSlotPitch = 116, kSlotListW = 470;
constexpr float kInfoH = 46, kInfoPitch = 50;
constexpr float kFooterY = kPanelY + kPanelH - 84;

float panel_w() { return ui::page_width() - kPanelX - kMargin; }
float row_w() { return panel_w() - 48; }
float row_right() { return kRowX + row_w(); }

// ---- what the pages hold -----------------------------------------------------------

enum class Kind { action, toggle, choice, slider, slot, info, stepper };

struct Row {
    const char *label;
    const char *desc;
    Kind kind;
    int icon = 0;
    int (*get)(const View &) = nullptr;               // toggle, choice, slider
    Request (*set)(const View &, int) = nullptr;
    std::vector<const char *> choices;                // choice
    int lo = 0, hi = 1;                               // slider
    bool lo_is_off = false;                           // slider: the lowest value reads "Off"
    Request req;                                      // action, slot
    const char *ask_title = nullptr, *ask_body = nullptr, *ask_button = nullptr;   // ask first
    int slot = 0;                                     // slot
    bool preview = false;                             // let the game show through while focused
    bool (*enabled)(const View &) = nullptr;
    int (*count)(const View &) = nullptr;               // stepper: how many values
    std::string (*text)(const View &) = nullptr;        // stepper: the value shown
};

enum PageId { kGame, kSave, kLoad, kLevels, kDisplay, kSound, kCheats, kControls };

// The game's sections (levels) by world, in the order they are played. The
// game has no names for them; a world is one of its seven sets of graphics.
struct Level {
    int section;
    const char *tag;   // "Boss", "Bonus room", or null
};
struct World {
    const char *name;
    std::vector<Level> levels;
};
const std::vector<World> &worlds()
{
    static const std::vector<World> w = {
        {"The castle", {{0x00, "The roof"}, {0x40, "The hall"}, {0x2E, "Arctic Toys"}, {0x50, "The ending"}}},
        {"Games room",
         {{0x01}, {0x02}, {0x03}, {0x04}, {0x05}, {0x06}, {0x07}, {0x08}, {0x09}, {0x0A}, {0x0B}, {0x48}, {0x4B},
          {0x11, "Boss"}}},
        {"Cakes and toys",
         {{0x0C}, {0x0E}, {0x0F}, {0x0D}, {0x10, "Boss"}, {0x25}, {0x26}, {0x27}, {0x28}, {0x29}, {0x2A}, {0x2C},
          {0x2D}, {0x41}, {0x46}, {0x49}, {0x4A}, {0x47, "Bonus room"}}},
        {"Sweets",
         {{0x13}, {0x14}, {0x15}, {0x17}, {0x18}, {0x19}, {0x1A, "Upside down"}, {0x16, "Bonus room"}, {0x42},
          {0x43}, {0x44}, {0x45, "Upside down"}}},
        {"Transport",
         {{0x1C}, {0x1B}, {0x1E}, {0x1D}, {0x21}, {0x22}, {0x23}, {0x20}, {0x1F, "Bonus room"},
          {0x24, "Bonus room"}, {0x2B, "Boss"}}},
        {"Music",
         {{0x39}, {0x3A}, {0x3B}, {0x3C}, {0x3D}, {0x3E}, {0x3F}, {0x4F}, {0x4E}, {0x4D}, {0x4C, "Bonus room"},
          {0x12, "Boss"}}},
        {"Bath time",
         {{0x30}, {0x31}, {0x32}, {0x33}, {0x34}, {0x35}, {0x37}, {0x38}, {0x36}, {0x2F, "Bonus room"}}},
    };
    return w;
}
int world_no, level_no;   // the Levels page's choice
bool levels_synced;       // set to where the game is, once per opening

struct Page {
    const char *name;
    int icon;
    const char *summary;
    std::vector<Row> rows;
};

settings::Settings &S(const View &v) { return *v.settings; }

Row action(const char *label, const char *desc, Cmd cmd, int ic = 0)
{
    Row r{label, desc, Kind::action};
    r.req.cmd = cmd;
    r.icon = ic;
    return r;
}

Row toggle(const char *label, const char *desc, int (*get)(const View &), Request (*set)(const View &, int))
{
    Row r{label, desc, Kind::toggle};
    r.get = get;
    r.set = set;
    return r;
}

Row choice(const char *label, const char *desc, std::vector<const char *> choices, int (*get)(const View &),
           Request (*set)(const View &, int))
{
    Row r{label, desc, Kind::choice};
    r.choices = std::move(choices);
    r.get = get;
    r.set = set;
    return r;
}

const char *kSlotNames[kSlots] = {"Quick slot", "Slot 1", "Slot 2", "Slot 3"};

const std::vector<Page> &pages()
{
    static std::vector<Page> p;
    if (!p.empty())
        return p;

    Page game{"Game", icon::game, "Back to the game, or leave it.", {}};
    game.rows.push_back(action("Resume", "Carry on from where you paused.", Cmd::resume, icon::play));
    Row title = action("Quit to title", "Back to the title screen, as Esc did on the Amiga.", Cmd::quit_to_title,
                       icon::home);
    title.ask_title = "Quit to the title screen?";
    title.ask_body = "Anything since your last save will be lost.";
    title.ask_button = "Quit to title";
    game.rows.push_back(title);
    Row quit = action("Quit game", "Close James Pond 2.", Cmd::exit_game, icon::quit);
    quit.ask_title = "Quit James Pond 2?";
    quit.ask_body = "Anything since your last save will be lost.";
    quit.ask_button = "Quit";
    game.rows.push_back(quit);
    p.push_back(game);

    for (int load = 0; load < 2; load++) {
        Page sl{load ? "Load" : "Save", load ? icon::load : icon::save,
                load ? "Pick up from a save." : "Keep your progress in one of four slots.", {}};
        for (int n = 0; n < kSlots; n++) {
            Row r{kSlotNames[n], nullptr, Kind::slot};
            r.slot = n;
            r.req = {load ? Cmd::load : Cmd::save, n};
            if (load) {
                r.ask_title = "Load this save?";
                r.ask_body = "Anything since your last save will be lost.";
                r.ask_button = "Load";
            }
            sl.rows.push_back(r);
        }
        p.push_back(sl);
    }

    Page levels{"Levels", icon::star, "Any level of the game, through its own map selector.", {}};
    {
        Row r{"World", "The game's seven worlds, each with its own look.", Kind::stepper};
        r.get = [](const View &) { return world_no; };
        r.set = [](const View &, int x) {
            world_no = x;
            level_no = 0;
            return Request{};
        };
        r.count = [](const View &) { return int(worlds().size()); };
        r.text = [](const View &) { return std::string(worlds()[size_t(world_no)].name); };
        levels.rows.push_back(r);
    }
    {
        Row r{"Level", "Its levels in the order the game plays them.", Kind::stepper};
        r.get = [](const View &) { return level_no; };
        r.set = [](const View &, int x) {
            level_no = x;
            return Request{};
        };
        r.count = [](const View &) { return int(worlds()[size_t(world_no)].levels.size()); };
        r.text = [](const View &) {
            const World &w = worlds()[size_t(world_no)];
            const Level &l = w.levels[size_t(level_no)];
            std::string t = std::to_string(level_no + 1) + " of " + std::to_string(w.levels.size());
            return l.tag ? t + "  \xC2\xB7  " + l.tag : t;
        };
        levels.rows.push_back(r);
    }
    {
        Row r = action("Play this level", "Starts a new game there: the score starts at 0, with three lives.",
                       Cmd::play_section, icon::play);
        r.ask_title = "";   // asked with the level's name, in input()
        levels.rows.push_back(r);
    }
    p.push_back(levels);

    Page display{"Display", icon::display, "How the game fills the screen.", {}};
    {
        Row r = toggle(
            "Widescreen", "Shows more of the level at the sides, drawn from the game's own map.",
            [](const View &v) { return int(S(v).widescreen); },
            [](const View &v, int x) { S(v).widescreen = x != 0; return Request{}; });
        r.preview = true;
        display.rows.push_back(r);
    }
    display.rows.push_back(toggle(
        "Smooth motion",
        "Draws the frames in between the game's 50 a second, so scrolling is smooth at 60, 90 or 120 Hz.",
        [](const View &v) { return int(S(v).smooth); },
        [](const View &v, int x) { S(v).smooth = x != 0; return Request{}; }));
    {
        Row r = choice(
            "Picture",
            "Crisp keeps every pixel sharp and even. Pixel perfect scales by whole numbers with a border. Soft "
            "blends a little. CRT is a television of the time: soft lines, glow, scanlines and curved glass.",
            {"Crisp", "Pixel perfect", "Soft", "CRT"}, [](const View &v) { return S(v).picture; },
            [](const View &v, int x) { S(v).picture = x; return Request{}; });
        r.preview = true;
        display.rows.push_back(r);
    }
    {
        Row r = choice(
            "Scanlines", "Dark lines between the rows of pixels, like a television of the time.",
            {"Off", "Light", "Strong"}, [](const View &v) { return S(v).scanlines; },
            [](const View &v, int x) { S(v).scanlines = x; return Request{}; });
        r.preview = true;
        display.rows.push_back(r);
    }
    display.rows.push_back(toggle(
        "Frame rate counter", "Shows how many pictures a second reach the screen, in the top left corner.",
        [](const View &v) { return int(S(v).show_fps); },
        [](const View &v, int x) { S(v).show_fps = x != 0; return Request{}; }));
    {
        Row r = toggle(
            "Full screen", "Fill the display. On a Steam Deck the game always fills the screen.",
            [](const View &v) { return int(S(v).fullscreen); },
            [](const View &v, int x) { S(v).fullscreen = x != 0; return Request{}; });
        r.enabled = [](const View &v) { return v.can_fullscreen; };
        display.rows.push_back(r);
    }
    p.push_back(display);

    Page sound{"Sound", icon::sound, "How the Amiga's four sound channels reach your speakers.", {}};
    {
        Row r{"Volume", "How loud the music and sound effects play.", Kind::slider};
        r.lo = 0;
        r.hi = 10;
        r.get = [](const View &v) { return S(v).volume; };
        r.set = [](const View &v, int x) { S(v).volume = x; return Request{}; };
        sound.rows.push_back(r);
    }
    sound.rows.push_back(choice(
        "Amiga model",
        "Whose output filter the sound goes through. The Amiga 500's gives the warm sound most people remember; "
        "the Amiga 1200's is brighter; Unfiltered is the sound chip on its own.",
        {"Amiga 500", "Amiga 1200", "Unfiltered"}, [](const View &v) { return S(v).sound_model; },
        [](const View &v, int x) { S(v).sound_model = x; return Request{}; }));
    sound.rows.push_back(choice(
        "Stereo",
        "The Amiga plays each channel on one side only. Wide and Narrow blend the two sides, which is easier on "
        "headphones.",
        {"Amiga", "Wide", "Narrow", "Mono"}, [](const View &v) { return S(v).stereo; },
        [](const View &v, int x) { S(v).stereo = x; return Request{}; }));
    sound.rows.push_back(toggle(
        "Let cut-off sounds ring out",
        "Many of the game's sounds were cut off while still ringing. This lets them fade away over a moment "
        "instead. The Amiga didn't do this; turn it off for the original.",
        [](const View &v) { return int(S(v).ring_out); },
        [](const View &v, int x) { S(v).ring_out = x != 0; return Request{}; }));
    p.push_back(sound);

    Page cheats{"Cheats", icon::cheats, "The game's own cheats, without typing the secret word.", {}};
    {
        Row r = toggle(
            "Invincible", "Nothing can hurt Robocod. He sparkles while it is on.",
            [](const View &v) { return int(v.invincible); },
            [](const View &, int x) { return Request{Cmd::invincible, x}; });
        r.icon = icon::shield;
        cheats.rows.push_back(r);
    }
    {
        Row r = toggle(
            "Infinite lives", "The lives counter never goes down.",
            [](const View &v) { return int(v.infinite_lives); },
            [](const View &, int x) { return Request{Cmd::infinite_lives, x}; });
        r.icon = icon::infinite;
        cheats.rows.push_back(r);
    }
    {
        Row r = toggle(
            "Wings", "Robocod flies wherever he likes. Only during a level.", [](const View &v) { return int(v.wings); },
            [](const View &, int x) { return Request{Cmd::wings, x}; });
        r.icon = icon::flight;
        r.enabled = [](const View &v) { return v.in_level; };
        cheats.rows.push_back(r);
    }
    {
        Row r = action("Skip to the exit", "Takes Robocod straight to the end of the level. Only during a level.",
                       Cmd::skip_exit, icon::skip);
        r.enabled = [](const View &v) { return v.in_level; };
        cheats.rows.push_back(r);
    }
    p.push_back(cheats);

    Page controls{"Controls", icon::controls, "What each button and key does.", {}};
    controls.rows.push_back(toggle(
        "Up also jumps",
        "On the Amiga, pushing the joystick up jumped. Leave this off and only the jump button does, so the stick "
        "can't jump by accident.",
        [](const View &v) { return int(S(v).up_jumps); },
        [](const View &v, int x) { S(v).up_jumps = x != 0; return Request{}; }));
    {
        Row r{"Rumble",
              "A buzz when Robocod is hit or loses a life, a thump with each explosion, and a shake when the screen "
              "shakes. 5 is the standard strength; you feel each change.",
              Kind::slider};
        r.lo = 0;
        r.hi = 10;
        r.lo_is_off = true;
        r.get = [](const View &v) { return S(v).rumble ? S(v).rumble_strength : 0; };
        r.set = [](const View &v, int x) {
            S(v).rumble = x > 0;
            if (x > 0)
                S(v).rumble_strength = x;
            return Request{};
        };
        controls.rows.push_back(r);
    }
    for (const char *name : {"Move", "Jump", "Stretch", "Quick save and load", "Smooth motion", "This menu"})
        controls.rows.push_back(Row{name, nullptr, Kind::info});
    p.push_back(controls);
    return p;
}

// ---- state ---------------------------------------------------------------------------

struct Box {
    float x, y, w, h;
};

struct Dialog {
    bool open = false;
    const char *title = nullptr, *body = nullptr, *button = nullptr;
    int icon = 0;
    int focus = 0;   // 0 cancel, 1 confirm
    Request req;
};

bool showing;
bool continue_mode;             // the launch question instead of the pages
SDL_Texture *continue_thumb;
std::string continue_when;
int continue_focus = 1;         // 0 title screen, 1 continue
int page_no, row_no;
bool in_rows;
Dialog dialog;
Box focus_box;
bool focus_known;
float plate_y = kRailY;
float page_age = 1, open_age = 1, preview_amt;
std::string toast_text;
float toast_age = 10;
// Moving parts of the controls, per row of the open page.
float knob[16], seg_x[16], seg_w[16];
bool parts_known;

bool row_enabled(const Row &r, const View &v)
{
    if (r.kind == Kind::slot && r.req.cmd == Cmd::load && !v.slots[r.slot].exists)
        return false;
    return !r.enabled || r.enabled(v);
}
bool focusable(const Row &r) { return r.kind != Kind::info; }

float row_y(const Page &p, int i)
{
    if (p.rows[size_t(i)].kind == Kind::slot)
        return kRowsY + float(i) * kSlotPitch;
    if (p.rows[size_t(i)].kind == Kind::info) {   // under the toggles and the column names
        int first = 0;
        while (p.rows[size_t(first)].kind != Kind::info)
            first++;
        return kRowsY + float(first) * kRowPitch + 30 + float(i - first) * kInfoPitch;
    }
    return kRowsY + float(i) * kRowPitch;
}
float row_h(const Row &r) { return r.kind == Kind::slot ? kSlotH : r.kind == Kind::info ? kInfoH : kRowH; }
float row_wid(const Row &r) { return r.kind == Kind::slot ? kSlotListW : row_w(); }

Box target_box()
{
    if (!in_rows)
        return {kRailX, kRailY + float(page_no) * kRailPitch, kRailW, kRailH};
    const Page &p = pages()[size_t(page_no)];
    const Row &r = p.rows[size_t(row_no)];
    return {kRowX, row_y(p, row_no), row_wid(r), row_h(r)};
}

void ask(const char *title, const char *body, const char *button, int ic, Request req)
{
    dialog = Dialog{true, title, body, button, ic, 0, req};
}

void go_page(int n)
{
    n = std::clamp(n, 0, int(pages().size()) - 1);
    if (n == page_no)
        return;
    page_no = n;
    row_no = 0;
    page_age = 0;
    parts_known = false;
}

void move_row(int dir)
{
    const Page &p = pages()[size_t(page_no)];
    for (int r = row_no + dir; r >= 0 && r < int(p.rows.size()); r += dir)
        if (focusable(p.rows[size_t(r)])) {
            row_no = r;
            return;
        }
}

std::vector<std::string> wrap(const char *text, float size, float width)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&]() {
        if (word.empty())
            return;
        std::string tryline = line.empty() ? word : line + " " + word;
        if (!line.empty() && ui::text_width(ui::regular, size, tryline.c_str()) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = tryline;
        }
        word.clear();
    };
    for (const char *p = text; *p; p++) {
        if (*p == ' ')
            flush_word();
        else
            word += *p;
    }
    flush_word();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

float ease(float t) { return 1 - std::pow(1 - std::clamp(t, 0.0f, 1.0f), 3.0f); }

// ---- drawing pieces ------------------------------------------------------------------

float fade = 1;   // everything is drawn through this (opening, page changes, previews)
Color F(Color c) { return ui::alpha(c, fade); }

void draw_focus(Box b, float strength = 1)
{
    ui::glow(b.x, b.y, b.w, b.h, kRadius, 26, F(ui::alpha(kAccent, 0.20f * strength)));
    ui::rect_v(b.x, b.y, b.w, b.h, kRadius, F(ui::mix(kRaised, kInk, 0.04f)), F(kRaised));
    ui::ring(b.x, b.y, b.w, b.h, kRadius, 2, F(ui::alpha(kAccent, strength)));
}

void small_caps(float x, float y, const char *s, Color c, ui::Align a = ui::left)
{
    ui::text(ui::semibold, 15, x, y, s, F(c), a, 2.2f);
}

void draw_toggle(float xr, float cy, float pos, bool enabled)
{
    float w = 84, h = 44;
    Color track = ui::mix(ui::alpha(kInk, 0.14f), kAccent, pos);
    if (!enabled)
        track = ui::alpha(kInk, 0.07f);
    ui::rect(xr - w, cy - h / 2, w, h, h / 2, F(track));
    float d = 34, tx = xr - w + 5 + (w - d - 10) * pos;
    ui::rect(tx, cy - d / 2, d, d, d / 2, F(enabled ? ui::mix(kInk, kDeep, pos) : kFaint));
}

// Every choice side by side, the chosen one lit; the light slides between them.
void draw_choice(float xr, float cy, const Row &r, int value, bool focused, bool enabled, int idx, float k)
{
    float pad = 22, gap = 4, h = 44;
    std::vector<float> widths;
    float total = 8;
    for (const char *c : r.choices) {
        widths.push_back(ui::text_width(ui::semibold, 20, c) + pad * 2);
        total += widths.back() + gap;
    }
    total -= gap;
    float x0 = xr - total, x = x0 + 4;
    ui::rect(x0, cy - h / 2 - 4, total, h + 8, (h + 8) / 2, F(ui::alpha(kInk, 0.06f)));
    float sel_x = x, sel_w = widths[0];
    for (size_t i = 0; i < widths.size(); i++) {
        if (int(i) == value) {
            sel_x = x;
            sel_w = widths[i];
        }
        x += widths[i] + gap;
    }
    if (!parts_known || seg_w[idx] == 0) {
        seg_x[idx] = sel_x;
        seg_w[idx] = sel_w;
    }
    seg_x[idx] += (sel_x - seg_x[idx]) * std::min(1.0f, k * 1.4f);
    seg_w[idx] += (sel_w - seg_w[idx]) * std::min(1.0f, k * 1.4f);
    Color lit = focused ? kAccent : ui::alpha(kInk, 0.18f);
    if (enabled)
        ui::rect(seg_x[idx], cy - h / 2, seg_w[idx], h, h / 2, F(lit));
    x = x0 + 4;
    for (size_t i = 0; i < widths.size(); i++) {
        bool on = int(i) == value;
        Color c = !enabled ? kFaint : on ? (focused ? kDeep : kInk) : kDim;
        ui::text(ui::semibold, 20, x + widths[i] / 2, cy, r.choices[i], F(c), ui::center);
        x += widths[i] + gap;
    }
}

void draw_slider(float xr, float cy, const Row &r, int value, float pos)
{
    float x0 = xr - 470, x1 = xr - 64;
    float t = (pos - float(r.lo)) / float(r.hi - r.lo);
    ui::rect(x0, cy - 4, x1 - x0, 8, 4, F(ui::alpha(kInk, 0.12f)));
    ui::rect(x0, cy - 4, (x1 - x0) * t, 8, 4, F(kAccent));
    for (int i = r.lo + 1; i < r.hi; i++) {   // notches
        float nx = x0 + (x1 - x0) * float(i - r.lo) / float(r.hi - r.lo);
        ui::rect(nx - 1.5f, cy - 1.5f, 3, 3, 1.5f, F(ui::alpha(float(i) <= pos ? kDeep : kInk, 0.35f)));
    }
    float kx = x0 + (x1 - x0) * t;
    ui::rect(kx - 13, cy - 13, 26, 26, 13, F(kInk));
    char num[8];
    std::snprintf(num, sizeof num, "%d", value);
    ui::text(ui::semibold, 25, xr, cy, r.lo_is_off && value == r.lo ? "Off" : num, F(kInk), ui::right);
}

void draw_thumb(const Slot &s, float x, float y, float w, float h, float radius, bool enabled)
{
    if (s.thumb) {
        ui::image_rounded(s.thumb, x, y, w, h, radius, fade * (enabled ? 1.0f : 0.5f));
        ui::ring(x, y, w, h, radius, 1, F(ui::alpha(kInk, 0.16f)));
        return;
    }
    ui::rect_v(x, y, w, h, radius, F(ui::alpha(kInk, 0.07f)), F(ui::alpha(kInk, 0.03f)));
    ui::ring(x, y, w, h, radius, 1, F(ui::alpha(kInk, 0.08f)));
    if (!s.exists)
        ui::icon(icon::add, h * 0.36f, x + w / 2, y + h / 2, F(kFaint));
}

// A button picture for the hints and the controls table.
float draw_glyph(const View &v, Button b, float x, float cy, float size)
{
    return glyphs::button(v.style, b, x, cy, size, fade);
}

struct Hint {
    std::vector<Button> pad;
    std::vector<const char *> keys;
    const char *what;
};

float hint_width(const View &v, const Hint &h)
{
    float w = 0;
    if (v.pad)
        for (Button b : h.pad)
            w += glyphs::button_width(v.style, b, 34) + 6;
    else
        for (const char *k : h.keys)
            w += glyphs::key_width(k, 34) + 6;
    return w + 6 + ui::text_width(ui::regular, 21, h.what);
}

void draw_hints(const View &v, const std::vector<Hint> &hints)
{
    float total = 0;
    for (const Hint &h : hints)
        total += hint_width(v, h) + 34;
    float x = ui::page_width() - kMargin - (total - 34), cy = 1004;
    for (const Hint &h : hints) {
        float hx = x;
        if (v.pad)
            for (Button b : h.pad)
                hx += draw_glyph(v, b, hx, cy, 34) + 6;
        else
            for (const char *k : h.keys)
                hx += glyphs::key(k, hx, cy, 34, fade) + 6;
        ui::text(ui::regular, 21, hx + 6, cy, h.what, F(kDim));
        x += hint_width(v, h) + 34;
    }
}

// ---- the pages' extra pieces ----------------------------------------------------------

void draw_stats(const View &v, float y)
{
    float x = kRowX, w = row_w(), h = 200;
    ui::rect_v(x, y, w, h, kRadius, F(ui::alpha(kInk, 0.045f)), F(ui::alpha(kInk, 0.02f)));
    float col = w / 3;
    // The score with its thousands separated.
    std::string sc = std::to_string(v.score);
    for (int i = int(sc.size()) - 3; i > 0; i -= 3)
        sc.insert(size_t(i), ",");
    small_caps(x + 32, y + 46, "SCORE", kFaint);
    ui::text(ui::semibold, 46, x + 30, y + 108, sc.c_str(), F(kInk));
    small_caps(x + col + 32, y + 46, "LIVES", kFaint);
    for (int i = 0; i < std::min(v.lives, 6); i++)
        ui::icon(icon::heart, 36, x + col + 50 + float(i) * 44, y + 108, F(ui::rgb(0xFF6B81)));
    small_caps(x + col * 2 + 32, y + 46, "ENERGY", kFaint);
    for (int i = 0; i < 5; i++)
        ui::icon(icon::bolt, 36, x + col * 2 + 50 + float(i) * 40, y + 108,
                 F(i < v.energy ? kAccent : ui::alpha(kInk, 0.15f)));
    // Cheats in use, as small tags under the numbers.
    float tx = x + 32;
    const std::pair<bool, const char *> tags[] = {
        {v.invincible, "Invincible"}, {v.infinite_lives, "Infinite lives"}, {v.wings, "Wings"}};
    for (auto [on, name] : tags) {
        if (!on)
            continue;
        float tw = ui::text_width(ui::semibold, 16, name) + 28;
        ui::rect(tx, y + 146, tw, 30, 15, F(ui::alpha(kAccent, 0.16f)));
        ui::text(ui::semibold, 16, tx + tw / 2, y + 161, name, F(kAccent), ui::center);
        tx += tw + 10;
    }
    if (!v.in_level)
        ui::text(ui::regular, 19, x + 32, y + 161, "Not in a level", F(kFaint));
}

void draw_slot_detail(const View &v, const Row &r)
{
    const Slot &s = v.slots[r.slot];
    // The picture as big as fits above the description, centred in its column.
    float x = kRowX + kSlotListW + 28, avail = row_right() - x;
    float h = std::min(avail * 9 / 16, 318.0f), w = h * 16 / 9, y = kRowsY;
    x += (avail - w) / 2;
    bool load = r.req.cmd == Cmd::load;
    draw_thumb(s, x, y, w, h, 14, true);
    if (!s.exists)
        ui::text(ui::regular, 21, x + w / 2, y + h / 2 + h * 0.2f, load ? "Nothing saved here" : "Empty slot",
                 F(kFaint), ui::center);
    ui::text(ui::semibold, 30, x, y + h + 40, r.label, F(kInk));
    if (s.exists) {
        ui::icon(icon::clock, 22, x + 11, y + h + 80, F(kDim));
        ui::text(ui::regular, 21, x + 30, y + h + 80, s.when.c_str(), F(kDim));
    }
    if (r.slot == 0) {   // the quick slot: say how play reaches it
        float gx = x, gy = y + h + 122;
        gx += draw_glyph(v, Button::l1, gx, gy, 30) + 10;
        gx += ui::text(ui::regular, 20, gx, gy, "saves here during play,", F(kDim)) + 12;
        gx += draw_glyph(v, Button::r1, gx, gy, 30) + 10;
        ui::text(ui::regular, 20, gx, gy, "loads it.", F(kDim));
    }
}

void draw_sound_card(const View &v, float y)
{
    float x = kRowX, w = row_w(), h = 150;
    ui::rect_v(x, y, w, h, kRadius, F(ui::alpha(kInk, 0.045f)), F(ui::alpha(kInk, 0.02f)));
    ui::icon(icon::music, 26, x + 44, y + 38, F(kAccent));
    float lx = x + 70 + ui::text_width(ui::semibold, 15, "LISTENING", 2.2f) + 16;
    small_caps(x + 70, y + 38, "LISTENING", kFaint);
    ui::text(ui::regular, 20, lx, y + 38, "the last few seconds of the game, through these settings", F(kDim));
    // Left and right, as two meters of lit bars.
    for (int side = 0; side < 2; side++) {
        float level = std::clamp(side ? v.level_r : v.level_l, 0.0f, 1.0f);
        float my = y + 82 + float(side) * 36;
        ui::text(ui::semibold, 17, x + 32, my, side ? "R" : "L", F(kFaint));
        int bars = 40;
        float bx = x + 64, bw = (w - 64 - 32) / float(bars);
        for (int i = 0; i < bars; i++) {
            bool lit = float(i) < level * float(bars);
            Color c = float(i) > float(bars) * 0.85f ? ui::rgb(0xFF6B81) : kAccent;
            ui::rect(bx + float(i) * bw, my - 9, bw - 4, 18, 3, F(lit ? c : ui::alpha(kInk, 0.07f)));
        }
    }
}

// The chosen world's levels as a row of tags, the chosen one lit, and where
// the game is now.
void draw_levels_card(const View &v, float y)
{
    float x = kRowX, w = row_w(), h = 200;
    ui::rect_v(x, y, w, h, kRadius, F(ui::alpha(kInk, 0.045f)), F(ui::alpha(kInk, 0.02f)));
    const World &world = worlds()[size_t(world_no)];
    small_caps(x + 32, y + 38, "IN THIS WORLD", kFaint);
    float cx = x + 32, cy = y + 64;
    for (size_t i = 0; i < world.levels.size(); i++) {
        const Level &l = world.levels[i];
        std::string t = l.tag && std::string(l.tag) == "Boss"         ? "Boss"
                        : l.tag && std::string(l.tag) == "Bonus room" ? "Bonus"
                                                                      : std::to_string(i + 1);
        float tw = std::max(44.0f, ui::text_width(ui::semibold, 18, t.c_str()) + 26);
        if (cx + tw > x + w - 32) {
            cx = x + 32;
            cy += 46;
        }
        bool chosen = int(i) == level_no, here = l.section == v.section;
        ui::rect(cx, cy, tw, 36, 18, F(chosen ? kAccent : ui::alpha(kInk, here ? 0.18f : 0.07f)));
        if (here && !chosen)
            ui::ring(cx, cy, tw, 36, 18, 1.5f, F(ui::alpha(kAccent, 0.8f)));
        ui::text(ui::semibold, 18, cx + tw / 2, cy + 18, t.c_str(), F(chosen ? kDeep : kInk), ui::center);
        cx += tw + 8;
    }
    // Where the game is now.
    std::string where = "Not in a level";
    for (size_t wi = 0; wi < worlds().size(); wi++)
        for (size_t li = 0; li < worlds()[wi].levels.size(); li++)
            if (worlds()[wi].levels[li].section == v.section)
                where = std::string("You're in ") + worlds()[wi].name + ", level " + std::to_string(li + 1);
    ui::icon(icon::eye, 22, x + 43, y + h - 30, F(kFaint));
    ui::text(ui::regular, 20, x + 66, y + h - 30, where.c_str(), F(kDim));
}

// The controls table: the pad's buttons and the keyboard's keys.
void draw_controls_row(const View &v, const Row &r, float y)
{
    float cy = y + kInfoH / 2, kx = row_right() - 28, px = kx - 300;
    std::vector<Button> pad;
    std::vector<const char *> keys;
    std::string what = r.label;
    if (what == "Move") {
        pad = {Button::dpad, Button::lstick};
        keys = {"\xE2\x86\x90", "\xE2\x86\x92"};
    } else if (what == "Jump") {
        pad = {Button::south};
        if (S(v).up_jumps)
            pad.push_back(Button::dpad_up);
        keys = {"\xE2\x86\x91"};
    } else if (what == "Stretch") {
        pad = {Button::east, Button::west};
        keys = {"Space"};
    } else if (what == "Quick save and load") {
        pad = {Button::l1, Button::r1};
        keys = {"F5", "F8"};
    } else if (what == "Smooth motion") {
        pad = {Button::north};
        keys = {"4"};
    } else {
        pad = {Button::menu, Button::view};
        keys = {"Esc"};
    }
    ui::text(ui::regular, 24, kRowX + 28, cy, r.label, F(kInk));
    float w = 0;
    for (Button b : pad)
        w += glyphs::button_width(v.style, b, 36) + 8;
    float gx = px - (w - 8);
    for (Button b : pad)
        gx += draw_glyph(v, b, gx, cy, 36) + 8;
    w = 0;
    for (const char *k : keys)
        w += glyphs::key_width(k, 36) + 8;
    gx = kx - (w - 8);
    for (const char *k : keys)
        gx += glyphs::key(k, gx, cy, 36, fade) + 8;
}

} // namespace

void open_continue(SDL_Texture *thumb, const std::string &when)
{
    showing = true;
    continue_mode = true;
    continue_thumb = thumb;
    continue_when = when;
    continue_focus = 1;
    open_age = 0;
}

bool asking_continue() { return showing && continue_mode; }

void open()
{
    continue_mode = false;
    levels_synced = false;
    showing = true;
    in_rows = false;
    dialog = Dialog{};
    open_age = 0;
    focus_known = false;
    parts_known = false;
    preview_amt = 0;
}

bool is_open() { return showing; }

void close()
{
    showing = false;
    continue_mode = false;
    dialog.open = false;
}

void show_for_test(int page, int row, bool rows, bool with_dialog)
{
    open();
    page_no = std::clamp(page, 0, int(pages().size()) - 1);
    row_no = std::clamp(row, 0, int(pages()[size_t(page_no)].rows.size()) - 1);
    in_rows = rows;
    page_age = open_age = 10;
    if (rows && pages()[size_t(page_no)].rows[size_t(row_no)].preview)
        preview_amt = 1;
    if (with_dialog) {
        const Row &r = pages()[kGame].rows[1];
        ask(r.ask_title, r.ask_body, r.ask_button, r.icon, r.req);
    }
}

void toast(const char *text)
{
    toast_text = text;
    toast_age = 0;
}

float preview() { return showing ? preview_amt : 0; }
bool sound_preview() { return showing && page_no == kSound && !dialog.open; }

Request input(Action a, const View &v)
{
    const std::vector<Page> &ps = pages();
    if (continue_mode) {
        switch (a) {
        case Action::left: continue_focus = 0; break;
        case Action::right: continue_focus = 1; break;
        case Action::confirm: return {continue_focus ? Cmd::continue_game : Cmd::start_fresh};
        case Action::back: return {Cmd::start_fresh};
        case Action::close: return {Cmd::continue_game};
        default: break;
        }
        return {};
    }
    if (dialog.open) {
        switch (a) {
        case Action::left: dialog.focus = 0; break;
        case Action::right: dialog.focus = 1; break;
        case Action::back:
        case Action::close: dialog.open = false; break;
        case Action::confirm:
            dialog.open = false;
            if (dialog.focus == 1)
                return dialog.req;
            break;
        default: break;
        }
        return {};
    }
    if (a == Action::close)
        return {Cmd::resume};
    if (a == Action::prev_page || a == Action::next_page) {
        go_page(page_no + (a == Action::next_page ? 1 : -1));
        if (in_rows) {
            row_no = -1;
            move_row(1);
        }
        return {};
    }
    if (!in_rows) {
        switch (a) {
        case Action::up: go_page(page_no - 1); break;
        case Action::down: go_page(page_no + 1); break;
        case Action::right:
        case Action::confirm:
            in_rows = true;
            row_no = -1;
            move_row(1);
            break;
        case Action::back: return {Cmd::resume};
        default: break;
        }
        return {};
    }
    const Page &p = ps[size_t(page_no)];
    const Row &r = p.rows[size_t(row_no)];
    bool on = row_enabled(r, v);
    switch (a) {
    case Action::up: move_row(-1); break;
    case Action::down: move_row(1); break;
    case Action::back: in_rows = false; break;
    case Action::left:
    case Action::right: {
        int dir = a == Action::right ? 1 : -1;
        if (r.kind == Kind::stepper && on) {
            int x = r.get(v) + dir;
            if (x >= 0 && x < r.count(v))
                return r.set(v, x);
        } else if (r.kind == Kind::choice && on) {
            int x = r.get(v) + dir;
            if (x >= 0 && x < int(r.choices.size()))
                return r.set(v, x);
        } else if (r.kind == Kind::slider && on) {
            int x = r.get(v) + dir;
            if (x >= r.lo && x <= r.hi)
                return r.set(v, x);
        } else if (r.kind == Kind::toggle && on) {
            if ((r.get(v) != 0) != (dir > 0))
                return r.set(v, dir > 0);
        } else if (a == Action::left) {
            in_rows = false;
        }
        break;
    }
    case Action::confirm:
        if (!on)
            break;
        if (r.kind == Kind::toggle)
            return r.set(v, !r.get(v));
        if (r.kind == Kind::choice)
            return r.set(v, (r.get(v) + 1) % int(r.choices.size()));
        if (r.kind == Kind::action && r.req.cmd == Cmd::play_section) {
            const World &w = worlds()[size_t(world_no)];
            static std::string title;
            title = "Play " + std::string(w.name) + ", level " + std::to_string(level_no + 1) + "?";
            ask(title.c_str(), "It starts a new game: score 0, three lives.", "Play", icon::star,
                {Cmd::play_section, w.levels[size_t(level_no)].section});
            return {};
        }
        if (r.kind == Kind::stepper)
            return {};
        if (r.kind == Kind::action) {
            if (r.ask_title) {
                ask(r.ask_title, r.ask_body, r.ask_button, r.icon, r.req);
                return {};
            }
            return r.req;
        }
        if (r.kind == Kind::slot) {
            if (r.req.cmd == Cmd::load) {
                ask(r.ask_title, r.ask_body, r.ask_button, icon::load, r.req);
                return {};
            }
            if (!v.slots[r.slot].exists)
                return r.req;
            static std::string title;
            title = std::string("Replace the save in ") + (r.slot == 0 ? "the quick slot" : r.label) + "?";
            ask(title.c_str(), "The save that is there now will be gone.", "Replace", icon::save, r.req);
            return {};
        }
        break;
    default: break;
    }
    return {};
}

// The launch question: a card over the picture of where the game was left.
static void draw_continue(const View &v, float dt)
{
    open_age += dt;
    fade = ease(open_age / 0.25f);
    float pw = ui::page_width();
    float cw = 1060, ch = 470, cx = (pw - cw) / 2, cy = 300;
    ui::glow(cx, cy, cw, ch, 28, 50, F(ui::alpha(kDeep, 0.6f)));
    ui::rect_v(cx, cy, cw, ch, 28, F(ui::alpha(kSurface, 0.97f)), F(ui::alpha(kSurface2, 0.97f)));
    ui::ring(cx, cy, cw, ch, 28, 1, F(ui::alpha(kInk, 0.08f)));
    float tw = 448, th = tw * 9 / 16, tx = cx + 48, ty = cy + 48;
    if (continue_thumb) {
        ui::image_rounded(continue_thumb, tx, ty, tw, th, 14, fade);
        ui::ring(tx, ty, tw, th, 14, 1, F(ui::alpha(kInk, 0.16f)));
    }
    float x = tx + tw + 48;
    ui::text(ui::semibold, 16, x, ty + 22, "WELCOME BACK", F(kAccent), ui::left, 3);
    ui::text(ui::semibold, 40, x, ty + 82, "Carry on where", F(kInk));
    ui::text(ui::semibold, 40, x, ty + 132, "you left off?", F(kInk));
    ui::icon(icon::clock, 22, x + 11, ty + 192, F(kDim));
    ui::text(ui::regular, 22, x + 32, ty + 192, continue_when.c_str(), F(kDim));
    float bw = (cw - 48 * 2 - 20) / 2, by = cy + ch - 48 - 72;
    const char *labels[2] = {"Title screen", "Continue"};
    const int icons[2] = {icon::home, icon::play};
    for (int i = 0; i < 2; i++) {
        float bx = cx + 48 + float(i) * (bw + 20);
        if (continue_focus == i)
            draw_focus({bx, by, bw, 72});
        else
            ui::rect(bx, by, bw, 72, kRadius, F(ui::alpha(kInk, 0.06f)));
        float lw = ui::text_width(ui::semibold, 25, labels[i]);
        ui::icon(icons[i], 28, bx + bw / 2 - lw / 2 - 22, by + 36, F(continue_focus == i ? kAccent : kDim));
        ui::text(ui::semibold, 25, bx + bw / 2 + 14, by + 36, labels[i], F(kInk), ui::center);
    }
    std::vector<Hint> hints = {{{Button::south}, {"Enter"}, "Choose"}};
    draw_hints(v, hints);
}

void draw(const View &v, float dt)
{
    if (continue_mode) {
        draw_continue(v, dt);
        return;
    }
    if (!levels_synced) {   // the Levels page starts at the level being played
        levels_synced = true;
        for (size_t wi = 0; wi < worlds().size(); wi++)
            for (size_t li = 0; li < worlds()[wi].levels.size(); li++)
                if (worlds()[wi].levels[li].section == v.section) {
                    world_no = int(wi);
                    level_no = int(li);
                }
    }
    const std::vector<Page> &ps = pages();
    const Page &page = ps[size_t(page_no)];
    const Row *focused_row = in_rows ? &page.rows[size_t(row_no)] : nullptr;
    page_age += dt;
    open_age += dt;
    toast_age += dt;
    float k = 1 - std::exp(-dt * 16);   // how far the springs move this frame
    float opened = ease(open_age / 0.18f);
    float want = focused_row && focused_row->preview && !dialog.open ? 1.0f : 0.0f;
    preview_amt += (want - preview_amt) * (1 - std::exp(-dt * 10));
    float p = preview_amt;
    float pw = ui::page_width();

    // Behind it all the host has drawn the game, frosted; a fine grid of dots over it.
    fade = opened * (1 - p);
    ui::dots(0, 0, pw, 1080, 32, 2, F(ui::alpha(kInk, 0.035f)));

    // The page itself fades to a ghost while a picture setting is previewed.
    fade = opened * (1 - p * 0.86f);

    // Header.
    ui::text(ui::semibold, 16, kMargin, 108, "JAMES POND 2  \xC2\xB7  CODENAME: ROBOCOD", F(kAccent), ui::left, 3);
    ui::text(ui::semibold, 64, kMargin - 3, 166, "Paused", F(kInk));

    // Rail: the open page has a plate and a light at its edge.
    float plate_to = kRailY + float(page_no) * kRailPitch;
    plate_y += (plate_to - plate_y) * k;
    ui::rect(kRailX, plate_y, kRailW, kRailH, kRadius, F(ui::alpha(kRaised, 0.85f)));
    ui::rect(kRailX + 8, plate_y + 18, 4, kRailH - 36, 2, F(kAccent));

    // Panel.
    ui::rect_v(kPanelX, kPanelY, panel_w(), kPanelH, 28, F(ui::alpha(kSurface, 0.96f)),
               F(ui::alpha(kSurface2, 0.96f)));
    ui::ring(kPanelX, kPanelY, panel_w(), kPanelH, 28, 1, F(ui::alpha(kInk, 0.07f)));
    ui::icon(page.icon, 34, kPanelX + 62, kPanelY + 58, F(kAccent));
    ui::text(ui::semibold, 34, kPanelX + 94, kPanelY + 58, page.name, F(kInk));

    // The one focus, travelling between the rail and the rows.
    Box to = target_box();
    if (!focus_known) {
        focus_box = to;
        focus_known = true;
    }
    focus_box.x += (to.x - focus_box.x) * k;
    focus_box.y += (to.y - focus_box.y) * k;
    focus_box.w += (to.w - focus_box.w) * k;
    focus_box.h += (to.h - focus_box.h) * k;
    if (!in_rows && !dialog.open)
        draw_focus(focus_box);

    for (size_t i = 0; i < ps.size(); i++) {
        float cy = kRailY + float(i) * kRailPitch + kRailH / 2;
        bool current = int(i) == page_no;
        ui::icon(ps[i].icon, 28, kRailX + 46, cy, F(current ? kAccent : kDim));
        ui::text(current ? ui::semibold : ui::regular, 25, kRailX + 78, cy, ps[i].name, F(current ? kInk : kDim));
    }

    // Rows, arriving one after another when the page changes. The focused
    // one stays solid during a preview, so its control can still be read.
    float page_fade = fade;
    for (size_t i = 0; i < page.rows.size(); i++) {
        const Row &r = page.rows[i];
        bool is_focused = in_rows && int(i) == row_no;
        float a = ease((page_age - float(i) * 0.035f) / 0.2f);
        fade = (is_focused ? opened : page_fade) * a;
        float y = row_y(page, int(i)) + (1 - a) * 12, h = row_h(r), cy = y + h / 2;
        bool on = row_enabled(r, v);
        float xr = row_right() - 28;
        if (r.kind == Kind::info) {
            draw_controls_row(v, r, y);
            continue;
        }
        float rw = row_wid(r);
        if (is_focused && !dialog.open)
            draw_focus({focus_box.x, focus_box.y + (1 - a) * 12, focus_box.w, focus_box.h});
        else
            ui::rect(kRowX, y, rw, h, kRadius, F(ui::alpha(kInk, 0.035f)));
        if (r.kind == Kind::slot) {
            const Slot &s = v.slots[r.slot];
            draw_thumb(s, kRowX + 16, y + 16, 128, 72, 8, on);
            ui::text(ui::regular, 24, kRowX + 164, y + 38, r.label, F(on ? kInk : kFaint));
            ui::text(ui::regular, 19, kRowX + 164, y + 70, s.exists ? s.when.c_str() : "Empty",
                     F(s.exists ? kDim : kFaint));
            continue;
        }
        float lx = kRowX + 28;
        if (r.icon) {
            ui::icon(r.icon, 26, kRowX + 44, cy, F(on ? (is_focused ? kAccent : kDim) : kFaint));
            lx = kRowX + 74;
        }
        ui::text(ui::regular, 25, lx, cy, r.label, F(on ? kInk : kFaint));
        int idx = int(i) & 15;
        switch (r.kind) {
        case Kind::toggle: {
            float target = r.get(v) != 0 ? 1.0f : 0.0f;
            if (!parts_known)
                knob[idx] = target;
            knob[idx] += (target - knob[idx]) * std::min(1.0f, k * 1.4f);
            draw_toggle(xr, cy, knob[idx], on);
            break;
        }
        case Kind::choice: draw_choice(xr, cy, r, r.get(v), is_focused, on, idx, k); break;
        case Kind::stepper: {
            float w = 380;
            int x = r.get(v), n = r.count(v);
            Color edge = is_focused ? kAccent : kDim;
            ui::text(ui::semibold, 30, xr - w + 10, cy - 2, "\xE2\x80\xB9", F(x > 0 ? edge : ui::alpha(kDim, 0.25f)),
                     ui::center);
            ui::text(ui::semibold, 30, xr - 10, cy - 2, "\xE2\x80\xBA", F(x < n - 1 ? edge : ui::alpha(kDim, 0.25f)),
                     ui::center);
            ui::text(ui::semibold, 23, xr - w / 2, cy, r.text(v).c_str(), F(kInk), ui::center);
            break;
        }
        case Kind::slider: {
            float target = float(r.get(v));
            if (!parts_known)
                knob[idx] = target;
            knob[idx] += (target - knob[idx]) * std::min(1.0f, k * 1.4f);
            draw_slider(xr, cy, r, r.get(v), knob[idx]);
            break;
        }
        case Kind::action:
            if (r.ask_title)
                ui::text(ui::semibold, 30, xr, cy - 2, "\xE2\x80\xBA", F(kFaint), ui::right);
            break;
        default: break;
        }
    }
    parts_known = true;
    fade = page_fade * ease((page_age - 0.12f) / 0.25f);
    if (page_no == kGame)
        draw_stats(v, kRowsY + 3 * kRowPitch + 24);
    if (page_no == kSound)
        draw_sound_card(v, kRowsY + float(page.rows.size()) * kRowPitch + 14);
    if (page_no == kLevels)
        draw_levels_card(v, kRowsY + float(page.rows.size()) * kRowPitch + 14);
    if (page_no == kSave || page_no == kLoad)
        draw_slot_detail(v, page.rows[size_t(in_rows ? row_no : 0)]);
    if (page_no == kControls) {
        int first = 0;
        while (page.rows[size_t(first)].kind != Kind::info)
            first++;
        float hy = kRowsY + float(first) * kRowPitch + 14, kx = row_right() - 28;
        small_caps(kx - 300, hy, "CONTROLLER", kFaint, ui::right);
        small_caps(kx, hy, "KEYBOARD", kFaint, ui::right);
    }
    fade = page_fade;

    // What the focused thing does, under a hairline.
    ui::line(kPanelX + 44, kFooterY, panel_w() - 88, F(ui::alpha(kInk, 0.09f)));
    const char *desc = focused_row && focused_row->desc ? focused_row->desc : page.summary;
    if (focused_row && focused_row->kind == Kind::slot)
        desc = focused_row->req.cmd == Cmd::load ? "Loads this save in place of the game you're playing."
                                                  : "Saves the game as it is now into this slot.";
    std::vector<std::string> lines = wrap(desc, 21, panel_w() - 88 - 40);
    float y0 = kFooterY + (lines.size() > 1 ? 27 : 42);
    ui::icon(icon::info, 24, kPanelX + 56, y0, F(kFaint));
    for (size_t i = 0; i < lines.size() && i < 2; i++)
        ui::text(ui::regular, 21, kPanelX + 80, y0 + float(i) * 30, lines[i].c_str(), F(kDim));

    // Button hints, solid even during a preview.
    fade = opened;
    std::vector<Hint> hints;
    if (dialog.open) {
        hints = {{{Button::south}, {"Enter"}, "Choose"}, {{Button::east}, {"Esc"}, "Cancel"}};
    } else {
        const char *what = "Select";
        if (focused_row && focused_row->kind == Kind::slot)
            what = focused_row->req.cmd == Cmd::load ? "Load" : "Save here";
        else if (focused_row && (focused_row->kind == Kind::choice || focused_row->kind == Kind::slider ||
                                 focused_row->kind == Kind::stepper))
            what = "Change";
        hints.push_back({{Button::south}, {"Enter"}, what});
        hints.push_back({{Button::east}, {"Esc"}, in_rows ? "Back" : "Resume"});
        hints.push_back({{Button::l1, Button::r1}, {"Tab"}, "Page"});
        if (v.pad)
            hints.push_back({{Button::menu}, {}, "Resume"});
    }
    draw_hints(v, hints);

    // A live-preview tag while the game shows through.
    if (p > 0.02f) {
        fade = opened * p;
        float tw = ui::text_width(ui::semibold, 19, "Live preview") + 74;
        ui::rect(pw - kMargin - tw, 86, tw, 48, 24, F(ui::alpha(kDeep, 0.85f)));
        ui::icon(icon::eye, 24, pw - kMargin - tw + 30, 110, F(kAccent));
        ui::text(ui::semibold, 19, pw - kMargin - tw + 52, 110, "Live preview", F(kInk));
    }

    // A short note after an action, top right.
    if (toast_age < 2.0f) {
        float a = std::min(ease(toast_age / 0.15f), 1 - ease((toast_age - 1.6f) / 0.4f));
        fade = opened * a;
        float tw = ui::text_width(ui::semibold, 20, toast_text.c_str()) + 78, ty = 86 - (1 - a) * 8;
        ui::rect(pw - kMargin - tw, ty, tw, 48, 24, F(kRaised));
        ui::ring(pw - kMargin - tw, ty, tw, 48, 24, 1, F(ui::alpha(kAccent, 0.5f)));
        ui::icon(icon::done, 26, pw - kMargin - tw + 30, ty + 24, F(kAccent));
        ui::text(ui::semibold, 20, pw - kMargin - tw + 54, ty + 24, toast_text.c_str(), F(kInk));
    }

    // The question, over everything.
    if (dialog.open) {
        fade = opened;
        ui::fill_window(F(ui::alpha(kBg, 0.7f)));
        float dw = 820, dh = 400, dx = (pw - dw) / 2, dy = 340;
        ui::glow(dx, dy, dw, dh, 28, 40, F(ui::alpha(kDeep, 0.6f)));
        ui::rect_v(dx, dy, dw, dh, 28, F(kSurface), F(kSurface2));
        ui::ring(dx, dy, dw, dh, 28, 1, F(ui::alpha(kInk, 0.09f)));
        if (dialog.icon) {
            ui::rect(dx + 56, dy + 52, 64, 64, 32, F(ui::alpha(kAccent, 0.14f)));
            ui::icon(dialog.icon, 32, dx + 88, dy + 84, F(kAccent));
        }
        ui::text(ui::semibold, 34, dx + 56, dy + 162, dialog.title, F(kInk));
        if (dialog.body)
            ui::text(ui::regular, 23, dx + 56, dy + 210, dialog.body, F(kDim));
        float bw = (dw - 56 * 2 - 20) / 2, by = dy + dh - 56 - 68;
        const char *labels[2] = {"Cancel", dialog.button};
        for (int i = 0; i < 2; i++) {
            float bx = dx + 56 + float(i) * (bw + 20);
            if (dialog.focus == i)
                draw_focus({bx, by, bw, 68});
            else
                ui::rect(bx, by, bw, 68, kRadius, F(ui::alpha(kInk, 0.06f)));
            ui::text(ui::semibold, 24, bx + bw / 2, by + 34, labels[i], F(kInk), ui::center);
        }
    }
}

} // namespace menu
