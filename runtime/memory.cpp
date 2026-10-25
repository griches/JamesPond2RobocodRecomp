// Address decoding for the 68000 side: RAM, custom chips, CIAs.
#include "amiga.h"

#include <cstdio>

namespace amiga {

uint8_t ram[kRamSize];
uint8_t *code_watch;   // set while recording: one byte per word that has run as code
uint8_t code_bits[kRamSize >> 3];
uint8_t page_dirty[kRamSize >> 8];

void loaded(uint32_t addr, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++)
        touch((addr + i) & (kRamSize - 1));
}

static inline void watch(uint32_t a)
{
    if (code_watch && code_watch[a >> 1])
        code_write_seen(a);
}

static void unmapped(const char *what, uint32_t a)
{
    static int reported;
    if (reported < 32) {
        reported++;
        std::fprintf(stderr, "[bus] %s unmapped $%06X\n", what, a);
    }
}

// CIA-A sits on odd addresses $BFE001 + reg*$100, CIA-B on even $BFD000 + reg*$100.
static bool cia_decode(uint32_t a, int *which, int *reg)
{
    if ((a & 0xFFF000) == 0xBFE000 && (a & 1)) {
        *which = 0;
        *reg = (a >> 8) & 0xF;
        return true;
    }
    if ((a & 0xFFF000) == 0xBFD000 && !(a & 1)) {
        *which = 1;
        *reg = (a >> 8) & 0xF;
        return true;
    }
    return false;
}

uint8_t read8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < kRamSize)
        return ram[a];
    if ((a & 0xFFFE00) == 0xDFF000) {
        uint16_t w = custom_read(a & 0x1FE);
        return (a & 1) ? uint8_t(w) : uint8_t(w >> 8);
    }
    int which, reg;
    if (cia_decode(a, &which, &reg))
        return cia_read(which, reg);
    if ((a & 0xFFF000) == 0xBFE000 || (a & 0xFFF000) == 0xBFD000)
        return 0xFF;   // the other CIA's half of the bus
    unmapped("read8", a);
    return 0;
}

uint16_t read16(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < kRamSize - 1)
        return uint16_t(ram[a] << 8 | ram[a + 1]);
    if ((a & 0xFFFE00) == 0xDFF000)
        return custom_read(a & 0x1FE);
    return uint16_t(read8(a) << 8 | read8(a + 1));
}

uint32_t read32(uint32_t a)
{
    return uint32_t(read16(a)) << 16 | read16(a + 2);
}

void write8(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFF;
    if (a < kRamSize) {
        watch(a);
        touch(a);
        ram[a] = v;
        return;
    }
    if ((a & 0xFFFE00) == 0xDFF000) {
        // A byte write puts the same byte on both halves of the data bus.
        custom_write(a & 0x1FE, uint16_t(v << 8 | v));
        return;
    }
    int which, reg;
    if (cia_decode(a, &which, &reg)) {
        cia_write(which, reg, v);
        return;
    }
    if ((a & 0xFFF000) == 0xBFE000 || (a & 0xFFF000) == 0xBFD000)
        return;
    unmapped("write8", a);
}

void write16(uint32_t a, uint16_t v)
{
    a &= 0xFFFFFF;
    if (a < kRamSize - 1) {
        watch(a);
        touch(a);
        touch(a + 1);
        ram[a] = uint8_t(v >> 8);
        ram[a + 1] = uint8_t(v);
        return;
    }
    if ((a & 0xFFFE00) == 0xDFF000) {
        custom_write(a & 0x1FE, v);
        return;
    }
    write8(a, uint8_t(v >> 8));
    write8(a + 1, uint8_t(v));
}

void write32(uint32_t a, uint32_t v)
{
    write16(a, uint16_t(v >> 16));
    write16(a + 2, uint16_t(v));
}

} // namespace amiga
