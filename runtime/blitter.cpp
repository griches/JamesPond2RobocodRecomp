// Blitter: area copies with shifts, masks, minterms and fills, and line mode.
// A blit completes the moment BLTSIZE is written, so BBUSY never reads set.
#include "amiga.h"

namespace amiga {

extern bool blitter_zero;

static inline uint16_t &R(uint32_t off) { return regs[off >> 1]; }

static uint32_t ptr(uint32_t off) { return uint32_t(R(off)) << 16 | R(off + 2); }
static void set_ptr(uint32_t off, uint32_t a)
{
    R(off) = uint16_t(a >> 16) & 0x1F;
    R(off + 2) = uint16_t(a) & 0xFFFE;
}

static inline uint16_t minterm(uint16_t a, uint16_t b, uint16_t c, uint8_t mt)
{
    uint16_t r = 0;
    if (mt & 0x80) r |= a & b & c;
    if (mt & 0x40) r |= a & b & ~c;
    if (mt & 0x20) r |= a & ~b & c;
    if (mt & 0x10) r |= a & ~b & ~c;
    if (mt & 0x08) r |= ~a & b & c;
    if (mt & 0x04) r |= ~a & b & ~c;
    if (mt & 0x02) r |= ~a & ~b & c;
    if (mt & 0x01) r |= ~a & ~b & ~c;
    return r;
}

static void area_blit()
{
    uint16_t con0 = R(0x040), con1 = R(0x042);
    uint16_t size = R(0x058);
    int h = size >> 6, w = size & 63;
    if (!h) h = 1024;
    if (!w) w = 64;
    bool useA = con0 & 0x800, useB = con0 & 0x400, useC = con0 & 0x200, useD = con0 & 0x100;
    uint8_t mt = uint8_t(con0);
    int ash = con0 >> 12, bsh = con1 >> 12;
    bool desc = con1 & 2;
    bool fill = con1 & 0x18;
    bool efe = con1 & 0x10;
    int inc = desc ? -2 : 2;
    int amod = int16_t(R(0x064)) & ~1, bmod = int16_t(R(0x062)) & ~1;
    int cmod = int16_t(R(0x060)) & ~1, dmod = int16_t(R(0x066)) & ~1;
    if (desc) {
        amod = -amod;
        bmod = -bmod;
        cmod = -cmod;
        dmod = -dmod;
    }
    uint32_t apt = ptr(0x050), bpt = ptr(0x04C), cpt = ptr(0x048), dpt = ptr(0x054);
    uint16_t adat = R(0x074), bdat = R(0x072), cdat = R(0x070);
    uint16_t aold = 0, bold = 0;
    bool zero = true;

    for (int y = 0; y < h; y++) {
        bool carry = con1 & 4;
        uint16_t mask = R(0x044);
        for (int x = 0; x < w; x++) {
            if (x == w - 1)
                mask &= R(0x046);
            uint16_t anew = useA ? chip16(apt) : adat;
            if (useA) apt += inc;
            uint16_t bnew = useB ? chip16(bpt) : bdat;
            if (useB) bpt += inc;
            uint16_t chold = useC ? chip16(cpt) : cdat;
            if (useC) cpt += inc;

            uint16_t am = anew & mask;
            uint16_t ahold, bhold;
            if (desc) {
                ahold = uint16_t((uint32_t(am) << 16 | aold) >> (16 - ash));
                bhold = uint16_t((uint32_t(bnew) << 16 | bold) >> (16 - bsh));
            } else {
                ahold = uint16_t((uint32_t(aold) << 16 | am) >> ash);
                bhold = uint16_t((uint32_t(bold) << 16 | bnew) >> bsh);
            }
            aold = am;
            bold = bnew;
            uint16_t d = minterm(ahold, bhold, chold, mt);
            if (fill) {
                uint16_t out = 0;
                for (int bit = 0; bit < 16; bit++) {
                    int in = (d >> bit) & 1;
                    carry ^= in;
                    int o = efe ? carry : (in | carry);
                    out |= uint16_t(o << bit);
                }
                d = out;
            }
            if (d)
                zero = false;
            if (useD) {
                chip16_write(dpt, d);
                dpt += inc;
            }
            mask = 0xFFFF;
        }
        if (useA) apt += amod;
        if (useB) bpt += bmod;
        if (useC) cpt += cmod;
        if (useD) dpt += dmod;
    }
    set_ptr(0x050, apt);
    set_ptr(0x04C, bpt);
    set_ptr(0x048, cpt);
    set_ptr(0x054, dpt);
    blitter_zero = zero;
}

static void line_blit()
{
    uint16_t con0 = R(0x040), con1 = R(0x042);
    int len = R(0x058) >> 6;
    if (!len) len = 1024;
    uint8_t mt = uint8_t(con0);
    int shift = con0 >> 12;
    bool sing = con1 & 2;
    bool sign = con1 & 0x40;
    bool sud = con1 & 0x10, sul = con1 & 8, aul = con1 & 4;
    int16_t amod = int16_t(R(0x064)), bmod = int16_t(R(0x062));
    int16_t cmod = int16_t(R(0x060));
    uint32_t apt = ptr(0x050);
    uint32_t cpt = ptr(0x048), dpt = ptr(0x054);
    uint16_t adat = R(0x074), afwm = R(0x044);
    int bsh = con1 >> 12;
    uint16_t bdat = R(0x072);
    uint16_t tex = uint16_t((bdat >> bsh) | (bdat << ((16 - bsh) & 15)));
    bool onedot = false;
    bool zero = true;

    auto incx = [&] { if (++shift == 16) { shift = 0; cpt += 2; } };
    auto decx = [&] { if (shift-- == 0) { shift = 15; cpt -= 2; } };
    auto incy = [&] { cpt += cmod; onedot = false; };
    auto decy = [&] { cpt -= cmod; onedot = false; };

    for (int i = 0; i < len; i++) {
        uint16_t c = chip16(cpt);
        uint16_t a = uint16_t((adat & afwm) >> shift);
        uint16_t b = (tex & 1) ? 0xFFFF : 0;
        bool draw = !sing || !onedot;
        uint16_t d = minterm(a, b, c, mt);
        onedot = true;
        uint32_t at = cpt;
        if (sign)
            apt += bmod;
        else
            apt += amod;
        if (!sign) {
            if (sud) { if (sul) decy(); else incy(); }
            else { if (sul) decx(); else incx(); }
        }
        if (sud) { if (aul) decx(); else incx(); }
        else { if (aul) decy(); else incy(); }
        sign = int16_t(apt) < 0;
        tex = uint16_t(tex << 1 | tex >> 15);
        if (draw) {
            chip16_write(i == 0 ? dpt : at, d);
            if (d)
                zero = false;
        }
        dpt = cpt;
    }
    set_ptr(0x050, apt);
    set_ptr(0x048, cpt);
    set_ptr(0x054, dpt);
    R(0x042) = uint16_t((con1 & ~0x40) | (sign ? 0x40 : 0));
    blitter_zero = zero;
}

void blitter_start()
{
    if (R(0x042) & 1)
        line_blit();
    else
        area_blit();
    request_irq(INT_BLIT);
}

} // namespace amiga
