#include "setup.h"

#include "disk.h"
#include "theme.h"
#include "ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace setup {

namespace {

using namespace theme;
namespace icon = ui::icon_id;
using glyphs::Button;

#ifdef __PROSPERO__
// On the PlayStation 5 the game sees only its own folder (/app0), which the
// player reaches over FTP as /data/homebrew/<title>. There are no file pickers.
constexpr bool kConsole = true;
constexpr const char *kConsoleFolder = "/data/homebrew/" JP2_PS5_TITLE_ID;
#else
constexpr bool kConsole = false;
constexpr const char *kConsoleFolder = "";
#endif

// ---- looking for disks ---------------------------------------------------------------

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool named_like_the_game(const std::string &name)
{
    std::string n = lower(name);
    return n.find("pond") != std::string::npos || n.find("robocod") != std::string::npos ||
           n.rfind("jp2", 0) == 0;
}

bool disk_extension(const std::string &name)
{
    std::string n = lower(name);
    for (const char *e : {".adf", ".adz", ".zip"})
        if (n.size() > 4 && n.compare(n.size() - 4, 4, e) == 0)
            return true;
    return false;
}

struct Scan {
    std::vector<std::string> paths;   // to check, in the order found
    int depth;
    int seen;
    bool any_name = false;            // a folder only for the game: any disk image counts
};

void look(const std::string &dir, int depth, Scan &sc);

SDL_EnumerationResult on_entry(void *user, const char *dirname, const char *fname)
{
    Scan &sc = *static_cast<Scan *>(user);
    if (++sc.seen > 4000 || sc.paths.size() >= 12)
        return SDL_ENUM_SUCCESS;
    if (fname[0] == '.')
        return SDL_ENUM_CONTINUE;
    std::string path = std::string(dirname) + fname;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info))
        return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_FILE) {
        if ((sc.any_name || named_like_the_game(fname)) && disk_extension(fname) && info.size < (8 << 20))
            sc.paths.push_back(path);
    } else if (info.type == SDL_PATHTYPE_DIRECTORY && sc.depth > 0) {
        // A WHDLoad install keeps the game's files under data/.
        SDL_PathInfo whd;
        if (named_like_the_game(fname) && (SDL_GetPathInfo((path + "/data/jp2_000").c_str(), &whd) ||
                                           SDL_GetPathInfo((path + "/data/JP2_000").c_str(), &whd)))
            sc.paths.push_back(path);
        else
            look(path, sc.depth - 1, sc);
    }
    return SDL_ENUM_CONTINUE;
}

void look(const std::string &dir, int depth, Scan &sc)
{
    int saved = sc.depth;
    sc.depth = depth;
    SDL_EnumerateDirectory(dir.c_str(), on_entry, &sc);
    sc.depth = saved;
}

