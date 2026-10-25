// What the game layer needs from the CPU backend, whichever one is in use.
#pragma once
#include <cstdint>

namespace amiga {

enum Reg { D0, D1, D2, D3, D4, D5, D6, D7, A0, A1, A2, A3, A4, A5, A6, A7, PC, SR };

using HookFn = void (*)(uint32_t pc);

void cpu_reset(uint32_t pc, uint32_t sp);
uint32_t cpu_reg(Reg r);
void cpu_set_reg(Reg r, uint32_t v);
void cpu_return();                         // pop the return address (rts)
void cpu_set_hook(uint32_t pc, bool on);   // call the handler before running pc
void cpu_set_hook_handler(HookFn fn);
void cpu_enable_coverage(uint8_t *map);    // kRamSize/2 bytes, or nullptr
uint32_t cpu_instruction_pc();             // start of the instruction running now
void cpu_report();                         // print backend statistics at exit

} // namespace amiga
