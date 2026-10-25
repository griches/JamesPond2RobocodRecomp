#include "wide.h"

#include "amiga.h"
#include "state.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace wide {

using namespace amiga;

namespace {

// Game state, found by tracing the scroll and blit code.
constexpr uint32_t kCamX = 0xAB0, kCamY = 0xAB2;   // camera, pixels
constexpr uint32_t kMapPtr = 0x22D8;               // level map: words, low 9 bits = tile, bit 9 = flip
constexpr uint32_t kMapW = 0xAE2;                  // map width in tiles
constexpr uint32_t kTiles = 0x3F8F4;               // 16x16 tiles, 4 planes, 128 bytes each
constexpr uint32_t kParallax = 0x56980;            // parallax image: 512 x 256, one plane
constexpr uint32_t kParallaxBytes = 64 * 256;
constexpr uint32_t kObjX = 0x1426, kObjY = 0x1428;  // position handed to the draw-record routine
constexpr uint32_t kObjFlags = 0x1438;              // byte, bit 3: mirrored
constexpr uint32_t kObjMirrorX = 0x143E;
constexpr uint32_t kObjFlash = 0x1424;              // >0 solid, <0 cut out
constexpr uint32_t kObjLayer = 0x143A;              // the record's layer tag
constexpr uint32_t kNoSprites = 0x5F2;              // set: no objects become sprites

// One object the game asked to draw, in world pixels.
struct Obj {
    uint32_t gfx, mask;   // 4 planes of w words x h rows each; mask is one such plane, 0 = solid
    int x, y, w, h;
    int flash;
    int layer;            // drawn in passes 4, 3, 2, 1, 0 (0 on top); others never
    bool column;          // one 16-pixel row (planes side by side) repeated down: Robocod's stretch
};

struct Params {
    bool valid;
    int camx, camy;
    uint32_t map;
    int map_w;
    bool par;
    uint32_t par_a, par_d;
    int par_shift;
    bool no_sprites;
    std::vector<Obj> objs;
};
// The game builds each picture ahead of showing it, so keep a little history:
// hist[0] was gathered during the frame just emulated, hist[k] k frames before.
constexpr int kHist = 4;
Params building, hist[kHist];
int cam_lag = 2, par_lag = 2;   // the picture on screen was built two frames ago
int obj_lag = 2;
const Params &cam_src() { return hist[cam_lag]; }
// The parallax layer may be double-buffered (two bitplane-5 buffers used in
// turn), so take the blit that went into the buffer on screen: from two
// frames back if it did, else three, else one. None: the buffer on screen
// wasn't blitted lately (see Found).
const Params *blit_for_shown()
{
    uint32_t shown = playfield_start.bpl[4];
    for (int k : {par_lag, par_lag + 1, par_lag - 1}) {
        const Params &p = hist[k];
        if (p.par && (p.par_d == shown || p.par_d + 2 == shown))
            return &p;
    }
    return nullptr;
}
const Params &obj_src() { return hist[obj_lag]; }

// Lines up the redrawn tiles with the game's own (world = camera + window
// position + offset). Found with JP2_WIDECHECK.
int tile_dx = 0, tile_dy = 0;
int obj_dx = 0, obj_dy = 0;   // the same for objects: window = world - camera + offset

uint32_t rd32(uint32_t a) { return uint32_t(chip16(a)) << 16 | chip16(a + 2); }

int tile_index(const Params &p, int x, int y)
{
    if (x < 0 || y < 0 || x >= p.map_w * 16)
        return 6;   // what the game fills an empty tile with
    uint16_t t = chip16(p.map + uint32_t(((y >> 4) * p.map_w + (x >> 4)) * 2));
    uint32_t n = t & 0x1FF;
    if (!n)
        return 6;
    int r = (t & 0x200) ? 15 - (y & 15) : (y & 15);
    uint32_t base = kTiles + n * 128 + uint32_t(r) * 2;
    int sh = 15 - (x & 15), v = 0;
    for (int pl = 0; pl < 4; pl++)
        v |= ((chip16(base + uint32_t(pl) * 32) >> sh) & 1) << pl;
    return v;
}

int parallax_bit(int x, int y)
{
    x &= 511;
    y &= 255;
    return (chip16(kParallax + uint32_t(y) * 64 + uint32_t(x >> 4) * 2) >> (15 - (x & 15))) & 1;
}

struct Geometry {
    int h0, v0, w;       // window in display coordinates
    int par_x0, par_y0;  // parallax image position at window (0, 0)
    bool par_known;      // false: draw no parallax layer
};

// Where the parallax image sits when no blit says so: a level or room that
// never redraws the layer (it may still flip between two buffers), or a save
// loaded straight into one. Found by matching the picture's bitplane 5
// against the image, one entry per bitplane-5 buffer, and re-checked against
// a few pixels every frame. `base_x` is the position with no scroll delay.
struct Found {
    uint32_t bpl5;
    bool valid;
    int base_x, y0;
    int retry_frame;   // a failed search may run again from this frame
};
Found found[4];
int found_next, frames_seen;

void forget_found()
{
    for (Found &f : found)
        f = Found{};
}

int shown_bit5(int h0, int v0, int x, int y)
{
    return (index_buffer[(v0 + y - kFbY0) * kFbW + (h0 + x - kFbX0)] >> 4) & 1;
}

bool layer_matches(int h0, int v0, int w, int x0, int y0)
{
    int bad = 0, n = 0;
    for (int y = 3; y < kHeight; y += 23)
        for (int x = 5; x < w; x += 37, n++)
            bad += parallax_bit(x0 + x, y0 + y) != shown_bit5(h0, v0, x, y);
    return bad * 10 <= n;
}

bool recover_parallax(int h0, int v0, int w, int delay, Found *f)
{
    struct Pt {
        int x, y, bit;
    };
    std::vector<Pt> pts;
    int set = 0;
    for (int y = 0; y < kHeight; y += 5)
        for (int x = 0; x < w; x += 7) {
            int b = shown_bit5(h0, v0, x, y);
            pts.push_back({x, y, b});
            set += b;
        }
    int n = int(pts.size());
    if (set < n / 20 || set > n * 19 / 20)
        return false;   // too empty or too full to place reliably
    for (int py = 0; py < 256; py++)
        for (int px = 0; px < 512; px++) {
            int bad = 0, i = 0;
            for (; i < n; i++)
                if (parallax_bit(px + pts[i].x, py + pts[i].y) != pts[i].bit && ++bad > n / 50)
                    break;
            if (i == n) {
                f->base_x = px + delay;
                f->y0 = py;
                return true;
            }
        }
    return false;
}

Found &found_for(uint32_t bpl5)
{
    for (Found &f : found)
        if (f.bpl5 == bpl5)
            return f;
    Found &f = found[found_next++ % 4];
    f = Found{};
    f.bpl5 = bpl5;
    return f;
}

bool geometry(Geometry *g)
{
    const PlayfieldStart &ps = playfield_start;
    if (!cam_src().valid || ps.line < 0)
        return false;
    if ((ps.bplcon0 & 0x7000) != 0x5000 || (ps.bplcon0 & 0x8C00))   // 5 planes, lowres, no HAM/DPF
        return false;
    g->h0 = ps.diwstrt & 0xFF;
    g->v0 = ps.diwstrt >> 8;
    g->w = ((ps.diwstop & 0xFF) | 0x100) - g->h0;
    if (g->w <= 0 || g->w > 320)
        return false;
    int first = 2 * (ps.ddfstrt & 0xFC) + 17;          // display column of the first fetched pixel
    int delay = ps.bplcon1 & 15;                       // plane 5 is odd: playfield 1 scroll
    const Params *pp = blit_for_shown();
    if (!pp) {
        Found &f = found_for(ps.bpl[4]);
        if (f.valid && !layer_matches(g->h0, g->v0, g->w, f.base_x - delay, f.y0))
            f.valid = false;
        if (!f.valid && frames_seen >= f.retry_frame) {
            f.valid = recover_parallax(g->h0, g->v0, g->w, delay, &f);
            if (!f.valid)
                f.retry_frame = frames_seen + 25;
        }
        // Not found: draw the margins without the layer rather than drop widescreen.
        g->par_x0 = f.valid ? f.base_x - delay : 0;
        g->par_y0 = f.valid ? f.y0 : 0;
        g->par_known = f.valid;
        return true;
    }
    g->par_known = true;
    uint32_t off = pp->par_a - kParallax;
    int lead = int(int32_t(ps.bpl[4] - pp->par_d)) * 8;
    g->par_x0 = int(off % 64) * 8 + lead - pp->par_shift + (g->h0 - first) - delay;
    g->par_y0 = int(off / 64);
    return true;
}

// Draws an object's 4-bit colours into a buffer of colour indices, keeping
// the parallax bit (plane 5) underneath, like the game's cookie-cut blit.
// Only columns where `allowed(x)` is true are touched.
// as_sprite: the game shows this object as a hardware sprite. Sprite colour 0
// is see-through, sprites use colours 16-31, and with the game's priority
// (BPLCON2 = 0) they only show where the playfield is colour 0.
template <class Allowed>
void draw_obj(const Obj &o, int wx, int wy, uint8_t *idx, int width, int height, int ox, Allowed allowed,
              bool as_sprite = false, bool behind = true, int rows = -1)
{
    uint32_t plane = o.column ? 2 : uint32_t(o.w) * 2 * uint32_t(o.h);
    if (rows < 0 || !o.column)
        rows = o.h;
    for (int r = 0; r < rows; r++) {
        int y = wy + r;
        if (y < 0 || y >= height)
            continue;
        for (int c = 0; c < o.w * 16; c++) {
            int x = wx + c + ox;
            if (x < 0 || x >= width || !allowed(x))
                continue;
            uint32_t word = o.column ? 0 : (uint32_t(r) * o.w + uint32_t(c >> 4)) * 2;
            int bit = 15 - (c & 15);
            if (o.mask && !((chip16(o.mask + word) >> bit) & 1))
                continue;
            int v;
            if (o.flash > 0)
                v = 15;
            else if (o.flash < 0)
                v = 0;
            else {
                v = 0;
                for (int pl = 0; pl < 4; pl++)
                    v |= ((chip16(o.gfx + uint32_t(pl) * plane + word) >> bit) & 1) << pl;
            }
            uint8_t &d = idx[size_t(y) * width + x];
            if (as_sprite) {
                if (v == 0 || (behind && (d & 31)))
                    continue;
                d = uint8_t(16 + v);
            } else {
                d = uint8_t((d & 16) | v);
            }
        }
    }
}

// Which objects the game shows as hardware sprites rather than drawing into
// the screen (the loop at $A934): unmasked-solid items never; otherwise a draw
// record no taller than 32 lines, not cut at the buffer's edge, starting in
// the band $14-$46 or $68-$90, while that band has words left of its 4 (in
// record order). Objects out in the widescreen margins have no record; they
// follow the same shape rule without using the budget.
std::vector<uint8_t> sprite_objects(const std::vector<Obj> &objs, int camx, int camy, bool none)
{
    std::vector<uint8_t> out(objs.size(), 0);
    if (none)
        return out;
    int c1 = 0, c2 = 0;
    bool stop = false;
    int left = camx & ~15;
    // Only layers 0, 1 and 2 can become sprites, considered in that order.
    std::vector<size_t> order;
    for (int layer = 0; layer <= 2; layer++)
        for (size_t i = 0; i < objs.size(); i++)
            if (objs[i].layer == layer)
                order.push_back(i);
    for (size_t i : order) {
        const Obj &o = objs[i];
        if (o.mask == 0)
            continue;
        int bx = o.x - left, y = o.y - camy, w = o.w * 16;
        int top = y < 0 ? 0 : y, h = y < 0 ? o.h + y : o.h;
        bool band1 = top >= 0x14 && top <= 0x46, band2 = top >= 0x68 && top <= 0x90;
        bool has_record = bx < 0x130 && bx + w > 0 && y < 0xD0 && y + o.h > 0;
        if (!has_record) {
            out[i] = h <= 32 && (band1 || band2);
            continue;
        }
        if (stop || bx < 0 || bx + w > 0x130 || h > 32)
            continue;
        if (c1 < 4) {
            if (top < 0x14)
                continue;
            if (top <= 0x46) {
                if (c1 + o.w <= 4) {
                    out[i] = 1;
                    c1 += o.w;
                }
                continue;
            }
        }
        if (c2 >= 4) {
            stop = true;
            continue;
        }
        if (!band2)
            continue;
        if (c2 + o.w <= 4) {
            out[i] = 1;
            c2 += o.w;
        }
    }
    return out;
}

// Sprite bands holding the HUD, by display line: +1 score (top, kept to the
// right edge), -1 lives (bottom, kept to the left edge), 0 game objects.
int hud_band(int line)
{
    if (line < 0x40)
        return 1;
    if (line >= 0xD8)
        return -1;
    return 0;
}

// Everything needed to draw one displayed frame from game state.
struct Shown {
    bool valid;
    Geometry g;
    int camx, camy;
    Params cam;           // map and camera
    std::vector<Obj> objs;
    bool no_sprites;
};
Shown shown_prev, shown_cur;

inline uint32_t rgb(uint16_t c)
{
    uint32_t r = (c >> 8) & 15, gg = (c >> 4) & 15, b = c & 15;
    return 0xFF000000u | (r * 17) << 16 | (gg * 17) << 8 | (b * 17);
}

} // namespace

