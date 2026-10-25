// Custom chip registers, interrupts, the copper and the per-line frame loop.
#include "amiga.h"
#include "state.h"

#include <cstdio>
#include <cstring>

namespace amiga {

uint16_t regs[0x100];
Joystick joy;

static uint16_t intena, intreq, adkcon;
static int cur_line;
static bool pal = true;
static bool long_frame = true;

// DMACON lives in regs[] so the display and Paula can read it directly.
static uint16_t &dmacon = regs[0x002 >> 1];

bool blitter_zero;   // set by the blitter, read back through DMACONR

int beam_line() { return cur_line; }
int beam_hpos()
{
    int h = cpu_cycles_this_line() / 2;
    return h < kCcksPerLine ? h : kCcksPerLine - 1;
}
bool pal_mode() { return pal; }
int lines_per_frame() { return pal ? kLinesPal : kLinesNtsc; }

void request_irq(uint16_t bits)
{
    intreq |= bits & 0x3FFF;
    cpu_irq_changed();
}

int irq_level()
{
    if (!(intena & INT_INTEN))
        return 0;
    uint16_t act = intena & intreq & 0x3FFF;
    if (!act)
        return 0;
    if (act & INT_EXTER)
        return 6;
    if (act & (INT_RBF | INT_DSKSYN))
        return 5;
    if (act & 0x0780)
        return 4;
    if (act & (INT_COPER | INT_VERTB | INT_BLIT))
        return 3;
    if (act & INT_PORTS)
        return 2;
    return 1;
}

static uint16_t joy1dat()
{
    int right = joy.right, left = joy.left;
    int down = joy.down, up = joy.up;
    int b1 = right, b9 = left;
    int b0 = b1 ^ down, b8 = b9 ^ up;
    return uint16_t(b9 << 9 | b8 << 8 | b1 << 1 | b0);
}

uint16_t custom_read(uint32_t off)
{
    switch (off) {
    case 0x002:
        return uint16_t((dmacon & 0x07FF) | (blitter_zero ? DMA_BZERO : 0));
    case 0x004: {
        int agnus = pal ? 0x20 : 0x30;   // ECS Agnus, so BEAMCON0 is honoured
        return uint16_t((long_frame ? 0x8000 : 0) | agnus << 8 | ((cur_line >> 8) & 1));
    }
    case 0x006:
        return uint16_t((cur_line & 0xFF) << 8 | beam_hpos());
    case 0x00A:
        return 0;
    case 0x00C:
        return joy1dat();
    case 0x00E:
        return 0x8000;
    case 0x010:
        return adkcon;
    case 0x016: {
        uint16_t v = 0x5500 | (regs[0x034 >> 1] & 0xAA00);
        if (joy.fire2)
            v &= uint16_t(~0x4000);
        return v;
    }
    case 0x018:
        return 0x3000;
    case 0x01C:
        return intena;
    case 0x01E:
        return intreq;
    case 0x07C:
        return 0xFFFF;
    default:
        return 0;
    }
}

static void set_clr(uint16_t &reg, uint16_t v, uint16_t mask)
{
    if (v & 0x8000)
        reg |= v & mask;
    else
        reg &= uint16_t(~(v & mask));
}

void custom_write(uint32_t off, uint16_t v)
{
    if (off >= 0x140 && off < 0x180) {
        regs[off >> 1] = v;
        display_sprite_reg(off, v);
        return;
    }
    if (off >= 0x0A0 && off < 0x0E0) {
        regs[off >> 1] = v;
        paula_reg_write(off, v);
        return;
    }
    switch (off) {
    case 0x020: case 0x022: case 0x024: case 0x026: {
        static bool said;
        if (!said) {
            said = true;
            std::fprintf(stderr, "[custom] disk register $%03X written (loader not hooked?)\n", off);
        }
        regs[off >> 1] = v;
        return;
    }
    case 0x058:
        regs[off >> 1] = v;
        blitter_start();
        return;
    case 0x088:
        copper_jump(1);
        return;
    case 0x08A:
        copper_jump(2);
        return;
    case 0x096: {
        uint16_t old = dmacon;
        set_clr(dmacon, v, 0x1FFF & ~0x6000);
        paula_dma_changed(old, dmacon);
        return;
    }
    case 0x09A:
        set_clr(intena, v, 0x7FFF);
        cpu_irq_changed();
        return;
    case 0x09C:
        set_clr(intreq, v, 0x7FFF);
        cpu_irq_changed();
        return;
    case 0x09E:
        set_clr(adkcon, v, 0x7FFF);
        return;
    case 0x1DC:
        regs[off >> 1] = v;
        pal = (v & 0x20) != 0;
        return;
    default:
        regs[off >> 1] = v;
        return;
    }
}

void custom_reset()
{
    std::memset(regs, 0, sizeof regs);
    intena = intreq = adkcon = 0;
    pal = true;
    regs[0x1DC >> 1] = 0x20;
}

// ---- copper ------------------------------------------------------------------
static uint32_t cop_pc;
static enum { COP_RUN, COP_WAIT, COP_STOP } cop_state = COP_STOP;
static uint16_t cop_w1, cop_w2;   // the pending WAIT
static int cop_h;                 // colour clock the copper has reached on this line

static uint32_t cop_lc(int which)
{
    uint32_t base = which == 1 ? 0x080 : 0x084;
    return uint32_t(regs[base >> 1]) << 16 | regs[(base + 2) >> 1];
}

void copper_jump(int which)
{
    cop_pc = cop_lc(which);
    cop_state = COP_RUN;
}

void copper_frame_start()
{
    cop_pc = cop_lc(1);
    cop_state = COP_RUN;
}

// Beam >= (vp,hp) under the masks of a WAIT/SKIP, at colour clock h.
static bool beam_past(int line, int h, uint16_t w1, uint16_t w2)
{
    int vp = w1 >> 8, hp = w1 & 0xFE;
    int ve = 0x80 | ((w2 >> 8) & 0x7F), he = w2 & 0xFE;
    int cv = line & 0xFF & ve, tv = vp & ve;
    if (cv != tv)
        return cv > tv;
    return (h & he) >= (hp & he);
}

void copper_run_line(int line)
{
    cop_h = 0;
    if (!(dmacon & DMA_EN) || !(dmacon & DMA_COP))
        return;
    int guard = 0;
    while (cop_h < kCcksPerLine && guard++ < 512) {
        if (cop_state == COP_STOP)
            return;
        if (cop_state == COP_WAIT) {
            int h = cop_h;
            while (h < kCcksPerLine && !beam_past(line, h, cop_w1, cop_w2))
                h++;
            if (h >= kCcksPerLine)
                return;
            cop_h = h + 2;
            cop_state = COP_RUN;
            continue;
        }
        uint16_t w1 = chip16(cop_pc), w2 = chip16(cop_pc + 2);
        cop_pc += 4;
        cop_h += 4;
        if (!(w1 & 1)) {
            uint32_t reg = w1 & 0x1FE;
            bool danger = regs[0x02E >> 1] & 2;
            if (reg < 0x40 || (reg < 0x80 && !danger)) {
                cop_state = COP_STOP;
                return;
            }
            display_catch_up(cop_h);
            custom_write(reg, w2);
        } else if (w2 & 1) {
            if (beam_past(line, cop_h, w1, w2))
                cop_pc += 4;   // SKIP
        } else {
            cop_w1 = w1;
            cop_w2 = w2;
            cop_state = COP_WAIT;
        }
    }
}

// ---- the frame loop ----------------------------------------------------------
void reset()
{
    custom_reset();
    cia_reset();
    std::memset(ram, 0, sizeof ram);
    cop_state = COP_STOP;
}

void run_frame()
{
    int lines = lines_per_frame();
    for (int line = 0; line < lines; line++) {
        cur_line = line;
        if (line == 0) {
            copper_frame_start();
            display_frame_start();
            cia_frame();
            request_irq(INT_VERTB);
        }
        display_line_begin(line);
        cpu_run(kCpuCyclesPerLine);
        copper_run_line(line);
        display_line_end();
        paula_run_line();
        cia_run_line();
    }
}

void custom_state(StateIO &s)
{
    s.bytes(regs, sizeof regs);
    s.io(intena);
    s.io(intreq);
    s.io(adkcon);
    s.io(cur_line);
    s.io(pal);
    s.io(long_frame);
    s.io(blitter_zero);
    s.io(cop_pc);
    s.io(cop_state);
    s.io(cop_w1);
    s.io(cop_w2);
    s.io(cop_h);
}

} // namespace amiga
