#include "wide.h"

#include "amiga.h"
#include "disk.h"
#include "png.h"
#include "state.h"

#include <algorithm>
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
constexpr uint32_t kStar = 0x2AF4E;                 // first frame of the star an enemy bursts into
constexpr uint32_t kStarCrc = 0x63AC0E5A;           // of its 512 bytes, in every level

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
// wasn't blitted lately: the last blit into it from before the history
// (the game stops blitting while the layer stands still), else see Found.
Params last_into[2];
int last_into_next;
const Params *blit_for_shown()
{
    uint32_t shown = playfield_start.bpl[4];
    for (int k : {par_lag, par_lag + 1, par_lag - 1}) {
        const Params &p = hist[k];
        if (p.par && (p.par_d == shown || p.par_d + 2 == shown))
            return &p;
    }
    for (const Params &p : last_into)
        if (p.par && (p.par_d == shown || p.par_d + 2 == shown))
            return &p;
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
    bool flip;           // shown upside down: window row y is world row camy + 207 - y
    int period;          // no place in the image, but plane 5 repeats every this many columns
};

// The upside-down levels show their bitplanes bottom up (a negative modulo
// steps each line back a row); the parallax image is read that way too.
bool par_flip;
int par_row(int y0, int y) { return par_flip ? y0 - y : y0 + y; }

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
// After a search finds nothing, none runs again before this frame. Unlike a
// buffer's own retry_frame it outlives forget_found(): a layer that is not
// in the image (the tube rooms) would otherwise be searched for with every
// blit, the whole image each time, and the game slows down as it scrolls.
int search_after;
// The search through the whole image covers a band of its rows a frame (all
// at once took over 10 ms on a Steam Deck); the next band starts here. Just
// after a load it covers the lot, so the first picture has its layer.
int scan_row;
bool scan_all;
// Where the layer was last placed (no scroll delay), to search near first.
int hint_x, hint_y;
bool hint_ok;

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
            bad += parallax_bit(x0 + x, par_row(y0, y)) != shown_bit5(h0, v0, x, y);
    return bad * 10 <= n;
}

// How many of a fine grid of window pixels disagree with the image placed at (px, py).
int mismatches(int h0, int v0, int w, int px, int py, int *count)
{
    int bad = 0, n = 0;
    for (int y = 1; y < kHeight; y += 4)
        for (int x = 2; x < w; x += 5, n++)
            bad += parallax_bit(px + x, par_row(py, y)) != shown_bit5(h0, v0, x, y);
    *count = n;
    return bad;
}

// The smallest number of columns after which the window's bitplane 5 repeats
// itself exactly (a layer built from a tile, as in the tube rooms), or 0.
int screen_period(int h0, int v0, int w)
{
    for (int p = 16; p <= w / 2; p += 16) {
        int bad = 0, n = 0;
        for (int y = 0; y < kHeight; y += 3)
            for (int x = 0; x + p < w; x += 3, n++)
                bad += shown_bit5(h0, v0, x, y) != shown_bit5(h0, v0, x + p, y);
        if (bad * 500 <= n)
            return p;
    }
    return 0;
}