void blit_seen()
{
    const uint16_t *r = regs;
    uint16_t con0 = r[0x040 >> 1], size = r[0x058 >> 1];
    if ((con0 & 0x0FFF) != 0x09F0 || (size & 63) != 19 || building.par)
        return;
    uint32_t a = uint32_t(r[0x050 >> 1]) << 16 | r[0x052 >> 1];
    if (a < kParallax || a >= kParallax + kParallaxBytes)
        return;
    // The first of the frame's parallax blits gives the image's position;
    // a second one only wraps the bottom of the image round to the top.
    building.par = true;
    forget_found();
    building.par_a = a;
    building.par_d = uint32_t(r[0x054 >> 1]) << 16 | r[0x056 >> 1];
    building.par_shift = con0 >> 12;
}

void object_seen(uint32_t a2, uint32_t a1)
{
    a2 &= 0xFFFFFF;
    if (a2 + 0x14 >= kRamSize || building.objs.size() >= 64)
        return;
    Obj o{};
    o.x = int16_t(chip16(kObjX));
    o.y = int16_t(chip16(kObjY));
    // A negative width means a column: one 16-pixel row, its four planes side
    // by side, repeated for the height (the game writes that height as it goes).
    int wbyte = int8_t(ram[a2 + 0x12]);
    o.column = wbyte < 0;
    o.w = o.column ? 1 : wbyte;
    o.h = ram[a2 + 0x13];
    int xoff = int8_t(ram[a2 + 0x10]), yoff = int8_t(ram[a2 + 0x11]);
    if (o.w <= 0 || o.w > 8 || o.h <= 0)
        return;
    // btst #3,$1438.w: a byte test, so bit 3 of the byte at $1438
    bool mirrored = (ram[kObjFlags] & 8) && int32_t(rd32(a2 + 8)) >= 0;
    if (mirrored) {
        o.gfx = rd32(a2 + 8);
        o.mask = rd32(a2 + 12);
        if (xoff)
            o.x = o.x + int16_t(chip16(kObjMirrorX)) - wbyte * 16 - xoff;
    } else {
        o.gfx = rd32(a2);
        o.mask = rd32(a2 + 4);
        o.x += xoff;
    }
    if (chip16(a2) == 0) {   // pointers relative to a base the caller passes in a1
        uint32_t base = rd32(a1 & 0xFFFFFF);
        o.gfx += base;
        o.mask += base;
    }
    // A negative mask pointer means "no mask": the whole rectangle is drawn
    // (items). The game tests the pointer's high word.
    bool solid = int16_t(o.mask >> 16) < 0;
    o.gfx &= 0xFFFFFF;
    o.mask = solid ? 0 : (o.mask & 0xFFFFFF);
    o.y += yoff;
    o.flash = int16_t(chip16(kObjFlash));
    o.layer = int16_t(chip16(kObjLayer));
    building.objs.push_back(o);
    if (std::getenv("JP2_OBJLOG"))
        std::fprintf(stderr, "object %06X mask %06X %dx%d at %d,%d layer %d\n", o.gfx, o.mask, o.w * 16, o.h, o.x, o.y,
                     o.layer);
}

