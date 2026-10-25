// Support for the recompiled 68000 code: CPU state, memory access and the
// flag arithmetic each instruction needs. Flag behaviour follows Musashi,
// including its choices for the officially undefined bits, so the two can be
// compared instruction by instruction.
#pragma once
#include <cstdint>

#include "amiga.h"

namespace jp2 {

struct Cpu {
    uint32_t d[8], a[8];
    uint32_t pc;
    uint32_t other_sp;      // USP while in supervisor mode, SSP in user mode
    uint8_t x, n, z, v, c;  // condition codes, each 0 or 1
    uint8_t s, t, mask;     // supervisor, trace, interrupt mask
    uint8_t stopped;
    uint8_t interp;         // run the instruction at pc in the interpreter
    int64_t cycles;         // CPU cycles since boot
    int64_t limit;          // generated code goes back to the dispatcher at this count
};
extern Cpu cpu;

using Handler = void (*)();

// ---- memory -------------------------------------------------------------------
using amiga::kRamSize;
using amiga::ram;

inline uint32_t rd8(uint32_t a)
{
    a &= 0xFFFFFF;
    return a < kRamSize ? ram[a] : amiga::read8(a);
}
inline uint32_t rd16(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < kRamSize - 1)
        return uint32_t(ram[a]) << 8 | ram[a + 1];
    return amiga::read16(a);
}
inline uint32_t rd32(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < kRamSize - 3)
        return uint32_t(ram[a]) << 24 | uint32_t(ram[a + 1]) << 16 | uint32_t(ram[a + 2]) << 8 | ram[a + 3];
    return amiga::read32(a);
}
inline void wr8(uint32_t a, uint32_t v) { amiga::write8(a, uint8_t(v)); }
inline void wr16(uint32_t a, uint32_t v) { amiga::write16(a, uint16_t(v)); }
inline void wr32(uint32_t a, uint32_t v) { amiga::write32(a, v); }

inline uint32_t sx8(uint32_t v) { return uint32_t(int32_t(int8_t(v))); }
inline uint32_t sx16(uint32_t v) { return uint32_t(int32_t(int16_t(v))); }

inline void push32(uint32_t v)
{
    cpu.a[7] -= 4;
    wr32(cpu.a[7], v);
}
inline uint32_t pop32()
{
    uint32_t v = rd32(cpu.a[7]);
    cpu.a[7] += 4;
    return v;
}

// ---- status register ------------------------------------------------------------
inline uint32_t get_ccr()
{
    return uint32_t(cpu.x << 4 | cpu.n << 3 | cpu.z << 2 | cpu.v << 1 | cpu.c);
}
inline void set_ccr(uint32_t v)
{
    cpu.x = (v >> 4) & 1;
    cpu.n = (v >> 3) & 1;
    cpu.z = (v >> 2) & 1;
    cpu.v = (v >> 1) & 1;
    cpu.c = v & 1;
}
inline uint32_t get_sr() { return uint32_t(cpu.t << 15 | cpu.s << 13 | cpu.mask << 8) | get_ccr(); }
inline void set_sr(uint32_t v)
{
    uint8_t s = (v >> 13) & 1;
    if (s != cpu.s) {
        uint32_t sp = cpu.a[7];
        cpu.a[7] = cpu.other_sp;
        cpu.other_sp = sp;
        cpu.s = s;
    }
    cpu.t = (v >> 15) & 1;
    cpu.mask = (v >> 8) & 7;
    set_ccr(v);
}

// Takes exception `vector`, stacking `ret` as the return address; leaves
// cpu.pc at the handler. Implemented in cpu_recomp.cpp.
void exception(int vector, uint32_t ret);

