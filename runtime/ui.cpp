#include "ui.h"

#include "png.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// The fonts, built into the program (res/fonts, embedded by CMake).
extern const unsigned char font_inter_regular[];
extern const unsigned char font_inter_semibold[];
extern const unsigned char font_icons[];

namespace ui {

namespace {

SDL_Renderer *ren;
float s = 1, ox = 0, oy = 0, page_w = 1920;
int out_w, out_h;
SDL_Texture *spot_tex;

// What each font is baked with: ASCII and a few more for text, the icons
// for the icon font.
constexpr int kExtra[] = {0x00B7, 0x00D7, 0x2013, 0x2014, 0x2019, 0x201C, 0x201D, 0x2026,
                          0x2039, 0x203A, 0x2190, 0x2191, 0x2192, 0x2193};
const std::vector<int> &codepoints(int font)
{
    static std::vector<int> txt, ico;
    if (txt.empty()) {
        for (int c = 32; c < 127; c++)
            txt.push_back(c);
        txt.insert(txt.end(), std::begin(kExtra), std::end(kExtra));
        using namespace icon_id;
        ico = {game, save, load, display, sound, cheats, controls, info, done, play, home, quit, star, heart,
               bolt, add, clock, eye, shield, infinite, flight, skip, music, keyboard, wave, monitor, joystick,
               folder};
    }
    return font == icons ? ico : txt;
}

struct FontData {
    stbtt_fontinfo info;
    const unsigned char *data;
    float cap;   // capital height per em
};
FontData fonts[3];

// One baked size of one font.
struct Atlas {
    int font, px;
    SDL_Texture *tex;
    int w, h;
    std::vector<stbtt_packedchar> chars;   // in codepoints(font) order
    float scale;                           // font units -> pixels
    int last_used;
};
std::vector<Atlas> atlases;
int frame_no;

float X(float x) { return ox + x * s; }
float Y(float y) { return oy + y * s; }

SDL_FColor fc(Color c) { return {c.r, c.g, c.b, c.a}; }

Atlas *atlas_for(int f, int px)
{
    for (Atlas &a : atlases)
        if (a.font == f && a.px == px) {
            a.last_used = frame_no;
            return &a;
        }
    // Sizes change with the window: drop ones not used for a while.
    atlases.erase(std::remove_if(atlases.begin(), atlases.end(),
                                 [](Atlas &a) {
                                     if (frame_no - a.last_used < 120)
                                         return false;
                                     SDL_DestroyTexture(a.tex);
                                     return true;
                                 }),
                  atlases.end());
    Atlas a{};
    a.font = f;
    a.px = px;
    a.last_used = frame_no;
    const std::vector<int> &cps = codepoints(f);
    a.chars.resize(cps.size());
    int over = px < 40 && f != icons ? 2 : 1;
    a.w = 512;
    a.h = 64;
    while (true) {
        std::vector<unsigned char> mono(size_t(a.w) * a.h);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, mono.data(), a.w, a.h, 0, 1, nullptr);
        stbtt_PackSetOversampling(&pc, over, 1);
        stbtt_pack_range range{};
        range.font_size = STBTT_POINT_SIZE(float(px));
        range.array_of_unicode_codepoints = const_cast<int *>(cps.data());
        range.num_chars = int(cps.size());
        range.chardata_for_range = a.chars.data();
        bool ok = stbtt_PackFontRanges(&pc, fonts[f].data, 0, &range, 1) != 0;
        stbtt_PackEnd(&pc);
        if (!ok && a.h < 4096) {
            a.h *= 2;
            continue;
        }
        std::vector<uint32_t> rgba(mono.size());
        for (size_t i = 0; i < mono.size(); i++)
            rgba[i] = 0x00FFFFFFu | uint32_t(mono[i]) << 24;
        a.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, a.w, a.h);
        SDL_UpdateTexture(a.tex, nullptr, rgba.data(), a.w * 4);
        SDL_SetTextureBlendMode(a.tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(a.tex, SDL_SCALEMODE_LINEAR);
        break;
    }
    a.scale = stbtt_ScaleForMappingEmToPixels(&fonts[f].info, float(px));
    atlases.push_back(std::move(a));
    return &atlases.back();
}

int glyph_index(int f, int cp)
{
    if (f != icons && cp >= 32 && cp < 127)
        return cp - 32;
    const std::vector<int> &cps = codepoints(f);
    for (size_t i = 0; i < cps.size(); i++)
        if (cps[i] == cp)
            return int(i);
    return f == icons ? -1 : '?' - 32;
}

int next_codepoint(const char *&p)
{
    unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x80)
        return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    int cp = c & (0x3F >> n);
    for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++)
        cp = cp << 6 | (*p++ & 0x3F);
    return cp;
}