// Keeps a found position on the layer as it scrolls: the best fit within three
// pixels of where the layer was last frame (in either buffer; a double-buffered
// layer has moved twice since this buffer was last shown) or of this buffer's
// own last place. False when nothing near fits any more.
// Where the picture repeats, several places fit equally well: ties go to the
// one nearest last frame's place, then this buffer's own, so the two buffers
// agree and smooth motion doesn't slide the layer between equal places.
bool follow_parallax(int h0, int v0, int w, int delay, Found *f)
{
    int best = 1 << 30, best_far = 0, bx = 0, by = 0, n = 1;
    int centres[2][2] = {{hint_x, hint_y}, {f->base_x, f->y0}};
    for (int c = hint_ok ? 0 : 1; c < 2; c++)
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int px = (centres[c][0] - delay + dx) & 511, py = (centres[c][1] + dy) & 255;
                int m = mismatches(h0, v0, w, px, py, &n), far = dx * dx + dy * dy;
                if (m < best || (m == best && far < best_far)) {
                    best = m;
                    best_far = far;
                    bx = px;
                    by = py;
                }
            }
    if (best * 10 > n)
        return false;
    f->base_x = bx + delay;
    f->y0 = by;
    return true;
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
    // Rarer bits first: a wrong position fails on them soonest.
    bool rare = set * 2 < n;
    std::stable_partition(pts.begin(), pts.end(), [&](const Pt &p) { return p.bit == int(rare); });
    // A wrong place misses about half the rare bits, so a few misses among the
    // first samples end the check early.
    auto fits = [&](int px, int py) {
        int bad = 0;
        for (int i = 0; i < n; i++)
            if (parallax_bit(px + pts[i].x, par_row(py, pts[i].y)) != pts[i].bit && ++bad > (i < 24 ? 3 : n / 50))
                return false;
        f->base_x = px + delay;
        f->y0 = py;
        return true;
    };
    // The layer moves a few pixels a frame, so look near where it was first.
    // A fit is only near the place (the samples are sparse): settle it on the best.
    if (hint_ok)
        for (int r = 0; r <= 32; r++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx += (std::abs(dy) == r ? 1 : 2 * r))
                    if (fits((hint_x - delay + dx) & 511, (hint_y + dy) & 255)) {
                        scan_row = 0;
                        return follow_parallax(h0, v0, w, delay, f);
                    }
    int end = scan_all ? 256 : std::min(scan_row + 32, 256);
    scan_all = false;
    for (int py = scan_row; py < end; py++)
        for (int px = 0; px < 512; px++)
            if (fits(px, py)) {
                scan_row = 0;
                return follow_parallax(h0, v0, w, delay, f);
            }
    scan_row = end % 256;
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
    g->period = 0;
    g->h0 = ps.diwstrt & 0xFF;
    g->v0 = ps.diwstrt >> 8;
    g->w = ((ps.diwstop & 0xFF) | 0x100) - g->h0;
    if (g->w <= 0 || g->w > 320)
        return false;
    int first = 2 * (ps.ddfstrt & 0xFC) + 17;          // display column of the first fetched pixel
    int delay = ps.bplcon1 & 15;                       // plane 5 is odd: playfield 1 scroll
    int fetch = (((ps.ddfstop & 0xFC) - (ps.ddfstrt & 0xFC)) / 8 + 1) * 2;   // bytes a line, lowres
    g->flip = ps.bpl1mod + fetch < 0;
    par_flip = g->flip;
    // The blit position is worked out for a buffer read top down.
    const Params *pp = g->flip ? nullptr : blit_for_shown();
    if (pp) {
        uint32_t off = pp->par_a - kParallax;
        int lead = int(int32_t(ps.bpl[4] - pp->par_d)) * 8;
        g->par_x0 = int(off % 64) * 8 + lead - pp->par_shift + (g->h0 - first) - delay;
        g->par_y0 = int(off / 64);
        g->par_known = true;
        // A blit position carried over (from before a save, or another room)
        // can be stale: trust it only while it matches what is on screen.
        if (!layer_matches(g->h0, g->v0, g->w, g->par_x0, g->par_y0))
            pp = nullptr;
    }
    if (!pp) {
        Found &f = found_for(ps.bpl[4]);
        if (f.valid && !follow_parallax(g->h0, g->v0, g->w, delay, &f))
            f.valid = false;
        if (!f.valid && frames_seen >= f.retry_frame && frames_seen >= search_after) {
            f.valid = recover_parallax(g->h0, g->v0, g->w, delay, &f);
            if (!f.valid && scan_row == 0)   // the whole image searched: wait before another go
                f.retry_frame = search_after = frames_seen + 25;
        }
        // Not found: draw the margins without the layer rather than drop widescreen,
        // unless it repeats across the window, when the margins can repeat it too.
        g->par_x0 = f.valid ? f.base_x - delay : 0;
        g->par_y0 = f.valid ? f.y0 : 0;
        g->par_known = f.valid;
        g->period = f.valid ? 0 : screen_period(g->h0, g->v0, g->w);
    }
    if (g->par_known) {
        hint_x = g->par_x0 + delay;
        hint_y = g->par_y0;
        hint_ok = true;
    }
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
              bool as_sprite = false, bool behind = true, int rows = -1, bool flip = false)
{
    uint32_t plane = o.column ? 2 : uint32_t(o.w) * 2 * uint32_t(o.h);
    if (rows < 0 || !o.column)
        rows = o.h;
    for (int r = 0; r < rows; r++) {
        int y = flip ? height - 1 - (wy + r) : wy + r;
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
    if ((con0 & 0x0FFF) != 0x09F0 || (size & 63) != 19)
        return;
    uint32_t a = uint32_t(r[0x050 >> 1]) << 16 | r[0x052 >> 1];
    if (a < kParallax || a >= kParallax + kParallaxBytes)
        return;
    // The game draws the layer into the screen buffer from one word before
    // it, and the screen buffer follows the image, so the first word lands on
    // the image's last word: a stray dash then shows wherever the image's
    // bottom right corner comes round (an original bug). The word is never
    // shown on screen; keep the image's own.
    if ((uint32_t(r[0x054 >> 1]) << 16 | r[0x056 >> 1]) == kParallax + kParallaxBytes - 2)
        blit_keep = kParallax + kParallaxBytes - 2;
    if (building.par)
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
    if (s.loading()) {
        forget_found();
        for (Params &p : last_into)
            p = Params{};
        hint_ok = false;
        search_after = scan_row = 0;
        scan_all = true;
        repair_parallax();
    }
}

// Saves made before blit_keep may carry the stray word in the image. The
// game builds the image from tiles written twice, 256 pixels apart, so when
// the two halves agree everywhere else the word's twin gives it back.
void repair_parallax()
{
    constexpr uint32_t last = kParallax + kParallaxBytes - 2, twin = last - 32;
    if (chip16(last) == chip16(twin))
        return;
    for (uint32_t at = kParallax; at < last; at += 2)
        if ((at - kParallax) % 64 < 32 && at != twin && chip16(at) != chip16(at + 32))
            return;
    chip16_write(last, chip16(twin));
}

static int extra_px;
void set_view_extra(int pixels) { extra_px = pixels; }

static bool available = true;
void set_available(bool on) { available = on; }
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

static int stars_last, star_count;
int bursts() { return star_count; }

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
    // A blit leaving the history is the last word on its buffer until the
    // next one.
    if (const Params &old = hist[kHist - 1]; old.par) {
        Params *slot = &last_into[last_into_next];
        for (Params &p : last_into)
            if (p.par && p.par_d == old.par_d)
                slot = &p;
        if (slot == &last_into[last_into_next])
            last_into_next ^= 1;
        slot->par = true;
        slot->par_a = old.par_a;
        slot->par_d = old.par_d;
        slot->par_shift = old.par_shift;
    }
    // Enemies bursting into a star, for the pad: stars in their first frame
    // that weren't there last frame.
    int stars = 0;
    for (const Obj &o : building.objs)
        stars += o.gfx == kStar;
    if (stars > stars_last && disk::crc32(&ram[kStar], 512) == kStarCrc)
        star_count += stars - stars_last;
    stars_last = stars;
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

// Where the game puts the parallax image for the camera at x: its table at
// $A3C0, by the level's mode in $ABE. Half the camera, a quarter, one and a
// half times; the other modes move it on their own (drifting clouds, rising
// bubbles, animations) or not at all.
int par_for_cam(int x)
{
    switch (chip16(0xABE)) {
    case 1: return x >> 1;
    case 3: return x >> 2;
    case 10: return x + (x >> 1);
    default: return 0;
    }
}

// How wide the parallax picture is before it repeats across: 64, 128 or 256.
// Positions that far apart look the same, so the two screen buffers may be
// placed a whole repeat apart, and blending must treat them as one.
int par_period()
{
    static int frame = -1, period = 256;
    if (frame == frames_seen)
        return period;
    frame = frames_seen;
    for (int p : {64, 128}) {
        int same = 0;
        for (uint32_t r = 0; r < 256; r++) {
            uint32_t row = kParallax + r * 64;
            bool eq = true;
            for (uint32_t i = 0; i < 32 && eq; i += 2)
                eq = chip16(row + i) == chip16(row + uint32_t(p / 8) + i);
            same += eq;
        }
        if (same >= 250) {
            period = p;
            return period;
        }
    }
    period = 256;
    return period;
}

int lerp_wrap(int a, int b, float alpha, int period)
{
    int d = ((b - a) % period + period + period / 2) % period - period / 2;
    if (std::abs(d) > 48 || alpha >= 1)
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
    if (!available)
        return false;
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
    // The parallax goes with the view, not with the game's camera: while the
    // view is held at a level's end, the camera moving on behind it mustn't
    // move the background (it would drift the wrong way, then catch up). So
    // each tick's position is taken for its view, and only that is blended:
    // blending the game's position and the camera apart and rounding each
    // could step it back a pixel between ticks.
    auto view_cam = [&](int cam) {
        int left = cam - extra;
        if (level_w >= width)
            left = std::clamp(left, 0, level_w - width);
        return left + extra;
    };
    auto par_for_view = [&](const Shown &s) {
        return s.g.par_x0 + par_for_cam(view_cam(s.camx)) - par_for_cam(s.camx);
    };
    // Blend on the picture's repeat (the game keeps the position modulo 256,
    // and some pictures repeat sooner): a wrap, or the two buffers placed a
    // whole repeat apart, is then a step like any other.
    int parx = pv ? lerp_wrap(par_for_view(*pv), par_for_view(cu), alpha, par_period()) : par_for_view(cu);
    if (std::getenv("JP2_PARVIEW"))
        std::fprintf(stderr, "parview: alpha %.2f cam %d shift %d image column at the view's left %d\n", alpha, camx,
                     shift, parx - extra);

    static uint8_t idx[kMaxWidth * kHeight];
    auto drawn_here = [&](int x) { return full || x < extra || x >= extra + g.w; };
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < width; x++) {
            if (!drawn_here(x))
                continue;
            int wx = x - extra + shift;   // window column
            int wy = g.flip ? kHeight - 1 - y : y + tile_dy;
            int par = g.par_known ? parallax_bit(parx + (x - extra), g.flip ? pary - y : pary + y)
                      : g.period ? shown_bit5(g.h0, g.v0, ((wx % g.period) + g.period) % g.period, y)
                                 : 0;
            idx[size_t(y) * width + x] = uint8_t(tile_index(cu.cam, camx + wx + tile_dx, camy + wy) | par << 4);
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
                     pass == 1, behind, rows, g.flip);
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
        // JP2_NO_HUD (pictures for artwork): the score and lives left out.
        static const bool no_hud = std::getenv("JP2_NO_HUD") != nullptr;
        int shift = (g.v0 + y) < 0x80 ? extra : -extra;
        for (int wx = 0; wx < g.w && !no_hud; wx++) {
            if (!(sp[wx] & 0x80))
                continue;
            int x = extra + wx + shift;
            if (x >= 0 && x < width)
                dst[x] = rgb(pal[sp[wx] & 31]);
        }
    }
    return true;
}

