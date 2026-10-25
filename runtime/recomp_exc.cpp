// Exception entry for the recompiled CPU, laid out the way Musashi (and a
// real 68000) does it: PC then SR on the supervisor stack.
#include "recomp_rt.h"

namespace jp2 {

Cpu cpu;

void exception(int vector, uint32_t ret)
{
    uint32_t sr = get_sr();
    if (!cpu.s) {
        uint32_t sp = cpu.a[7];
        cpu.a[7] = cpu.other_sp;
        cpu.other_sp = sp;
        cpu.s = 1;
    }
    cpu.t = 0;
    push32(ret);
    cpu.a[7] -= 2;
    wr16(cpu.a[7], sr);
    cpu.pc = rd32(uint32_t(vector) * 4);
    cpu.stopped = 0;
    cpu.cycles += 34;
}

} // namespace jp2
