// Drawing for the menus: anti-aliased rounded shapes, text (Inter) and icons
// (Material Symbols), via stb_truetype, laid out on a page 1080 units tall
// whose width follows the window's shape (1920 at 16:9, 1728 on a Steam
// Deck), never under 1500. Every coordinate below is in page units.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

namespace ui {

struct Color {
    float r, g, b, a;
};
constexpr Color rgb(uint32_t hex, float a = 1.0f)
{
    return {float((hex >> 16) & 255) / 255.0f, float((hex >> 8) & 255) / 255.0f, float(hex & 255) / 255.0f, a};
}
inline Color alpha(Color c, float a) { return {c.r, c.g, c.b, c.a * a}; }
inline Color mix(Color a, Color b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

enum Font { regular, semibold, icons };
enum Align { left, center, right };

// Material Symbols codepoints the menus use.
namespace icon_id {
constexpr int game = 0xEA28, save = 0xE161, load = 0xE28E, display = 0xE333, sound = 0xE050, cheats = 0xE65F,
              controls = 0xF135, info = 0xE88E, done = 0xE86C, play = 0xE037, home = 0xE88A, quit = 0xE9BA,
              star = 0xE838, heart = 0xE87D, bolt = 0xEA0B, add = 0xE145, clock = 0xE192, eye = 0xE417,
              shield = 0xE9E0, infinite = 0xEB3D, flight = 0xE539, skip = 0xE044, music = 0xE3A1,
              keyboard = 0xE312, wave = 0xE1B8, monitor = 0xEF5B, joystick = 0xF5EE, folder = 0xE2C8;
}

bool init(SDL_Renderer *ren);
void begin(int out_w, int out_h);   // once per frame, with the output size in pixels
float scale();                      // pixels per page unit
float page_width();

void fill_window(Color c);
void rect(float x, float y, float w, float h, float radius, Color c);
void rect_v(float x, float y, float w, float h, float radius, Color top, Color bottom);   // top to bottom
void ring(float x, float y, float w, float h, float radius, float thickness, Color c);
void glow(float x, float y, float w, float h, float radius, float spread, Color c);
void spot(float cx, float cy, float radius, Color c);   // a soft round light
void dots(float x, float y, float w, float h, float pitch, float size, Color c);
void line(float x, float y, float w, Color c);   // a 1-pixel hairline
void segment(float x0, float y0, float x1, float y1, float thickness, Color c);

// y is the middle of the capitals, so text centres on a row by its middle.
float text(Font f, float size, float x, float y, const char *s, Color c, Align a = left, float tracking = 0);
float text_width(Font f, float size, const char *s, float tracking = 0);
void icon(int codepoint, float size, float cx, float cy, Color c);   // centred on (cx, cy)

void image(SDL_Texture *t, float x, float y, float w, float h, float alpha = 1);

// What the renderer has drawn so far this frame, as a PNG; a 4K screen's at
// half size. Written on its own thread: a console's storage busy with other
// transfers once took a minute over one, and the game waited.
void save_screen(SDL_Renderer *ren, const std::string &path);
// Development, on the PlayStation 5: true once when a file named devloop-shot
// has appeared in the title's folder (FTP one there to ask for a screenshot).
bool console_shot_asked();
void image_rounded(SDL_Texture *t, float x, float y, float w, float h, float radius, float alpha = 1);

} // namespace ui
