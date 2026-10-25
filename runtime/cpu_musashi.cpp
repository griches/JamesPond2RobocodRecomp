// CPU backend: the Musashi 68000 interpreter. Used to bring the game up and,
// once the recompiled code exists, as the reference it is checked against and
// the fallback for any code the recompiler did not see.
#include "amiga.h"
#include "state.h"
#include "cpu.h"

extern "C" {
#include "m68k.h"
}

#include <cstring>

namespace amiga {

static int line_cycles;   // cycles run on this line before the current slice
static uint8_t hook_map[kRamSize >> 1];
static HookFn hook_fn;
static uint8_t *coverage;  // optional: one byte per instruction word reached

void cpu_set_hook(uint32_t pc, bool on)
{
    if (pc < kRamSize)
        hook_map[pc >> 1] = on;
}

void cpu_set_hook_handler(HookFn fn) { hook_fn = fn; }

void cpu_enable_coverage(uint8_t *map) { coverage = map; }

uint32_t cpu_reg(Reg r)
{
    static const m68k_register_t map[] = {
        M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3, M68K_REG_D4, M68K_REG_D5,
        M68K_REG_D6, M68K_REG_D7, M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
        M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7, M68K_REG_PC, M68K_REG_SR,
    };
    return m68k_get_reg(nullptr, map[r]);
}

void cpu_set_reg(Reg r, uint32_t v)
{
    static const m68k_register_t map[] = {
        M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3, M68K_REG_D4, M68K_REG_D5,
        M68K_REG_D6, M68K_REG_D7, M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
        M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7, M68K_REG_PC, M68K_REG_SR,
    };
    m68k_set_reg(map[r], v);
}

uint32_t cpu_instruction_pc() { return m68k_get_reg(nullptr, M68K_REG_PPC); }

void cpu_return()
{
    uint32_t sp = cpu_reg(A7);
    cpu_set_reg(PC, read32(sp));
    cpu_set_reg(A7, sp + 4);
}

void cpu_reset(uint32_t pc, uint32_t sp)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_A7, sp);
    m68k_set_reg(M68K_REG_PC, pc);
    line_cycles = 0;
}

void cpu_run(int cycles)
{
    line_cycles = 0;
    while (line_cycles < cycles) {
        m68k_set_irq(irq_level());
        line_cycles += m68k_execute(cycles - line_cycles);
    }
}

void cpu_report() {}

int cpu_cycles_this_line() { return line_cycles + m68k_cycles_run(); }

void cpu_irq_changed()
{
    // Musashi only looks at the interrupt line when a slice starts, so end
    // this one; cpu_run starts the next with the new level.
    m68k_set_irq(irq_level());
    m68k_end_timeslice();
}

void cpu_state(StateIO &s)
{
    static const m68k_register_t order[] = {
        M68K_REG_SR, M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3, M68K_REG_D4, M68K_REG_D5, M68K_REG_D6,
        M68K_REG_D7, M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3, M68K_REG_A4, M68K_REG_A5, M68K_REG_A6,
        M68K_REG_A7, M68K_REG_USP, M68K_REG_ISP, M68K_REG_PC,
    };
    for (m68k_register_t r : order) {
        uint32_t v = s.loading() ? 0 : m68k_get_reg(nullptr, r);
        s.io(v);
        if (s.loading() && s.ok())
            m68k_set_reg(r, v);
    }
}

} // namespace amiga

using namespace amiga;

extern "C" {

void jp2_instr_hook(unsigned int pc)
{
    if (pc >= kRamSize)
        return;
    if (coverage)
        coverage[pc >> 1] = 1;
    if (hook_map[pc >> 1] && hook_fn)
        hook_fn(pc);
}

}
