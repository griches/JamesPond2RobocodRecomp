// Differential test: every translated instruction against Musashi.
//
//   jp2-difftest --adf game.adf [--trials N] [--show N]
//
// Each instruction runs from the same random register, flag and memory state
// in both, then registers, SR, PC and every byte written are compared. The
// bus here is plain memory (no custom chips), with an undo log so both runs
// start from identical memory.
#include "disk.h"
#include "recomp_rt.h"
#include "recomp_tables.h"

extern "C" {
#include "m68k.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

// ---- test bus: flat 16 MB, logged writes --------------------------------------------
namespace amiga {
uint8_t ram[kRamSize];
uint8_t code_bits[kRamSize >> 3];
uint8_t page_dirty[kRamSize >> 8];
}

namespace {

std::vector<uint8_t> high(0x1000000 - amiga::kRamSize);

struct Write {
    uint32_t addr;
    uint8_t old, now;
};
std::vector<Write> log_;

uint8_t &cell(uint32_t a)
{
    a &= 0xFFFFFF;
    return a < amiga::kRamSize ? amiga::ram[a] : high[a - amiga::kRamSize];
}

void put(uint32_t a, uint8_t v)
{
    uint8_t &c = cell(a);
    log_.push_back({a & 0xFFFFFF, c, v});
    c = v;
}

void undo()
{
    for (size_t i = log_.size(); i-- > 0;)
        cell(log_[i].addr) = log_[i].old;
    log_.clear();
}

} // namespace

namespace amiga {
uint8_t read8(uint32_t a) { return cell(a); }
uint16_t read16(uint32_t a) { return uint16_t(cell(a) << 8 | cell(a + 1)); }
uint32_t read32(uint32_t a) { return uint32_t(read16(a)) << 16 | read16(a + 2); }
void write8(uint32_t a, uint8_t v) { put(a, v); }
void write16(uint32_t a, uint16_t v)
{
    put(a, uint8_t(v >> 8));
    put(a + 1, uint8_t(v));
}
void write32(uint32_t a, uint32_t v)
{
    write16(a, uint16_t(v >> 16));
    write16(a + 2, uint16_t(v));
}
} // namespace amiga

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return amiga::read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return amiga::read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return amiga::read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { amiga::write8(a, uint8_t(v)); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { amiga::write16(a, uint16_t(v)); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { amiga::write32(a, v); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return amiga::read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return amiga::read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return amiga::read32(a); }
void jp2_instr_hook(unsigned int) {}
}

using jp2::cpu;

namespace {

struct State {
    uint32_t d[8], a[8], other_sp, pc, sr;
    bool stopped;
};

State capture_recomp()
{
    State s{};
    std::memcpy(s.d, cpu.d, sizeof s.d);
    std::memcpy(s.a, cpu.a, sizeof s.a);
    s.other_sp = cpu.other_sp;
    s.pc = cpu.pc & 0xFFFFFF;
    s.sr = jp2::get_sr();
    s.stopped = cpu.stopped;
    return s;
}

void load_recomp(const State &s)
{
    cpu = jp2::Cpu{};
    std::memcpy(cpu.d, s.d, sizeof s.d);
    std::memcpy(cpu.a, s.a, sizeof s.a);
    cpu.other_sp = s.other_sp;
    cpu.pc = s.pc;
    cpu.s = (s.sr >> 13) & 1;
    cpu.t = (s.sr >> 15) & 1;
    cpu.mask = (s.sr >> 8) & 7;
    jp2::set_ccr(s.sr);
    cpu.limit = INT64_MAX;
}

State run_musashi(const State &in, bool reset)
{
    if (reset) {
        m68k_pulse_reset();
        m68k_execute(1);
    }
    m68k_set_reg(M68K_REG_SR, in.sr);
    for (int i = 0; i < 8; i++)
        m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), in.d[i]);
    for (int i = 0; i < 7; i++)
        m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), in.a[i]);
    m68k_set_reg(M68K_REG_A7, in.a[7]);
    bool s = (in.sr >> 13) & 1;
    m68k_set_reg(s ? M68K_REG_USP : M68K_REG_ISP, in.other_sp);
    m68k_set_reg(M68K_REG_PC, in.pc);
    m68k_set_irq(0);
    m68k_execute(1);
    State out{};
    out.sr = m68k_get_reg(nullptr, M68K_REG_SR);
    for (int i = 0; i < 8; i++)
        out.d[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
    for (int i = 0; i < 8; i++)
        out.a[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_A0 + i));
    out.other_sp = m68k_get_reg(nullptr, (out.sr & 0x2000) ? M68K_REG_USP : M68K_REG_ISP);
    out.pc = m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF;
    return out;
}

std::map<uint32_t, uint8_t> final_writes(const std::vector<Write> &w)
{
    std::map<uint32_t, uint8_t> m;
    for (const Write &x : w)
        m[x.addr] = x.now;
    return m;
}

