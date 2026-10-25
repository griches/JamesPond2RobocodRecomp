#include "trace.h"

#include "amiga.h"

#include <cstdio>
#include <string>

namespace trace {

static int armed = -1;
static std::string dir;
static FILE *out;

void arm(int frame, const char *d)
{
    armed = frame;
    dir = d;
}

bool on() { return out != nullptr; }

void frame_begin(int frame)
{
    if (frame == armed)
        out = std::fopen((dir + "/frame.txt").c_str(), "w");
}

void frame_end()
{
    if (!out)
        return;
    std::fclose(out);
    out = nullptr;
    armed = -1;
    if (FILE *f = std::fopen((dir + "/ram.bin").c_str(), "wb")) {
        std::fwrite(amiga::ram, 1, amiga::kRamSize, f);
        std::fclose(f);
    }
    if (FILE *f = std::fopen((dir + "/index.bin").c_str(), "wb")) {
        std::fwrite(amiga::index_buffer, 1, sizeof amiga::index_buffer, f);
        std::fclose(f);
    }
}

static std::string name(uint32_t off)
{
    static const struct { uint32_t off; const char *n; } fixed[] = {
        {0x02E, "COPCON"}, {0x040, "BLTCON0"}, {0x042, "BLTCON1"}, {0x044, "BLTAFWM"}, {0x046, "BLTALWM"},
        {0x048, "BLTCPTH"}, {0x04A, "BLTCPTL"}, {0x04C, "BLTBPTH"}, {0x04E, "BLTBPTL"}, {0x050, "BLTAPTH"},
        {0x052, "BLTAPTL"}, {0x054, "BLTDPTH"}, {0x056, "BLTDPTL"}, {0x058, "BLTSIZE"}, {0x060, "BLTCMOD"},
        {0x062, "BLTBMOD"}, {0x064, "BLTAMOD"}, {0x066, "BLTDMOD"}, {0x070, "BLTCDAT"}, {0x072, "BLTBDAT"},
        {0x074, "BLTADAT"}, {0x080, "COP1LCH"}, {0x082, "COP1LCL"}, {0x084, "COP2LCH"}, {0x086, "COP2LCL"},
        {0x088, "COPJMP1"}, {0x08A, "COPJMP2"}, {0x08E, "DIWSTRT"}, {0x090, "DIWSTOP"}, {0x092, "DDFSTRT"},
        {0x094, "DDFSTOP"}, {0x096, "DMACON"}, {0x098, "CLXCON"}, {0x09A, "INTENA"}, {0x09C, "INTREQ"},
        {0x09E, "ADKCON"}, {0x100, "BPLCON0"}, {0x102, "BPLCON1"}, {0x104, "BPLCON2"}, {0x106, "BPLCON3"},
        {0x108, "BPL1MOD"}, {0x10A, "BPL2MOD"}, {0x1DC, "BEAMCON0"}, {0x1FC, "FMODE"},
    };
    for (auto &e : fixed)
        if (e.off == off)
            return e.n;
    char buf[16];
    if (off >= 0x0A0 && off < 0x0E0)
        std::snprintf(buf, sizeof buf, "AUD%d+%X", (off - 0xA0) >> 4, off & 0xF);
    else if (off >= 0x0E0 && off < 0x100)
        std::snprintf(buf, sizeof buf, "BPL%dPT%c", ((off - 0xE0) >> 2) + 1, off & 2 ? 'L' : 'H');
    else if (off >= 0x120 && off < 0x140)
        std::snprintf(buf, sizeof buf, "SPR%dPT%c", (off - 0x120) >> 2, off & 2 ? 'L' : 'H');
    else if (off >= 0x140 && off < 0x180) {
        static const char *k[] = {"POS", "CTL", "DATA", "DATB"};
        std::snprintf(buf, sizeof buf, "SPR%d%s", (off - 0x140) >> 3, k[(off >> 1) & 3]);
    } else if (off >= 0x180 && off < 0x1C0)
        std::snprintf(buf, sizeof buf, "COLOR%02d", (off - 0x180) >> 1);
    else
        std::snprintf(buf, sizeof buf, "R%03X", off);
    return buf;
}

void reg_write(int line, int hpos, bool copper, uint32_t off, uint16_t v, uint32_t pc)
{
    if (!out)
        return;
    std::fprintf(out, "%3d:%3d %s %-9s %04X", line, hpos, copper ? "cop" : "cpu", name(off).c_str(), v);
    if (!copper)
        std::fprintf(out, "  pc %06X", pc);
    std::fprintf(out, "\n");
}

void blit(int line, uint32_t pc)
{
    if (!out)
        return;
    const uint16_t *r = amiga::regs;
    auto ptr = [&](uint32_t o) { return uint32_t(r[o >> 1]) << 16 | r[(o + 2) >> 1]; };
    uint16_t size = r[0x058 >> 1], con0 = r[0x040 >> 1], con1 = r[0x042 >> 1];
    std::fprintf(out,
                 "%3d     BLIT %s %2dw x %3dh con0 %04X con1 %04X  A %06X B %06X C %06X D %06X  mod A %d B %d C %d D %d"
                 "  fwm %04X lwm %04X  pc %06X\n",
                 line, (con1 & 1) ? "LINE" : "AREA", (size & 63) ? size & 63 : 64, (size >> 6) ? size >> 6 : 1024, con0,
                 con1, ptr(0x050), ptr(0x04C), ptr(0x048), ptr(0x054), int16_t(r[0x064 >> 1]), int16_t(r[0x062 >> 1]),
                 int16_t(r[0x060 >> 1]), int16_t(r[0x066 >> 1]), r[0x044 >> 1], r[0x046 >> 1], pc);
}

} // namespace trace