// ---- flags ------------------------------------------------------------------------
inline void nz8(uint32_t r) { cpu.n = (r >> 7) & 1; cpu.z = (r & 0xFF) == 0; }
inline void nz16(uint32_t r) { cpu.n = (r >> 15) & 1; cpu.z = (r & 0xFFFF) == 0; }
inline void nz32(uint32_t r) { cpu.n = r >> 31; cpu.z = r == 0; }
inline void logic8(uint32_t r) { nz8(r); cpu.v = cpu.c = 0; }
inline void logic16(uint32_t r) { nz16(r); cpu.v = cpu.c = 0; }
inline void logic32(uint32_t r) { nz32(r); cpu.v = cpu.c = 0; }

#define JP2_ARITH(bits, mask, sign)                                                             \
    inline uint32_t add##bits(uint32_t s, uint32_t d)                                            \
    {                                                                                            \
        uint64_t r = uint64_t(s & mask) + (d & mask);                                            \
        uint32_t rr = uint32_t(r) & mask;                                                        \
        cpu.x = cpu.c = (r >> bits) & 1;                                                         \
        cpu.v = (((s ^ rr) & (d ^ rr)) & sign) != 0;                                             \
        cpu.n = (rr & sign) != 0;                                                                \
        cpu.z = rr == 0;                                                                         \
        return rr;                                                                               \
    }                                                                                            \
    inline uint32_t sub##bits(uint32_t s, uint32_t d)                                            \
    {                                                                                            \
        uint32_t rr = (d - s) & mask;                                                            \
        cpu.x = cpu.c = (s & mask) > (d & mask);                                                 \
        cpu.v = (((s ^ d) & (rr ^ d)) & sign) != 0;                                              \
        cpu.n = (rr & sign) != 0;                                                                \
        cpu.z = rr == 0;                                                                         \
        return rr;                                                                               \
    }                                                                                            \
    inline void cmp##bits(uint32_t s, uint32_t d)                                                \
    {                                                                                            \
        uint32_t rr = (d - s) & mask;                                                            \
        cpu.c = (s & mask) > (d & mask);                                                         \
        cpu.v = (((s ^ d) & (rr ^ d)) & sign) != 0;                                              \
        cpu.n = (rr & sign) != 0;                                                                \
        cpu.z = rr == 0;                                                                         \
    }                                                                                            \
    inline uint32_t addx##bits(uint32_t s, uint32_t d)                                           \
    {                                                                                            \
        uint64_t r = uint64_t(s & mask) + (d & mask) + cpu.x;                                    \
        uint32_t rr = uint32_t(r) & mask;                                                        \
        cpu.x = cpu.c = (r >> bits) & 1;                                                         \
        cpu.v = (((s ^ rr) & (d ^ rr)) & sign) != 0;                                             \
        cpu.n = (rr & sign) != 0;                                                                \
        if (rr)                                                                                  \
            cpu.z = 0;                                                                           \
        return rr;                                                                               \
    }                                                                                            \
    inline uint32_t subx##bits(uint32_t s, uint32_t d)                                           \
    {                                                                                            \
        uint64_t r = uint64_t(d & mask) - (s & mask) - cpu.x;                                    \
        uint32_t rr = uint32_t(r) & mask;                                                        \
        cpu.x = cpu.c = (r >> bits) & 1;                                                         \
        cpu.v = (((s ^ d) & (rr ^ d)) & sign) != 0;                                              \
        cpu.n = (rr & sign) != 0;                                                                \
        if (rr)                                                                                  \
            cpu.z = 0;                                                                           \
        return rr;                                                                               \
    }                                                                                            \
    inline uint32_t neg##bits(uint32_t d) { return sub##bits(d, 0); }                            \
    inline uint32_t negx##bits(uint32_t d) { return subx##bits(d, 0); }

JP2_ARITH(8, 0xFFu, 0x80u)
JP2_ARITH(16, 0xFFFFu, 0x8000u)
JP2_ARITH(32, 0xFFFFFFFFu, 0x80000000u)
#undef JP2_ARITH