std::string home_path()
{
    const char *h = SDL_GetUserFolder(SDL_FOLDER_HOME);
    std::string s = h ? h : "";
    if (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

// ~/... for showing a path.
std::string short_path(const std::string &p)
{
    std::string home = home_path();
    if (!home.empty() && p.rfind(home, 0) == 0)
        return "~" + p.substr(home.size());
    return p;
}

std::string base_name(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string folder_of(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

// ---- the page ------------------------------------------------------------------------

struct Row {
    int icon;
    std::string label, detail;
    enum Kind { found, pick_file, pick_folder, rescan, quit } kind;
    std::string path;
};

struct State {
    std::vector<Row> rows;
    int focus = 0;
    float fy = 0, fh = 0;
    bool focus_known = false;
    std::string message;
    std::string hint;
    glyphs::Style style;
    bool pad = true;
    float age = 0;
};

constexpr float kPanelY = 236, kPanelH = 692;
constexpr float kIntroY = kPanelY + 70;

float panel_x() { return kMargin; }
float panel_w() { return ui::page_width() - kMargin * 2; }
float rows_top(const State &st)
{
    bool found = !st.rows.empty() && st.rows[0].kind == Row::found;
    return kPanelY + (found ? 218 : 178);
}
float row_h(const Row &r) { return r.kind == Row::found ? 86 : 72; }
float row_y(const State &st, int i)
{
    float y = rows_top(st);
    for (int k = 0; k < i; k++) {
        y += row_h(st.rows[size_t(k)]) + 8;
        if (st.rows[size_t(k)].kind == Row::found && st.rows[size_t(k) + 1].kind != Row::found)
            y += 20;   // a gap after the disks found
    }
    return y;
}

std::vector<std::string> wrap(const std::string &text, float size, float width)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&]() {
        if (word.empty())
            return;
        std::string t = line.empty() ? word : line + " " + word;
        if (!line.empty() && ui::text_width(ui::regular, size, t.c_str()) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = t;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ')
            flush();
        else
            word += c;
    }
    flush();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

void draw(State &st, float dt)
{
    st.age += dt;
    float a = 1 - std::pow(1 - std::min(1.0f, st.age / 0.25f), 3.0f);
    float pw = ui::page_width();
    ui::fill_window(kBg);
    ui::dots(0, 0, pw, 1080, 32, 2, ui::alpha(kInk, 0.035f * a));
    ui::text(ui::semibold, 16, kMargin, 108, "JAMES POND 2  \xC2\xB7  CODENAME: ROBOCOD", ui::alpha(kAccent, a), ui::left,
             3);
    ui::text(ui::semibold, 64, kMargin - 3, 166, "Find your disk", ui::alpha(kInk, a));

    float px = panel_x(), w = panel_w();
    ui::rect_v(px, kPanelY, w, kPanelH, 28, ui::alpha(kSurface, 0.96f * a), ui::alpha(kSurface2, 0.96f * a));
    ui::ring(px, kPanelY, w, kPanelH, 28, 1, ui::alpha(kInk, 0.07f * a));
    std::vector<std::string> intro =
        wrap(kConsole ? std::string("James Pond 2 plays from your own copy of the Amiga disk. Copy a disk image (an "
                                    ".adf, or an .adz or .zip with one inside) into the game's folder, ") +
                            kConsoleFolder + ", over FTP, then choose Look again. This happens only once."
                      : std::string("James Pond 2 plays from your own copy of the Amiga disk. Choose a disk image (an "
                                    ".adf, or an .adz or .zip with one inside) or the folder of a WHDLoad install. "
                                    "It's copied into the game's own folder, so this happens only once."),
             23, w - 96);
    for (size_t i = 0; i < intro.size() && i < 3; i++)
        ui::text(ui::regular, 23, px + 48, kIntroY + float(i) * 33, intro[i].c_str(), ui::alpha(kDim, a));
    bool any_found = !st.rows.empty() && st.rows[0].kind == Row::found;
    if (any_found)
        ui::text(ui::semibold, 15, px + 48, rows_top(st) - 26, kConsole ? "FOUND ON THIS CONSOLE" : "FOUND ON THIS COMPUTER",
                 ui::alpha(kFaint, a),
                 ui::left, 2.2f);

    float k = 1 - std::exp(-dt * 16);
    float ty = row_y(st, st.focus), th = row_h(st.rows[size_t(st.focus)]);
    if (!st.focus_known) {
        st.fy = ty;
        st.fh = th;
        st.focus_known = true;
    }
    st.fy += (ty - st.fy) * k;
    st.fh += (th - st.fh) * k;
    float rx = px + 24, rw = w - 48;
    for (size_t i = 0; i < st.rows.size(); i++) {
        const Row &r = st.rows[i];
        float y = row_y(st, int(i)), h = row_h(r), cy = y + h / 2;
        bool focused = int(i) == st.focus;
        if (focused)
            theme::focus(rx, st.fy, rw, st.fh, a);
        else
            ui::rect(rx, y, rw, h, kRadius, ui::alpha(kInk, 0.035f * a));
        ui::icon(r.icon, 28, rx + 46, cy, ui::alpha(focused ? kAccent : kDim, a));
        if (r.detail.empty()) {
            ui::text(ui::regular, 25, rx + 80, cy, r.label.c_str(), ui::alpha(kInk, a));
        } else {
            ui::text(ui::regular, 25, rx + 80, cy - 14, r.label.c_str(), ui::alpha(kInk, a));
            ui::text(ui::regular, 19, rx + 80, cy + 18, r.detail.c_str(), ui::alpha(kDim, a));
        }
    }

    // Why the last disk didn't work, or where else a disk can go.
    float fy = kPanelY + kPanelH - 84;
    ui::line(px + 44, fy, w - 88, ui::alpha(kInk, 0.09f * a));
    if (!st.message.empty()) {
        std::vector<std::string> lines = wrap(st.message, 21, w - 88 - 40);
        float y0 = fy + (lines.size() > 1 ? 27 : 42);
        ui::icon(icon::info, 24, px + 56, y0, ui::alpha(kWarn, a));
        for (size_t i = 0; i < lines.size() && i < 2; i++)
            ui::text(ui::regular, 21, px + 80, y0 + float(i) * 30, lines[i].c_str(), ui::alpha(kWarn, a));
    } else {
        std::string note = kConsole ? std::string("FTP is the console's file server, on port 2121 with most homebrew "
                                                  "loaders.")
                                    : "Or copy a disk image into " + st.hint + ", named disk.adf, and start the game again.";
        std::vector<std::string> lines = wrap(note, 21, w - 88 - 40);
        float y0 = fy + (lines.size() > 1 ? 27 : 42);
        ui::icon(icon::info, 24, px + 56, y0, ui::alpha(kFaint, a));
        for (size_t i = 0; i < lines.size() && i < 2; i++)
            ui::text(ui::regular, 21, px + 80, y0 + float(i) * 30, lines[i].c_str(), ui::alpha(kDim, a));
    }

    // Hints.
    struct Hint {
        Button b;
        const char *key, *what;
    } hints[] = {{Button::south, "Enter", "Select"}, {Button::east, "Esc", "Quit"}};
    float total = 0;
    for (const Hint &h : hints)
        total += (st.pad ? glyphs::button_width(st.style, h.b, 34) : glyphs::key_width(h.key, 34)) + 12 +
                 ui::text_width(ui::regular, 21, h.what) + 34;
    float x = pw - kMargin - (total - 34);
    for (const Hint &h : hints) {
        x += (st.pad ? glyphs::button(st.style, h.b, x, 1004, 34, a) : glyphs::key(h.key, x, 1004, 34, a)) + 12;
        x += ui::text(ui::regular, 21, x, 1004, h.what, ui::alpha(kDim, a)) + 34;
    }
}

void build_rows(State &st, const std::vector<std::string> &found)
{
    st.rows.clear();
    for (const std::string &p : found)
        st.rows.push_back({icon::save, base_name(p), kConsole ? kConsoleFolder : short_path(folder_of(p)), Row::found, p});
    if (kConsole) {
        st.rows.push_back({icon::folder, "Look again", "", Row::rescan, ""});
    } else {
        st.rows.push_back({icon::add, "Choose a disk image\xE2\x80\xA6", "", Row::pick_file, ""});
        st.rows.push_back({icon::folder, "Choose a WHDLoad folder\xE2\x80\xA6", "", Row::pick_folder, ""});
    }
    st.rows.push_back({icon::quit, "Quit", "", Row::quit, ""});
}

// What the system's pickers hand back, on whatever thread they call from.
std::mutex picked_lock;
std::string picked;
bool picked_ready;

void SDLCALL on_picked(void *, const char *const *files, int)
{
    std::lock_guard<std::mutex> g(picked_lock);
    if (files && files[0]) {
        picked = files[0];
        picked_ready = true;
    }
}

} // namespace

std::vector<std::string> scan(TryDisk try_disk)
{
    Scan sc{{}, 0, 0};
    if (kConsole) {   // the title's folder, and folders in it
        sc.any_name = true;
        look("/app0", 1, sc);
        sc.any_name = false;
    }
    std::string home = home_path();
    for (SDL_Folder f : {SDL_FOLDER_DOWNLOADS, SDL_FOLDER_DESKTOP, SDL_FOLDER_DOCUMENTS})
        if (const char *d = SDL_GetUserFolder(f))
            look(d, 2, sc);
    for (const char *sub : {"/Emulation/roms/amiga", "/Emulation/roms/amiga600", "/roms/amiga", "/Games"})
        look(home + sub, 2, sc);
    // Steam Deck SD cards (EmuDeck keeps its games there too).
    for (const char *media : {"/run/media/", "/run/media/deck/"}) {
        SDL_EnumerateDirectory(
            media,
            [](void *user, const char *dirname, const char *fname) -> SDL_EnumerationResult {
                Scan &s = *static_cast<Scan *>(user);
                std::string card = std::string(dirname) + fname;
                for (const char *sub : {"/Emulation/roms/amiga", "/Emulation/roms/amiga600", "/roms/amiga"})
                    look(card + sub, 2, s);
                return SDL_ENUM_CONTINUE;
            },
            &sc);
    }
    look(home, 0, sc);
    std::vector<std::string> good;
    for (const std::string &p : sc.paths) {
        std::string err;
        if (std::find(good.begin(), good.end(), p) == good.end() && try_disk(p, &err))
            good.push_back(p);
        if (good.size() >= 4)
            break;
    }
    return good;
}

std::string run(SDL_Window *win, SDL_Renderer *ren, TryDisk try_disk, const std::string &why, glyphs::Style style,
                const std::string &store_hint)
{
    State st;
    st.style = style;
    st.hint = store_hint;
    st.message = why;
    // Only check found disks for being James Pond 2: booting comes when one is chosen.
    build_rows(st, scan([](const std::string &p, std::string *err) { return disk::open(p.c_str(), err); }));
    SDL_Gamepad *pad = nullptr;
    bool stick_held = false;
    uint64_t last = SDL_GetTicksNS();
    auto attempt = [&](const std::string &path) -> bool {
        std::string err;
        if (try_disk(path, &err))
            return true;
        st.message = base_name(path) + ": " + err;
        return false;
    };
    // Tests: JP2_TEST_DROP=<path> drops that file on the page after a moment.
    const char *test_drop = std::getenv("JP2_TEST_DROP");
    uint64_t opened = last;
    for (;;) {
        if (test_drop && SDL_GetTicksNS() - opened > 800000000ull) {
            std::string p = test_drop;
            test_drop = nullptr;
            if (attempt(p))
                return p;
        }
        SDL_Event e;
        int act = 0;   // 1 activate, -1 quit
        int move = 0;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: return "";
            case SDL_EVENT_DROP_FILE:
                if (e.drop.data && attempt(e.drop.data))
                    return e.drop.data;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad)
                    pad = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                st.pad = true;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
                    act = 1;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
                    act = -1;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP)
                    move = -1;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                    move = 1;
                break;
            case SDL_EVENT_KEY_DOWN:
                st.pad = false;
                if (e.key.scancode == SDL_SCANCODE_UP)
                    move = -1;
                else if (e.key.scancode == SDL_SCANCODE_DOWN)
                    move = 1;
                else if (!e.key.repeat && (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_SPACE))
                    act = 1;
                else if (!e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE)
                    act = -1;
                break;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                // Mouse over a row focuses it; a click chooses it.
                float s = ui::scale();
                int ow = 0, oh = 0;
                SDL_GetRenderOutputSize(ren, &ow, &oh);
                float wx = 0, wy = 0;
                SDL_RenderCoordinatesFromWindow(ren, e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x,
                                                e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y, &wx, &wy);
                float py = (wy - (float(oh) - 1080 * s) / 2) / s, px = wx / s;
                for (size_t i = 0; i < st.rows.size(); i++) {
                    float y = row_y(st, int(i));
                    if (px > panel_x() && px < panel_x() + panel_w() && py >= y && py < y + row_h(st.rows[i])) {
                        st.focus = int(i);
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                            act = 1;
                    }
                }
                break;
            }
            }
        }
        if (pad) {
            Sint16 ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            bool held = std::abs(ly) > 20000;
            if (held && !stick_held)
                move = ly < 0 ? -1 : 1;
            stick_held = held;
        }
        st.focus = std::clamp(st.focus + move, 0, int(st.rows.size()) - 1);
        if (act < 0)
            return "";
        if (act > 0) {
            Row &r = st.rows[size_t(st.focus)];
            static const SDL_DialogFileFilter filters[] = {{"Amiga disk images", "adf;adz;zip"}, {"All files", "*"}};
            switch (r.kind) {
            case Row::found:
                if (attempt(r.path))
                    return r.path;
                break;
            case Row::pick_file:
                SDL_ShowOpenFileDialog(on_picked, nullptr, win, filters, 2, SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS),
                                       false);
                break;
            case Row::pick_folder: SDL_ShowOpenFolderDialog(on_picked, nullptr, win, nullptr, false); break;
            case Row::rescan:
                build_rows(st, scan([](const std::string &p, std::string *err) { return disk::open(p.c_str(), err); }));
                st.focus = 0;
                st.focus_known = false;
                if (st.rows[0].kind != Row::found)
                    st.message = std::string("No disk image in ") + kConsoleFolder + " yet.";
                else
                    st.message.clear();
                break;
            case Row::quit: return "";
            }
        }
        std::string chosen;
        {
            std::lock_guard<std::mutex> g(picked_lock);
            if (picked_ready) {
                chosen = picked;
                picked_ready = false;
            }
        }
        if (!chosen.empty() && attempt(chosen))
            return chosen;

        uint64_t now = SDL_GetTicksNS();
        float dt = std::min(0.1f, float(now - last) / 1e9f);
        last = now;
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        ui::begin(ow, oh);
        draw(st, dt);
        if (ui::console_shot_asked())
            ui::save_screen(ren, "/app0/userdata/shot.png");
        SDL_RenderPresent(ren);
    }
}

void draw_for_test(int w, int h, const std::vector<std::string> &found, const std::string &why, glyphs::Style style,
                   const std::string &store_hint)
{
    State st;
    st.style = style;
    st.hint = store_hint;
    st.message = why;
    build_rows(st, found);
    st.age = 10;
    ui::begin(w, h);
    draw(st, 1.0f);
}

} // namespace setup