std::string diff(const State &m, const State &r, const std::map<uint32_t, uint8_t> &wm,
                 const std::map<uint32_t, uint8_t> &wr)
{
    std::string out;
    char buf[128];
    for (int i = 0; i < 8; i++)
        if (m.d[i] != r.d[i]) {
            std::snprintf(buf, sizeof buf, " D%d %08X/%08X", i, m.d[i], r.d[i]);
            out += buf;
        }
    for (int i = 0; i < 8; i++)
        if (m.a[i] != r.a[i]) {
            std::snprintf(buf, sizeof buf, " A%d %08X/%08X", i, m.a[i], r.a[i]);
            out += buf;
        }
    if (m.other_sp != r.other_sp) {
        std::snprintf(buf, sizeof buf, " otherSP %08X/%08X", m.other_sp, r.other_sp);
        out += buf;
    }
    if ((m.sr & 0xA71F) != (r.sr & 0xA71F)) {
        std::snprintf(buf, sizeof buf, " SR %04X/%04X", m.sr & 0xA71F, r.sr & 0xA71F);
        out += buf;
    }
    if (m.pc != r.pc) {
        std::snprintf(buf, sizeof buf, " PC %06X/%06X", m.pc, r.pc);
        out += buf;
    }
    if (wm != wr) {
        std::snprintf(buf, sizeof buf, " writes %zu/%zu", wm.size(), wr.size());
        out += buf;
        for (auto &[a, v] : wm) {
            auto it = wr.find(a);
            if (it == wr.end() || it->second != v) {
                std::snprintf(buf, sizeof buf, " [%06X %02X/%s]", a, v,
                              it == wr.end() ? "--" : std::to_string(it->second).c_str());
                out += buf;
                break;
            }
        }
    }
    return out;
}

uint32_t pick_data(std::mt19937 &rng)
{
    static const uint32_t special[] = {0, 1, 2, 7, 8, 15, 16, 31, 32, 63, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000,
                                       0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFF8000u, 0x99, 0x09};
    switch (rng() % 4) {
    case 0: return special[rng() % (sizeof special / sizeof special[0])];
    case 1: return rng() & 0xFF;
    case 2: return rng() & 0xFFFF;
    default: return rng();
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *adf = nullptr;
    int trials = 24, show = 40;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--adf") && i + 1 < argc)
            adf = argv[++i];
        else if (!std::strcmp(argv[i], "--trials") && i + 1 < argc)
            trials = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--show") && i + 1 < argc)
            show = std::atoi(argv[++i]);
    }
    std::string err;
    std::vector<uint8_t> main_prog;
    if (!adf || !disk::open(adf, &err) || !disk::main_program(main_prog, &err)) {
        std::fprintf(stderr, "usage: %s --adf game.adf  (%s)\n", argv[0], err.c_str());
        return 1;
    }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_execute(1);   // the first slice after a reset is spent on the reset itself

    std::mt19937 rng(1234);
    for (uint32_t a = 0; a < amiga::kRamSize; a++)
        amiga::ram[a] = uint8_t(rng());
    for (auto &b : high)
        b = uint8_t(rng());

    int current = -2;
    long checked = 0, failed_insns = 0;
    std::map<uint16_t, int> fail_by_opcode;
    int shown = 0;
    for (int t = 0; t < jp2::test_count; t++) {
        const jp2::TestEntry &te = jp2::tests[t];
        if (te.image != current) {
            current = te.image;
            const jp2::CodeImage &ci = jp2::code_images[te.image];
            std::vector<uint8_t> bytes;
            if (ci.file < 0)
                bytes = main_prog;
            else
                disk::file(ci.file, bytes);
            std::memcpy(&amiga::ram[ci.base], bytes.data(), bytes.size());
        }
        uint16_t op = amiga::read16(te.pc);
        bool bad = false;
        for (int k = 0; k < trials && !bad; k++) {
            State in{};
            for (int i = 0; i < 8; i++)
                in.d[i] = pick_data(rng);
            for (int i = 0; i < 8; i++)
                in.a[i] = (0x1000 + rng() % 0xE0000) & ~1u;
            if (rng() % 8 == 0)
                in.a[rng() % 8] = pick_data(rng);
            in.other_sp = (0x1000 + rng() % 0xE0000) & ~1u;
            in.pc = te.pc;
            in.sr = (rng() % 16 ? 0x2000 : 0) | (rng() % 8) << 8 | (rng() & 0x1F);

            State m = run_musashi(in, op == 0x4E72);
            std::vector<Write> wm = log_;
            undo();
            load_recomp(in);
            te.fn();
            State r = capture_recomp();
            std::vector<Write> wr = log_;
            undo();
            checked++;
            std::string d = diff(m, r, final_writes(wm), final_writes(wr));
            if (!d.empty()) {
                bad = true;
                failed_insns++;
                fail_by_opcode[op & 0xF1C0]++;
                if (shown++ < show) {
                    char text[128];
                    m68k_disassemble(text, te.pc, M68K_CPU_TYPE_68000);
                    std::printf("$%06X %04X %-28s in: D=%08X,%08X A=%08X SR=%04X  musashi/recomp:%s\n", te.pc, op, text,
                                in.d[0], in.d[1], in.a[0], in.sr, d.c_str());
                }
            }
        }
    }
    std::printf("\n%d instructions, %ld runs, %ld instructions differ\n", jp2::test_count, checked, failed_insns);
    if (!fail_by_opcode.empty()) {
        std::printf("failures by opcode group (op & F1C0):");
        for (auto &[op, n] : fail_by_opcode)
            std::printf(" %04X:%d", op, n);
        std::printf("\n");
    }
    return failed_insns ? 2 : 0;
}