// ---- shifts and rotates (count already reduced to 0..63) --------------------------
#define JP2_SHIFTS(bits, mask, sign)                                                            \
    inline uint32_t asl##bits(uint32_t v, uint32_t n)                                            \
    {                                                                                            \
        v &= mask;                                                                               \
        if (n == 0) {                                                                            \
            cpu.c = 0;                                                                           \
            cpu.v = 0;                                                                           \
            cpu.n = (v & sign) != 0;                                                             \
            cpu.z = v == 0;                                                                      \
            return v;                                                                            \
        }                                                                                        \
        if (n < bits) {                                                                          \
            uint32_t r = uint32_t((uint64_t(v) << n) & mask);                                    \
            cpu.x = cpu.c = (uint64_t(v) >> (bits - n)) & 1;                                     \
            /* V: did the top n+1 bits differ (the sign changed along the way)? */                \
            uint64_t m = ((uint64_t(mask) << (bits - n - 1)) & mask);                            \
            uint64_t s = v & m;                                                                  \
            cpu.v = !(s == 0 || s == m);                                                         \
            cpu.n = (r & sign) != 0;                                                             \
            cpu.z = r == 0;                                                                      \
            return r;                                                                            \
        }                                                                                        \
        cpu.x = cpu.c = n == bits ? (v & 1) : 0;                                                 \
        cpu.n = 0;                                                                               \
        cpu.z = 1;                                                                               \
        cpu.v = v != 0;                                                                          \
        return 0;                                                                                \
    }                                                                                            \
    inline uint32_t asr##bits(uint32_t v, uint32_t n)                                            \
    {                                                                                            \
        v &= mask;                                                                               \
        if (n == 0) {                                                                            \
            cpu.c = 0;                                                                           \
            cpu.v = 0;                                                                           \
            cpu.n = (v & sign) != 0;                                                             \
            cpu.z = v == 0;                                                                      \
            return v;                                                                            \
        }                                                                                        \
        bool neg = v & sign;                                                                     \
        uint32_t r;                                                                              \
        if (n < bits) {                                                                          \
            int64_t sv = neg ? int64_t(v) - (int64_t(mask) + 1) : int64_t(v);                    \
            r = uint32_t(sv >> n) & mask;                                                        \
            cpu.x = cpu.c = (v >> (n - 1)) & 1;                                                  \
        } else {                                                                                 \
            r = neg ? mask : 0;                                                                  \
            cpu.x = cpu.c = neg;                                                                 \
        }                                                                                        \
        cpu.v = 0;                                                                               \
        cpu.n = (r & sign) != 0;                                                                 \
        cpu.z = r == 0;                                                                          \
        return r;                                                                                \
    }                                                                                            \
    inline uint32_t lsl##bits(uint32_t v, uint32_t n)                                            \
    {                                                                                            \
        v &= mask;                                                                               \
        if (n == 0) {                                                                            \
            cpu.c = 0;                                                                           \
            cpu.v = 0;                                                                           \
            cpu.n = (v & sign) != 0;                                                             \
            cpu.z = v == 0;                                                                      \
            return v;                                                                            \
        }                                                                                        \
        uint32_t r = n < bits ? uint32_t((uint64_t(v) << n) & mask) : 0;                         \
        cpu.x = cpu.c = n <= bits ? (uint64_t(v) >> (bits - n)) & 1 : 0;                         \
        cpu.v = 0;                                                                               \
        cpu.n = (r & sign) != 0;                                                                 \
        cpu.z = r == 0;                                                                          \
        return r;                                                                                \
    }                                                                                            \
    inline uint32_t lsr##bits(uint32_t v, uint32_t n)                                            \
    {                                                                                            \
        v &= mask;                                                                               \
        if (n == 0) {                                                                            \
            cpu.c = 0;                                                                           \
            cpu.v = 0;                                                                           \
            cpu.n = (v & sign) != 0;                                                             \
            cpu.z = v == 0;                                                                      \
            return v;                                                                            \
        }                                                                                        \
        uint32_t r = n < bits ? v >> n : 0;                                                      \
        cpu.x = cpu.c = n <= bits ? (v >> (n - 1)) & 1 : 0;                                      \
        cpu.v = 0;                                                                               \
        cpu.n = (r & sign) != 0;                                                                 \
        cpu.z = r == 0;                                                                          \
        return r;                                                                                \
    }                                                                                            \
    inline uint32_t rol##bits(uint32_t v, uint32_t n)                                            \
    {                                                                                            \
        v &= mask;                                                                               \
        uint32_t r = v;                                                                          \
        if (n) {                                                                                 \
            uint32_t k = n % bits;                                                               \
            r = k ? uint32_t(((uint64_t(v) << k) | (v >> (bits - k))) & mask) : v;              \
            cpu.c = r & 1;                                                                       \
        } else {                                                                                 \
            cpu.c = 0;                                                                           \
        }                                                                                        \
        cpu.v = 0;                                                                               \
        cpu.n = (r & sign) != 0;                                                                 \
        cpu.z = r == 0;                                                                          \
        return r;                                                                                \
    }                                                                                            \
    inline uint32_t ror##bits(uint32_t v, uint32_t n)                                            \
    {                                                                                            \
        v &= mask;                                                                               \
        uint32_t r = v;                                                                          \
        if (n) {                                                                                 \
            uint32_t k = n % bits;                                                               \
            r = k ? uint32_t(((v >> k) | (uint64_t(v) << (bits - k))) & mask) : v;              \
            cpu.c = (r & sign) != 0;                                                             \
        } else {                                                                                 \
            cpu.c = 0;                                                                           \
        }                                                                                        \
        cpu.v = 0;                                                                               \
        cpu.n = (r & sign) != 0;                                                                 \
        cpu.z = r == 0;                                                                          \
        return r;                                                                                \
    }                                                                                            \
    inline uint32_t roxl##bits(uint32_t v, uint32_t n)                                           \
    {                                                                                            \
        v &= mask;                                                                               \
        uint32_t r = v;                                                                          \
        if (n) {                                                                                 \
            uint32_t k = n % (bits + 1);                                                         \
            uint64_t w = uint64_t(v) | uint64_t(cpu.x) << bits;                                  \
            uint64_t full = (uint64_t(1) << (bits + 1)) - 1;                                     \
            if (k)                                                                               \
                w = ((w << k) | (w >> (bits + 1 - k))) & full;                                   \
            cpu.x = cpu.c = (w >> bits) & 1;                                                     \
            r = uint32_t(w) & mask;                                                              \
        } else {                                                                                 \
            cpu.c = cpu.x;                                                                       \
        }                                                                                        \
        cpu.v = 0;                                                                               \
        cpu.n = (r & sign) != 0;                                                                 \
        cpu.z = r == 0;                                                                          \
        return r;                                                                                \
    }                                                                                            \
    inline uint32_t roxr##bits(uint32_t v, uint32_t n)                                           \
    {                                                                                            \
        v &= mask;                                                                               \
        uint32_t r = v;                                                                          \
        if (n) {                                                                                 \
            uint32_t k = n % (bits + 1);                                                         \
            uint64_t w = uint64_t(v) | uint64_t(cpu.x) << bits;                                  \
            uint64_t full = (uint64_t(1) << (bits + 1)) - 1;                                     \
            if (k)                                                                               \
                w = ((w >> k) | (w << (bits + 1 - k))) & full;                                   \
            cpu.x = cpu.c = (w >> bits) & 1;                                                     \
            r = uint32_t(w) & mask;                                                              \
        } else {                                                                                 \
            cpu.c = cpu.x;                                                                       \
        }                                                                                        \
        cpu.v = 0;                                                                               \
        cpu.n = (r & sign) != 0;                                                                 \
        cpu.z = r == 0;                                                                          \
        return r;                                                                                \
    }