void quad(std::vector<SDL_Vertex> &v, std::vector<int> &idx, const stbtt_aligned_quad &q, Color c, float dx = 0,
          float dy = 0)
{
    int b = int(v.size());
    SDL_FColor col = fc(c);
    v.push_back({{q.x0 + dx, q.y0 + dy}, col, {q.s0, q.t0}});
    v.push_back({{q.x1 + dx, q.y0 + dy}, col, {q.s1, q.t0}});
    v.push_back({{q.x1 + dx, q.y1 + dy}, col, {q.s1, q.t1}});
    v.push_back({{q.x0 + dx, q.y1 + dy}, col, {q.s0, q.t1}});
    for (int k : {0, 1, 2, 0, 2, 3})
        idx.push_back(b + k);
}

// Text runs through the same loop for measuring and drawing.
float lay_out(Font f, float size, float x, float y, const char *str, Color c, float tracking, bool draw)
{
    int px = std::max(1, int(std::lround(size * s)));
    Atlas *a = atlas_for(f, px);
    float pen = X(x), base = std::round(Y(y) + fonts[f].cap * float(px) * 0.5f);
    float start = pen;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    int prev = 0;
    for (const char *p = str; *p;) {
        int cp = next_codepoint(p);
        if (prev)
            pen += float(stbtt_GetCodepointKernAdvance(&fonts[f].info, prev, cp)) * a->scale;
        prev = cp;
        int gi = glyph_index(f, cp);
        if (gi < 0)
            continue;
        stbtt_aligned_quad q;
        float gx = pen, gy = base;
        stbtt_GetPackedQuad(a->chars.data(), a->w, a->h, gi, &gx, &gy, &q, 0);
        if (draw && q.x1 > q.x0)
            quad(v, idx, q, c);
        pen = gx + tracking * s;
    }
    if (draw && !v.empty())
        SDL_RenderGeometry(ren, a->tex, v.data(), int(v.size()), idx.data(), int(idx.size()));
    return (pen - start - (prev ? tracking * s : 0)) / s;
}

// The outline of a rounded rectangle, in pixels, grown by `grow` pixels.
void outline(float x, float y, float w, float h, float r, float grow, std::vector<SDL_FPoint> &out)
{
    out.clear();
    float px0 = X(x) - grow, py0 = Y(y) - grow, px1 = X(x + w) + grow, py1 = Y(y + h) + grow;
    float rr = std::max(0.0f, std::min(r * s + grow, std::min(px1 - px0, py1 - py0) * 0.5f));
    int n = std::clamp(int(r * s / 3) + 3, 3, 14);
    const float cx[4] = {px1 - rr, px1 - rr, px0 + rr, px0 + rr};
    const float cy[4] = {py0 + rr, py1 - rr, py1 - rr, py0 + rr};
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= n; i++) {
            float t = (float(k) - 1.0f + float(i) / float(n)) * 1.5707963f;
            out.push_back({cx[k] + std::cos(t) * rr, cy[k] + std::sin(t) * rr});
        }
}

// A band between two outlines of the same shape, coloured ca inside and cb outside.
void band(const std::vector<SDL_FPoint> &in, const std::vector<SDL_FPoint> &out, Color ca, Color cb,
          std::vector<SDL_Vertex> &v, std::vector<int> &idx)
{
    int n = int(in.size()), b = int(v.size());
    for (int i = 0; i < n; i++) {
        v.push_back({in[i], fc(ca), {0, 0}});
        v.push_back({out[i], fc(cb), {0, 0}});
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int a0 = b + i * 2, a1 = a0 + 1, b0 = b + j * 2, b1 = b0 + 1;
        for (int k : {a0, a1, b1, a0, b1, b0})
            idx.push_back(k);
    }
}