// Items: a whole 16x16 image in the tile layout, no mask.
void tile_object_seen(uint32_t image)
{
    if (building.objs.size() >= 64)
        return;
    Obj o{};
    o.gfx = image & 0xFFFFFF;
    o.mask = 0;
    o.w = 1;
    o.h = 16;
    o.x = int16_t(chip16(kObjX));
    o.y = int16_t(chip16(kObjY));
    o.layer = int16_t(chip16(kObjLayer));
    building.objs.push_back(o);
    if (std::getenv("JP2_OBJLOG"))
        std::fprintf(stderr, "item %06X at %d,%d layer %d (camera %d,%d)\n", o.gfx, o.x, o.y, o.layer,
                     int16_t(chip16(kCamX)), int16_t(chip16(kCamY)));
}

bool playing() { return shown_cur.valid; }

void save_state(amiga::StateIO &s)
{
    for (Params *p : {&hist[0], &hist[1], &hist[2], &hist[3], &building}) {
        s.io(p->par);
        s.io(p->par_a);
        s.io(p->par_d);
        s.io(p->par_shift);
    }
    if (s.loading())
        forget_found();
}

static int extra_px;
void set_view_extra(int pixels) { extra_px = pixels; }
int view_extra() { return extra_px; }

void view_extents(int cam_x, int *left, int *right)
{
    int e = extra_px, level_w = chip16(kMapW) * 16;
    *left = *right = e;
    if (cam_x - e < 0) {
        *left = cam_x > 0 ? cam_x : 0;
        *right = 2 * e - *left;
    } else if (level_w > 0 && cam_x + 288 + e > level_w) {
        *right = level_w - (cam_x + 288) > 0 ? level_w - (cam_x + 288) : 0;
        *left = 2 * e - *right;
    }
}