// Development: per-frame flicker in the game's own picture (bitplane 5) and
// in the redraw, and how far apart the two are (JP2_FLICKER=1).
void flicker_check()
{
    static uint8_t last_game[320 * kHeight];
    static uint32_t last_mine[320 * kHeight], mine[320 * kHeight];
    static bool have;
    Geometry g;
    if (!shown_cur.valid)
        return;
    g = shown_cur.g;
    if (!render(mine, g.w, 1.0f, true))
        return;
    int game_changed = 0, mine_changed = 0, apart = 0, n = 0;
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < g.w; x++, n++) {
            int line = g.v0 + y - kFbY0;
            uint8_t b = (index_buffer[line * kFbW + (g.h0 + x - kFbX0)] >> 4) & 1;
            game_changed += have && b != last_game[y * g.w + x];
            mine_changed += have && mine[y * g.w + x] != last_mine[y * g.w + x];
            apart += mine[y * g.w + x] != framebuffer[line * kFbW + (g.h0 + x - kFbX0)] &&
                     !(sprite_buffer[line * kFbW + (g.h0 + x - kFbX0)] & 0x80);
            last_game[y * g.w + x] = b;
            last_mine[y * g.w + x] = mine[y * g.w + x];
        }
    have = true;
    std::printf("flicker: game plane-5 changed %5.2f%%  redraw changed %5.2f%%  redraw vs game %5.2f%%  bpl5 %06X blit %d "
                "origin %d,%d  frame %d\n",
                100.0 * game_changed / n, 100.0 * mine_changed / n, 100.0 * apart / n, playfield_start.bpl[4],
                blit_for_shown() ? 1 : 0, g.par_x0, g.par_y0, frames_seen);
}