JP2_SHIFTS(8, 0xFFu, 0x80u)
JP2_SHIFTS(16, 0xFFFFu, 0x8000u)
JP2_SHIFTS(32, 0xFFFFFFFFu, 0x80000000u)
#undef JP2_SHIFTS

// ---- multiply / divide ---------------------------------------------------------------
inline uint32_t mulu(uint32_t s, uint32_t d)
{
    uint32_t r = (s & 0xFFFF) * (d & 0xFFFF);
    logic32(r);
    return r;
}
inline uint32_t muls(uint32_t s, uint32_t d)
{
    uint32_t r = uint32_t(int32_t(int16_t(s)) * int32_t(int16_t(d)));
    logic32(r);
    return r;
}
// Divides only; the caller has already dealt with a zero divisor.
inline uint32_t divu(uint32_t s, uint32_t d)
{
    s &= 0xFFFF;
    uint32_t q = d / s, r = d % s;
    if (q < 0x10000) {
        cpu.z = q == 0;
        cpu.n = (q >> 15) & 1;
        cpu.v = cpu.c = 0;
        return (q & 0xFFFF) | r << 16;
    }
    cpu.v = 1;
    return d;
}
inline uint32_t divs(uint32_t s, uint32_t d)
{
    int32_t src = int16_t(s);
    if (d == 0x80000000u && src == -1) {
        cpu.z = 1;   // Musashi: FLAG_Z = 0 means "zero"
        cpu.n = cpu.v = cpu.c = 0;
        return 0;
    }
    int32_t q = int32_t(d) / src, r = int32_t(d) % src;
    if (q == int16_t(q)) {
        cpu.z = int16_t(q) == 0;
        cpu.n = (uint32_t(q) >> 15) & 1;
        cpu.v = cpu.c = 0;
        return (uint32_t(q) & 0xFFFF) | uint32_t(r) << 16;
    }
    cpu.v = 1;
    return d;
}