void frame_done()
{
    frames_seen++;
    building.camx = int16_t(chip16(kCamX));
    building.camy = int16_t(chip16(kCamY));
    building.map = rd32(kMapPtr) & 0xFFFFFF;
    building.map_w = chip16(kMapW);
    building.no_sprites = chip16(kNoSprites) != 0;
    building.valid = building.map_w >= 8 && building.map_w <= 1024 && building.map < kRamSize;
    if (!building.par && hist[0].par) {
        // No parallax blit this frame: the layer hasn't moved (rooms never redraw it).
        building.par = true;
        building.par_a = hist[0].par_a;
        building.par_d = hist[0].par_d;
        building.par_shift = hist[0].par_shift;
    }
    for (int k = kHist - 1; k > 0; k--)
        hist[k] = hist[k - 1];
    hist[0] = building;
    building = Params{};

    shown_prev = std::move(shown_cur);
    shown_cur = Shown{};
    Geometry g;
    if (std::getenv("JP2_PARLOG") && geometry(&g))
        std::fprintf(stderr, "par: cam %d bpl5 %06X bplcon1 %04X blit %d -> origin %d,%d  D: %06X %06X %06X %06X\n",
                     cam_src().camx, playfield_start.bpl[4], playfield_start.bplcon1, blit_for_shown() ? 1 : 0, g.par_x0,
                     g.par_y0, hist[0].par_d, hist[1].par_d, hist[2].par_d, hist[3].par_d);
    if (geometry(&g)) {
        shown_cur.valid = true;
        shown_cur.g = g;
        shown_cur.cam = cam_src();
        shown_cur.camx = cam_src().camx;
        shown_cur.camy = cam_src().camy;
        shown_cur.objs = obj_src().objs;
        shown_cur.no_sprites = obj_src().no_sprites;
    }
}