// The game's window, the redraw of it, and where they differ (red), stacked.
void diff_png(const char *path)
{
    if (!shown_cur.valid)
        return;
    Geometry g = shown_cur.g;
    static uint32_t mine[320 * kHeight], out[320 * kHeight * 3];
    if (!render(mine, g.w, 1.0f, true))
        return;
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < g.w; x++) {
            int line = g.v0 + y - kFbY0;
            uint32_t game = framebuffer[line * kFbW + (g.h0 + x - kFbX0)];
            bool hud = sprite_buffer[line * kFbW + (g.h0 + x - kFbX0)] & 0x80;
            out[y * g.w + x] = game;
            out[(kHeight + y) * g.w + x] = mine[y * g.w + x];
            uint32_t grey = 0xFF000000u | ((game >> 2) & 0x3F3F3F);
            out[(2 * kHeight + y) * g.w + x] = hud ? 0xFF003000u : mine[y * g.w + x] != game ? 0xFFFF2020u : grey;
        }
    write_png(path, out, g.w, kHeight * 3, g.w);
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
            tiles_bad += (real & 15) != tile_index(c, c.camx + x + tile_dx, c.camy + (g.flip ? kHeight - 1 - y : y + tile_dy));
            int par = g.par_known ? parallax_bit(g.par_x0 + x, g.flip ? g.par_y0 - y : g.par_y0 + y)
                      : g.period ? shown_bit5(g.h0, g.v0, x % g.period, y)
                                 : 0;
            par_bad += (real >> 4 & 1) != par;
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