// ---- binary-coded decimal (Musashi's handling of the undefined N and V) ------------
inline uint32_t abcd(uint32_t s, uint32_t d)
{
    uint32_t res = (s & 0x0F) + (d & 0x0F) + cpu.x;
    uint32_t v = ~res;
    if (res > 9)
        res += 6;
    res += (s & 0xF0) + (d & 0xF0);
    cpu.x = cpu.c = res > 0x99;
    if (cpu.c)
        res -= 0xA0;
    v &= res;
    cpu.v = (v >> 7) & 1;
    cpu.n = (res >> 7) & 1;
    res &= 0xFF;
    if (res)
        cpu.z = 0;
    return res;
}
inline uint32_t sbcd(uint32_t s, uint32_t d)
{
    uint32_t res = (d & 0x0F) - (s & 0x0F) - cpu.x;
    uint32_t v = ~res;
    if (res > 9)
        res -= 6;
    res += (d & 0xF0) - (s & 0xF0);
    cpu.x = cpu.c = res > 0x99;
    if (cpu.c)
        res += 0xA0;
    res &= 0xFF;
    v &= res;
    cpu.v = (v >> 7) & 1;
    cpu.n = (res >> 7) & 1;
    if (res)
        cpu.z = 0;
    return res;
}
inline uint32_t nbcd(uint32_t d, bool *store)
{
    uint32_t res = (0x9A - d - cpu.x) & 0xFF;
    *store = false;
    if (res != 0x9A) {
        uint32_t v = ~res;
        if ((res & 0x0F) == 0x0A)
            res = (res & 0xF0) + 0x10;
        res &= 0xFF;
        v &= res;
        cpu.v = (v >> 7) & 1;
        if (res)
            cpu.z = 0;
        cpu.c = cpu.x = 1;
        *store = true;
    } else {
        cpu.v = cpu.c = cpu.x = 0;
    }
    cpu.n = (res >> 7) & 1;
    return res;
}

} // namespace jp2