namespace {

// Moves from a towards b by alpha, unless the jump is too big to be motion.
int lerp(int a, int b, float alpha, int limit = 48)
{
    if (std::abs(b - a) > limit)
        return b;
    return int(std::lround(a + (b - a) * alpha));
}

int lerp_wrap(int a, int b, float alpha, int period)
{
    int d = ((b - a) % period + period + period / 2) % period - period / 2;
    if (std::abs(d) > 48)
        return b;
    return int(std::lround(a + d * alpha));
}

// The previous frame's object most likely to be the same one.
const Obj *match(const Obj &o, const std::vector<Obj> &prev)
{
    const Obj *best = nullptr;
    int best_d = 33;
    for (const Obj &p : prev) {
        if (p.column != o.column || (p.mask == 0) != (o.mask == 0))
            continue;
        if (!o.column && (p.w != o.w || p.h != o.h))
            continue;
        // a column changes height as it stretches: match it on its bottom edge
        int d = o.column ? std::abs(p.x - o.x) + std::abs(p.y + p.h - o.y - o.h)
                         : std::abs(p.x - o.x) + std::abs(p.y - o.y);
        if (d < best_d) {
            best_d = d;
            best = &p;
        }
    }
    return best;
}

} // namespace

bool render(uint32_t *out, int width, float alpha, bool full)
{
    if (!shown_cur.valid || width < shown_cur.g.w || width > kMaxWidth)
        return false;
    const Shown &cu = shown_cur;
    const Shown *pv = shown_prev.valid ? &shown_prev : nullptr;
    const Geometry &g = cu.g;
    int extra = (width - g.w) / 2;
    if (!full)
        alpha = 1;
    int camx = pv ? lerp(pv->camx, cu.camx, alpha) : cu.camx;
    int camy = pv ? lerp(pv->camy, cu.camy, alpha) : cu.camy;
    int parx = pv ? lerp_wrap(pv->g.par_x0, g.par_x0, alpha, 512) : g.par_x0;
    int pary = pv ? lerp_wrap(pv->g.par_y0, g.par_y0, alpha, 256) : g.par_y0;
    // Keep the wide view inside the level: at its ends the view stops at the
    // edge and the game's own window slides across instead.
    int level_w = cu.cam.map_w * 16;
    int view_left = camx - extra;
    if (level_w >= width)
        view_left = view_left < 0 ? 0 : view_left > level_w - width ? level_w - width : view_left;
    int shift = view_left - (camx - extra);
    if (shift)
        full = true;   // the game's own picture is no longer where it was

    static uint8_t idx[kMaxWidth * kHeight];
    auto drawn_here = [&](int x) { return full || x < extra || x >= extra + g.w; };
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < width; x++) {
            if (!drawn_here(x))
                continue;
            int wx = x - extra + shift;   // window column
            int par = g.par_known ? parallax_bit(parx + wx, pary + y) : 0;
            idx[size_t(y) * width + x] = uint8_t(tile_index(cu.cam, camx + wx + tile_dx, camy + y + tile_dy) | par << 4);
        }
    // Objects are drawn layer by layer as the game does (4, 3, 2, 1, then 0
    // on top); the ones it shows as sprites come last, behind anything drawn.
    std::vector<uint8_t> as_sprite = sprite_objects(cu.objs, cu.camx, cu.camy, cu.no_sprites);
    bool behind = ((regs[0x104 >> 1] >> 3) & 7) == 0;
    static const int kPasses[] = {4, 3, 2, 1, 0, -1};   // -1: the sprites
    for (int layer : kPasses)
        for (size_t i = 0; i < cu.objs.size(); i++) {
            const Obj &o = cu.objs[i];
            if (layer < 0 ? !as_sprite[i] : (as_sprite[i] || o.layer != layer))
                continue;
            int pass = layer < 0;
            int ox = o.x, oy = o.y, rows = o.h;
            if (pv && full)
                if (const Obj *p = match(o, pv->objs)) {
                    ox = lerp(p->x, o.x, alpha, 32);
                    oy = lerp(p->y, o.y, alpha, 32);
                    if (o.column)   // top and bottom move separately as it stretches
                        rows = lerp(p->y + p->h, o.y + o.h, alpha, 64) - oy;
                }
            draw_obj(o, ox - camx - shift + obj_dx, oy - camy + obj_dy, idx, width, kHeight, extra, drawn_here,
                     pass == 1, behind, rows);
        }
    for (int y = 0; y < kHeight; y++) {
        int line = g.v0 + y - kFbY0;
        if (line < 0 || line >= kFbH)
            continue;
        const uint16_t *pal = line_palette[line];
        const uint32_t *src = &framebuffer[line * kFbW + (g.h0 - kFbX0)];
        const uint8_t *pf = &index_buffer[line * kFbW + (g.h0 - kFbX0)];
        const uint8_t *sp = &sprite_buffer[line * kFbW + (g.h0 - kFbX0)];
        uint32_t *dst = out + size_t(y) * width;
        // The score (top) and lives (bottom) are sprites marked by the display;
        // take them out of the middle and pin them to the wide screen's
        // corners. Other sprites (small objects) stay where they are.
        for (int x = 0; x < width; x++) {
            if (drawn_here(x))
                dst[x] = rgb(pal[idx[size_t(y) * width + x] & 31]);
            else if (sp[x - extra] & 0x80)
                dst[x] = rgb(pal[pf[x - extra] & 31]);   // the scenery behind the HUD
            else
                dst[x] = src[x - extra];
        }
        int shift = (g.v0 + y) < 0x80 ? extra : -extra;
        for (int wx = 0; wx < g.w; wx++) {
            if (!(sp[wx] & 0x80))
                continue;
            int x = extra + wx + shift;
            if (x >= 0 && x < width)
                dst[x] = rgb(pal[sp[wx] & 31]);
        }
    }
    return true;
}