void draw(const std::vector<SDL_Vertex> &v, const std::vector<int> &idx)
{
    if (!idx.empty())
        SDL_RenderGeometry(ren, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

} // namespace

bool init(SDL_Renderer *r)
{
    ren = r;
    const unsigned char *data[3] = {font_inter_regular, font_inter_semibold, font_icons};
    for (int f = 0; f < 3; f++) {
        fonts[f].data = data[f];
        if (!stbtt_InitFont(&fonts[f].info, data[f], 0))
            return false;
        int x0, y0, x1, y1;
        if (stbtt_GetCodepointBox(&fonts[f].info, 'H', &x0, &y0, &x1, &y1))
            fonts[f].cap = float(y1) * stbtt_ScaleForMappingEmToPixels(&fonts[f].info, 1.0f);
    }
    // A soft round light, drawn tinted for glows behind things.
    constexpr int n = 128;
    std::vector<uint32_t> px(size_t(n) * n);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float dx = (float(x) + 0.5f) / n * 2 - 1, dy = (float(y) + 0.5f) / n * 2 - 1;
            float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
            float a = (1 - d) * (1 - d);
            px[size_t(y) * n + x] = 0x00FFFFFFu | uint32_t(a * 255) << 24;
        }
    spot_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, n, n);
    SDL_UpdateTexture(spot_tex, nullptr, px.data(), n * 4);
    SDL_SetTextureBlendMode(spot_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(spot_tex, SDL_SCALEMODE_LINEAR);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    return true;
}

void begin(int w, int h)
{
    out_w = w;
    out_h = h;
    s = float(h) / 1080.0f;
    if (float(w) / s < 1500.0f)
        s = float(w) / 1500.0f;
    page_w = float(w) / s;
    ox = 0;
    oy = (float(h) - 1080.0f * s) * 0.5f;
    frame_no++;
}

float scale() { return s; }
float page_width() { return page_w; }

void fill_window(Color c)
{
    SDL_SetRenderDrawColorFloat(ren, c.r, c.g, c.b, c.a);
    SDL_FRect all{0, 0, float(out_w), float(out_h)};
    SDL_RenderFillRect(ren, &all);
}

void rect_v(float x, float y, float w, float h, float r, Color top, Color bottom)
{
    std::vector<SDL_FPoint> in, out;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    outline(x, y, w, h, r, -0.5f, in);
    outline(x, y, w, h, r, 0.5f, out);
    float y0 = Y(y), hh = std::max(1.0f, h * s);
    auto at = [&](float py) { return mix(top, bottom, std::clamp((py - y0) / hh, 0.0f, 1.0f)); };
    // The inside as a fan, then a one-pixel soft edge.
    float mx = X(x + w * 0.5f), my = Y(y + h * 0.5f);
    v.push_back({{mx, my}, fc(at(my)), {0, 0}});
    for (const SDL_FPoint &p : in)
        v.push_back({p, fc(at(p.y)), {0, 0}});
    int n = int(in.size());
    for (int i = 0; i < n; i++)
        for (int k : {0, 1 + i, 1 + (i + 1) % n})
            idx.push_back(k);
    int b = int(v.size());
    for (int i = 0; i < n; i++) {
        Color c = at(in[i].y);
        v.push_back({in[i], fc(c), {0, 0}});
        v.push_back({out[i], fc(alpha(c, 0)), {0, 0}});
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int a0 = b + i * 2, a1 = a0 + 1, b0 = b + j * 2, b1 = b0 + 1;
        for (int k : {a0, a1, b1, a0, b1, b0})
            idx.push_back(k);
    }
    draw(v, idx);
}

void rect(float x, float y, float w, float h, float r, Color c) { rect_v(x, y, w, h, r, c, c); }

