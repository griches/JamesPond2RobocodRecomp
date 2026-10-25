// Agnus/Denise display: bitplane fetch, sprites and colour output, drawn a
// line at a time. The copper calls display_catch_up() before each register
// write, so colour changes part way along a line land where they should.
#include "amiga.h"
#include "state.h"

#include <cstring>

namespace amiga {

uint32_t framebuffer[kFbW * kFbH];

// Copper writes land at colour clock h; on screen that is lowres column 2h+1.
static inline int cck_to_x(int h) { return 2 * h + 1; }

constexpr int kLineX = 0x200;   // lowres columns per line, more than a line holds

static int cur_line;
static int drawn_to;            // columns [0, drawn_to) of this line are done
static bool fetched;
static bool bpl_active;          // bitplane DMA ran on this line
static int pf_words;             // words fetched per plane
static uint8_t pf_index[kLineX]; // playfield colour index per lowres column (raw plane bits)
static uint8_t pf_index_hi[kLineX]; // second hires pixel when HIRES
static int ham_prev;

// ---- sprites ----------------------------------------------------------------
struct Sprite {
    uint16_t pos, ctl, data, datb;
    bool armed;
    enum { FETCH_CTL, WAIT_START, ACTIVE, DONE } state;
    int vstart() const { return (pos >> 8) | ((ctl & 4) << 6); }
    int vstop() const { return (ctl >> 8) | ((ctl & 2) << 7); }
    int hstart() const { return ((pos & 0xFF) << 1) | (ctl & 1); }
};
static Sprite spr[8];
constexpr int kSpriteDmaFirstLine = 25;
static uint8_t spr_index[kLineX];  // 0 = none, else colour 16..31
static uint8_t spr_pair[kLineX];   // which sprite pair drew it

static inline uint32_t rgb12(uint16_t c)
{
    uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return 0xFF000000u | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
}

void display_sprite_reg(uint32_t off, uint16_t v)
{
    Sprite &s = spr[(off - 0x140) >> 3];
    switch (off & 6) {
    case 0: s.pos = v; break;
    case 2: s.ctl = v; s.armed = false; break;
    case 4: s.data = v; s.armed = true; break;
    case 6: s.datb = v; break;
    }
}

void display_frame_start()
{
    for (Sprite &s : spr)
        s.state = Sprite::FETCH_CTL;
}

static uint32_t spr_pt(int n) { return uint32_t(regs[(0x120 >> 1) + n * 2]) << 16 | regs[(0x122 >> 1) + n * 2]; }
static void set_spr_pt(int n, uint32_t a)
{
    regs[(0x120 >> 1) + n * 2] = uint16_t(a >> 16);
    regs[(0x122 >> 1) + n * 2] = uint16_t(a);
}

static void sprite_dma(int line)
{
    uint16_t dmacon = regs[0x002 >> 1];
    if (!(dmacon & DMA_EN) || !(dmacon & DMA_SPR) || line < kSpriteDmaFirstLine)
        return;
    for (int n = 0; n < 8; n++) {
        Sprite &s = spr[n];
        uint32_t pt = spr_pt(n);
        switch (s.state) {
        case Sprite::FETCH_CTL:
            display_sprite_reg(0x140 + n * 8, chip16(pt));
            display_sprite_reg(0x142 + n * 8, chip16(pt + 2));
            set_spr_pt(n, pt + 4);
            s.state = (s.pos == 0 && s.ctl == 0) ? Sprite::DONE : Sprite::WAIT_START;
            break;
        case Sprite::WAIT_START:
            if (line == s.vstart()) {
                s.state = Sprite::ACTIVE;
                display_sprite_reg(0x146 + n * 8, chip16(pt + 2));
                display_sprite_reg(0x144 + n * 8, chip16(pt));
                set_spr_pt(n, pt + 4);
            }
            break;
        case Sprite::ACTIVE:
            if (line == s.vstop()) {
                display_sprite_reg(0x140 + n * 8, chip16(pt));
                display_sprite_reg(0x142 + n * 8, chip16(pt + 2));
                set_spr_pt(n, pt + 4);
                s.state = (s.pos == 0 && s.ctl == 0) ? Sprite::DONE : Sprite::WAIT_START;
            } else {
                display_sprite_reg(0x146 + n * 8, chip16(pt + 2));
                display_sprite_reg(0x144 + n * 8, chip16(pt));
                set_spr_pt(n, pt + 4);
            }
            break;
        case Sprite::DONE:
            break;
        }
    }
}

static void build_sprite_line()
{
    std::memset(spr_index, 0, sizeof spr_index);
    // Lower numbered sprites win, so draw from 7 down to 0.
    for (int n = 7; n >= 0; n--) {
        Sprite &s = spr[n];
        if (!s.armed)
            continue;
        int pair = n >> 1;
        bool attached = (n & 1) && (s.ctl & 0x80);
        if ((n & 1) == 0 && (spr[n + 1].ctl & 0x80) && spr[n + 1].armed)
            continue;   // drawn together with its odd partner
        int x0 = s.hstart() + 1;
        for (int i = 0; i < 16; i++) {
            int x = x0 + i;
            if (x < 0 || x >= kLineX)
                continue;
            int bit = 15 - i;
            int v = ((s.data >> bit) & 1) | ((s.datb >> bit) & 1) << 1;
            if (attached) {
                const Sprite &e = spr[n - 1];
                int xe = x - (e.hstart() + 1);
                int ve = 0;
                if (xe >= 0 && xe < 16)
                    ve = ((e.data >> (15 - xe)) & 1) | ((e.datb >> (15 - xe)) & 1) << 1;
                int c = v << 2 | ve;
                if (c) {
                    spr_index[x] = uint8_t(16 + c);
                    spr_pair[x] = uint8_t(pair);
                }
            } else if (v) {
                spr_index[x] = uint8_t(16 + pair * 4 + v);
                spr_pair[x] = uint8_t(pair);
            }
        }
    }
}

// ---- bitplanes ------------------------------------------------------------------
static uint32_t bpl_pt(int p) { return uint32_t(regs[(0x0E0 >> 1) + p * 2]) << 16 | regs[(0x0E2 >> 1) + p * 2]; }
static void set_bpl_pt(int p, uint32_t a)
{
    regs[(0x0E0 >> 1) + p * 2] = uint16_t(a >> 16);
    regs[(0x0E2 >> 1) + p * 2] = uint16_t(a);
}

static int planes()
{
    int n = (regs[0x100 >> 1] >> 12) & 7;
    return n > 6 ? 4 : n;
}

static bool hires() { return regs[0x100 >> 1] & 0x8000; }

static void vertical_window(int *v0, int *v1)
{
    uint16_t strt = regs[0x08E >> 1], stop = regs[0x090 >> 1];
    *v0 = strt >> 8;
    *v1 = (stop >> 8) | ((stop & 0x8000) ? 0 : 0x100);
}

static void horizontal_window(int *h0, int *h1)
{
    *h0 = regs[0x08E >> 1] & 0xFF;
    *h1 = (regs[0x090 >> 1] & 0xFF) | 0x100;
}

static void fetch_line()
{
    fetched = true;
    std::memset(pf_index, 0, sizeof pf_index);
    std::memset(pf_index_hi, 0, sizeof pf_index_hi);
    bpl_active = false;
    uint16_t dmacon = regs[0x002 >> 1];
    if (!(dmacon & DMA_EN) || !(dmacon & DMA_BPL))
        return;
    int v0, v1;
    vertical_window(&v0, &v1);
    if (cur_line < v0 || cur_line >= v1)
        return;
    int np = planes();
    if (np == 0)
        return;
    bpl_active = true;

    int strt = regs[0x092 >> 1] & 0xFC, stop = regs[0x094 >> 1] & 0xFC;
    bool hr = hires();
    pf_words = hr ? (stop - strt) / 4 + 2 : (stop - strt) / 8 + 1;
    if (pf_words < 0)
        pf_words = 0;
    if (pf_words > 64)
        pf_words = 64;
    int first = hr ? 2 * strt + 9 : 2 * strt + 17;
    uint16_t con1 = regs[0x102 >> 1];
    int delay[2] = {con1 & 15, (con1 >> 4) & 15};

    uint16_t words[6][64];
    for (int p = 0; p < np; p++) {
        uint32_t pt = bpl_pt(p);
        for (int w = 0; w < pf_words; w++)
            words[p][w] = chip16(pt + w * 2);
        int16_t mod = int16_t(regs[(p & 1 ? 0x10A : 0x108) >> 1]);
        set_bpl_pt(p, pt + pf_words * 2 + mod);
    }

    int pixels = pf_words * 16;
    for (int p = 0; p < np; p++) {
        int d = delay[p & 1];
        for (int i = 0; i < pixels; i++) {
            int bit = (words[p][i >> 4] >> (15 - (i & 15))) & 1;
            if (!bit)
                continue;
            if (hr) {
                int hp = i + 2 * d;   // hires pixel column from the fetch start
                int x = first + (hp >> 1);
                if (x >= 0 && x < kLineX) {
                    if (hp & 1)
                        pf_index_hi[x] |= uint8_t(1 << p);
                    else
                        pf_index[x] |= uint8_t(1 << p);
                }
            } else {
                int x = first + i + d;
                if (x >= 0 && x < kLineX)
                    pf_index[x] |= uint8_t(1 << p);
            }
        }
    }
}

// Colour for one playfield index, before sprites. Returns false for "nothing
// drawn here" (index 0, or transparent in dual playfield) with *front_pf set
// to the playfield that would show.
static uint32_t playfield_colour(int idx, int x)
{
    uint16_t con0 = regs[0x100 >> 1];
    int np = planes();
    const uint16_t *col = &regs[0x180 >> 1];
    if (con0 & 0x0800 && np == 6) {   // HAM
        int c = idx & 15;
        switch (idx >> 4) {
        case 0: ham_prev = col[c]; break;
        case 1: ham_prev = (ham_prev & 0xFF0) | c; break;
        case 2: ham_prev = (ham_prev & 0x0FF) | c << 8; break;
        case 3: ham_prev = (ham_prev & 0xF0F) | c << 4; break;
        }
        (void)x;
        return rgb12(uint16_t(ham_prev));
    }
    if (np == 6 && !(con0 & 0x0400)) {   // extra half-brite
        uint16_t c = col[idx & 31];
        if (idx & 32)
            c = (c >> 1) & 0x777;
        return rgb12(c);
    }
    return rgb12(col[idx & 31]);
}

static uint32_t mix(uint32_t a, uint32_t b)
{
    return 0xFF000000u | (((a & 0xFEFEFE) >> 1) + ((b & 0xFEFEFE) >> 1));
}

static void draw_span(int x0, int x1)
{
    if (cur_line < kFbY0 || cur_line >= kFbY0 + kFbH)
        return;
    uint32_t *row = &framebuffer[(cur_line - kFbY0) * kFbW];
    int v0, v1, h0, h1;
    vertical_window(&v0, &v1);
    horizontal_window(&h0, &h1);
    bool in_v = cur_line >= v0 && cur_line < v1;
    uint16_t con0 = regs[0x100 >> 1], con2 = regs[0x104 >> 1];
    bool dpf = con0 & 0x0400;
    bool hr = con0 & 0x8000;
    int pf1p = con2 & 7, pf2p = (con2 >> 3) & 7;
    bool pf2pri = con2 & 0x40;
    const uint16_t *col = &regs[0x180 >> 1];
    uint32_t border = rgb12(col[0]);

    for (int x = x0 < kFbX0 ? kFbX0 : x0; x < x1 && x < kFbX0 + kFbW; x++) {
        uint32_t out;
        if (!in_v || x < h0 || x >= h1) {
            out = border;
        } else {
            int idx = bpl_active ? pf_index[x] : 0;
            // which playfield (1 or 2) owns the pixel, 0 = background
            int owner = 0;
            uint32_t pfc;
            if (dpf) {
                int a = (idx & 1) | (idx >> 1 & 2) | (idx >> 2 & 4);        // planes 1,3,5
                int b = (idx >> 1 & 1) | (idx >> 2 & 2) | (idx >> 3 & 4);   // planes 2,4,6
                int pick;
                if (a && b)
                    pick = pf2pri ? 2 : 1;
                else
                    pick = a ? 1 : (b ? 2 : 0);
                owner = pick;
                pfc = rgb12(pick == 1 ? col[a] : pick == 2 ? col[8 + b] : col[0]);
            } else {
                owner = idx ? 2 : 0;
                pfc = idx || (con0 & 0x0800) ? playfield_colour(idx, x) : rgb12(col[0]);
                if (hr && bpl_active)
                    pfc = mix(pfc, pf_index_hi[x] ? playfield_colour(pf_index_hi[x], x) : rgb12(col[0]));
            }
            out = pfc;
            int s = spr_index[x];
            if (s) {
                int pair = spr_pair[x];
                int prio = owner == 1 ? pf1p : pf2p;
                if (owner == 0 || pair < prio)
                    out = rgb12(col[s]);
            }
        }
        row[x - kFbX0] = out;
    }
}

void display_catch_up(int h)
{
    int x = cck_to_x(h);
    if (x <= drawn_to)
        return;
    if (!fetched && h >= (regs[0x092 >> 1] & 0xFC))
        fetch_line();
    draw_span(drawn_to, x);
    drawn_to = x;
}

void display_line_begin(int line)
{
    cur_line = line;
    drawn_to = 0;
    fetched = false;
    bpl_active = false;
    ham_prev = regs[0x180 >> 1];
    sprite_dma(line);
    build_sprite_line();
}

void display_line_end()
{
    display_catch_up(kCcksPerLine);
    if (!fetched)
        fetch_line();
    if (drawn_to < kLineX)
        draw_span(drawn_to, kLineX);
}

void display_state(StateIO &s) { s.bytes(spr, sizeof spr); }

} // namespace amiga