void self_check()
{
    // Every frame: this tick drawn at alpha 0 must equal the last tick drawn
    // at alpha 1, or motion would jump at each tick.
    static uint32_t last_end[kMaxWidth * kHeight], start[kMaxWidth * kHeight];
    static bool have_last;
    static double worst;
    static int checked;
    constexpr int kW = 368;
    if (render(start, kW, 0.0f, true) && have_last && shown_cur.g.v0 == shown_prev.g.v0) {
        int bad = 0, n = 0;
        for (int y = 0; y < kHeight; y++) {
            if (hud_band(shown_cur.g.v0 + y))
                continue;
            for (int x = 0; x < kW; x++, n++)
                bad += start[y * kW + x] != last_end[y * kW + x];
        }
        double off = 100.0 * bad / n;
        worst = off > worst ? off : worst;
        checked++;
    }
    have_last = render(last_end, kW, 1.0f, true);
    static int frames;
    if (++frames % 50)
        return;
    std::printf("tick joins: %d checked, worst %.2f%% off; ", checked, worst);
    worst = 0;
    checked = 0;
    Geometry g;
    if (!geometry(&g))
        return;
    const Params &c = cam_src();
    // Background: tiles and parallax against the game's own middle.
    int tiles_bad = 0, par_bad = 0, count = 0;
    for (int y = 0; y < kHeight; y += 2)
        for (int x = 0; x < g.w; x += 2) {
            int real = index_buffer[(g.v0 + y - kFbY0) * kFbW + (g.h0 + x - kFbX0)];
            tiles_bad += (real & 15) != tile_index(c, c.camx + x + tile_dx, c.camy + y + tile_dy);
            par_bad += (real >> 4 & 1) != parallax_bit(g.par_x0 + x, g.par_y0 + y);
            count++;
        }
    // Objects: for each history entry and offset, how many of the object
    // pixels it would draw in the middle agree with the game's picture.
    // Sprite objects and tile items are scored apart.
    static uint8_t real[320 * kHeight];
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < g.w; x++)
            real[y * g.w + x] = index_buffer[(g.v0 + y - kFbY0) * kFbW + (g.h0 + x - kFbX0)] & 15;
    struct Best { double score; int lag, dx, dy; size_t n; };
    auto score_kind = [&](bool items) {
        Best best{-1, 0, 0, 0, 0};
        for (int lag = 0; lag < kHist; lag++)
            for (int dy = -24; dy <= 24; dy++)
                for (int dx = -24; dx <= 24; dx++) {
                    size_t hit = 0, n = 0;
                    for (const Obj &o : hist[lag].objs) {
                        if ((o.mask == 0) != items)
                            continue;
                        int wx = o.x - c.camx + dx, wy = o.y - c.camy + dy;
                        uint32_t plane = uint32_t(o.w) * 2 * uint32_t(o.h);
                        for (int r = 0; r < o.h; r += 2)
                            for (int col = 0; col < o.w * 16; col += 2) {
                                int x = wx + col, y = wy + r;
                                if (x < 0 || x >= g.w || y < 0 || y >= kHeight)
                                    continue;
                                uint32_t word = (uint32_t(r) * o.w + uint32_t(col >> 4)) * 2;
                                int bit = 15 - (col & 15);
                                if (o.mask && !((chip16(o.mask + word) >> bit) & 1))
                                    continue;
                                int v = 0;
                                if (o.flash > 0)
                                    v = 15;
                                else if (o.flash == 0)
                                    for (int pl = 0; pl < 4; pl++)
                                        v |= ((chip16(o.gfx + uint32_t(pl) * plane + word) >> bit) & 1) << pl;
                                hit += real[y * g.w + x] == v;
                                n++;
                            }
                    }
                    double sc = n ? double(hit) / double(n) : 0;
                    if (n > 20 && sc > best.score)
                        best = {sc, lag, dx, dy, n};
                }
        return best;
    };
    Best bobs = score_kind(false), tiles_o = score_kind(true);
    int items = 0;
    for (const Obj &o : obj_src().objs)
        items += o.mask == 0;
    // A whole-frame redraw from state against the game's own picture (outside
    // the HUD bands, where sprites are shown that the redraw moves).
    static uint32_t full[kMaxWidth * kHeight];
    double full_off = -1;
    if (render(full, g.w, 1.0f, true)) {
        int bad = 0, n = 0;
        for (int y = 0; y < kHeight; y++) {
            int line = g.v0 + y - kFbY0;
            for (int x = 0; x < g.w; x++) {
                if (sprite_buffer[line * kFbW + (g.h0 + x - kFbX0)] & 0x80)
                    continue;   // the score and lives, which the redraw moves
                bad += full[y * g.w + x] != framebuffer[line * kFbW + (g.h0 + x - kFbX0)];
                n++;
            }
        }
        full_off = 100.0 * bad / n;
    }
    std::printf("whole-frame redraw %.2f%% off; ", full_off);
    std::printf("wide check (cam %d,%d): tiles %.1f%% off, parallax %.1f%% off, %zu objects (%d tile items); "
                "sprites %.1f%% at lag %d %d,%d (%zu px); items %.1f%% at lag %d %d,%d (%zu px)\n",
                c.camx, c.camy, 100.0 * tiles_bad / count, 100.0 * par_bad / count, obj_src().objs.size(), items,
                100 * bobs.score, bobs.lag, bobs.dx, bobs.dy, bobs.n, 100 * tiles_o.score, tiles_o.lag, tiles_o.dx,
                tiles_o.dy, tiles_o.n);
}

} // namespace wide