void ring(float x, float y, float w, float h, float r, float t, Color c)
{
    float tp = std::max(1.0f, t * s);
    std::vector<SDL_FPoint> a, b, cc, d;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    outline(x, y, w, h, r, 0.5f, a);
    outline(x, y, w, h, r, -0.5f, b);
    outline(x, y, w, h, r, -tp + 0.5f, cc);
    outline(x, y, w, h, r, -tp - 0.5f, d);
    band(b, a, c, alpha(c, 0), v, idx);
    band(cc, b, c, c, v, idx);
    band(cc, d, c, alpha(c, 0), v, idx);
    draw(v, idx);
}

void glow(float x, float y, float w, float h, float r, float spread, Color c)
{
    float g = spread * s;
    std::vector<SDL_FPoint> a, b, cc;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    outline(x, y, w, h, r, 0, a);
    outline(x, y, w, h, r, g * 0.35f, b);
    outline(x, y, w, h, r, g, cc);
    band(a, b, c, alpha(c, 0.35f), v, idx);
    band(b, cc, alpha(c, 0.35f), alpha(c, 0), v, idx);
    draw(v, idx);
}

void spot(float cx, float cy, float radius, Color c)
{
    SDL_SetTextureColorModFloat(spot_tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(spot_tex, c.a);
    SDL_FRect d{X(cx - radius), Y(cy - radius), radius * 2 * s, radius * 2 * s};
    SDL_RenderTexture(ren, spot_tex, nullptr, &d);
}

void dots(float x, float y, float w, float h, float pitch, float size, Color c)
{
    std::vector<SDL_FRect> r;
    float d = std::max(1.0f, std::round(size * s));
    for (float yy = y + pitch * 0.5f; yy < y + h; yy += pitch)
        for (float xx = x + pitch * 0.5f; xx < x + w; xx += pitch)
            r.push_back({std::round(X(xx)), std::round(Y(yy)), d, d});
    SDL_SetRenderDrawColorFloat(ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRects(ren, r.data(), int(r.size()));
}

void line(float x, float y, float w, Color c)
{
    SDL_SetRenderDrawColorFloat(ren, c.r, c.g, c.b, c.a);
    SDL_FRect r{std::round(X(x)), std::round(Y(y)), std::round(w * s), 1};
    SDL_RenderFillRect(ren, &r);
}

void segment(float x0, float y0, float x1, float y1, float t, Color c)
{
    // A quad along the line with a soft pixel on each side.
    float ax = X(x0), ay = Y(y0), bx = X(x1), by = Y(y1);
    float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0)
        return;
    float nx = -dy / len, ny = dx / len, hw = std::max(0.5f, t * s * 0.5f);
    SDL_FColor full = fc(c), none = fc(alpha(c, 0));
    std::vector<SDL_Vertex> v;
    for (float off : {-hw - 0.5f, -hw + 0.5f, hw - 0.5f, hw + 0.5f}) {
        bool edge = off < -hw || off > hw;
        v.push_back({{ax + nx * off, ay + ny * off}, edge ? none : full, {0, 0}});
        v.push_back({{bx + nx * off, by + ny * off}, edge ? none : full, {0, 0}});
    }
    std::vector<int> idx;
    for (int k = 0; k < 3; k++) {
        int a0 = k * 2, a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
        for (int i : {a0, a1, b1, a0, b1, b0})
            idx.push_back(i);
    }
    draw(v, idx);
}

float text(Font f, float size, float x, float y, const char *str, Color c, Align al, float tracking)
{
    float w = al == left ? 0 : lay_out(f, size, 0, 0, str, c, tracking, false);
    float at = al == left ? x : al == center ? x - w * 0.5f : x - w;
    return lay_out(f, size, at, y, str, c, tracking, true);
}

float text_width(Font f, float size, const char *str, float tracking)
{
    return lay_out(f, size, 0, 0, str, {}, tracking, false);
}

void icon(int cp, float size, float cx, float cy, Color c)
{
    int px = std::max(1, int(std::lround(size * s)));
    Atlas *a = atlas_for(icons, px);
    int gi = glyph_index(icons, cp);
    if (gi < 0)
        return;
    stbtt_aligned_quad q;
    float gx = 0, gy = 0;
    stbtt_GetPackedQuad(a->chars.data(), a->w, a->h, gi, &gx, &gy, &q, 1);
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    quad(v, idx, q, c, std::round(X(cx) - (q.x0 + q.x1) * 0.5f), std::round(Y(cy) - (q.y0 + q.y1) * 0.5f));
    SDL_RenderGeometry(ren, a->tex, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

// A picture cut to a rounded rectangle: the inside as a fan, then a
// one-pixel band fading out at the edge so the corners aren't jagged.
void image_rounded(SDL_Texture *t, float x, float y, float w, float h, float r, float a)
{
    std::vector<SDL_FPoint> in, out;
    outline(x, y, w, h, r, -0.5f, in);
    outline(x, y, w, h, r, 0.5f, out);
    float px0 = X(x), py0 = Y(y), pw = w * s, ph = h * s;
    auto uv = [&](SDL_FPoint p) {
        return SDL_FPoint{std::clamp((p.x - px0) / pw, 0.0f, 1.0f), std::clamp((p.y - py0) / ph, 0.0f, 1.0f)};
    };
    SDL_FColor full{1, 1, 1, a}, none{1, 1, 1, 0};
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    SDL_FPoint mid{X(x + w * 0.5f), Y(y + h * 0.5f)};
    v.push_back({mid, full, uv(mid)});
    for (const SDL_FPoint &p : in)
        v.push_back({p, full, uv(p)});
    int n = int(in.size());
    for (int i = 0; i < n; i++)
        for (int k : {0, 1 + i, 1 + (i + 1) % n})
            idx.push_back(k);
    int b = int(v.size());
    for (int i = 0; i < n; i++) {
        v.push_back({in[i], full, uv(in[i])});
        v.push_back({out[i], none, uv(out[i])});
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int a0 = b + i * 2, a1 = a0 + 1, b0 = b + j * 2, b1 = b0 + 1;
        for (int k : {a0, a1, b1, a0, b1, b0})
            idx.push_back(k);
    }
    SDL_RenderGeometry(ren, t, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

void image(SDL_Texture *t, float x, float y, float w, float h, float a)
{
    SDL_SetTextureAlphaModFloat(t, a);
    SDL_FRect d{X(x), Y(y), w * s, h * s};
    SDL_RenderTexture(ren, t, nullptr, &d);
    SDL_SetTextureAlphaModFloat(t, 1);
}

void save_screen(SDL_Renderer *ren, const std::string &path)
{
    SDL_Surface *got = SDL_RenderReadPixels(ren, nullptr);
    if (!got) {
        std::fprintf(stderr, "shot %s: %s\n", path.c_str(), SDL_GetError());
        return;
    }
    if (got->w > 1920)
        if (SDL_Surface *half = SDL_ScaleSurface(got, got->w / 2, got->h / 2, SDL_SCALEMODE_LINEAR)) {
            SDL_DestroySurface(got);
            got = half;
        }
    if (SDL_Surface *argb = SDL_ConvertSurface(got, SDL_PIXELFORMAT_ARGB8888)) {
        std::vector<uint32_t> px(size_t(argb->w) * size_t(argb->h));
        for (int y = 0; y < argb->h; y++)
            std::memcpy(&px[size_t(y) * size_t(argb->w)],
                        static_cast<uint8_t *>(argb->pixels) + size_t(y) * size_t(argb->pitch), size_t(argb->w) * 4);
        std::thread([path, px = std::move(px), w = argb->w, h = argb->h] {
            write_png(path.c_str(), px.data(), w, h, w);
        }).detach();
        SDL_DestroySurface(argb);
    }
    SDL_DestroySurface(got);
}

bool console_shot_asked()
{
#ifdef __PROSPERO__
    // (SDL_RemovePath says yes to a file that isn't there, so look first.)
    static uint64_t checked;
    uint64_t now = SDL_GetTicksNS();
    if (now - checked < 500000000ull)
        return false;
    checked = now;
    return SDL_GetPathInfo("/app0/devloop-shot", nullptr) && SDL_RemovePath("/app0/devloop-shot");
#else
    return false;
#endif
}

} // namespace ui
