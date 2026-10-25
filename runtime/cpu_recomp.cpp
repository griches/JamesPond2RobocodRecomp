// CPU backend: the recompiled code. A small dispatcher picks the translated
// function for cpu.pc, delivers interrupts between calls, and hands anything
// the recompiler didn't see (or memory that no longer matches what it
// translated) to Musashi one instruction at a time.
#include "amiga.h"
#include "cpu.h"
#include "disk.h"
#include "recomp_rt.h"
#include "recomp_tables.h"

extern "C" {
#include "m68k.h"
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace jp2;

namespace amiga {

static HookFn hook_fn;
static uint8_t hook_map[kRamSize >> 1];
static uint8_t *coverage;          // marks instructions that had to be interpreted
static int64_t line_start;
static std::unordered_map<uint32_t, uint64_t> interp_count;

// ---- translated images and page validity ------------------------------------------------
struct Loaded {
    const CodeImage *ci;
    std::vector<uint8_t> bytes;    // what it was translated from
    std::vector<Handler> table;    // by (pc - base) / 2
    std::vector<uint8_t> len;      // instruction length by (pc - base) / 2, 0 = none
};
static std::vector<Loaded> images;

constexpr int kPages = kRamSize >> 8;
constexpr int kMaxCand = 8;
static uint8_t cand_n[kPages];
static uint8_t cand[kPages][kMaxCand];
static uint8_t page_ok[kPages];     // bit k: candidate k matches memory

// Every translated instruction that starts in the page must still have the
// bytes it was translated from.
static bool image_matches_page(const Loaded &im, int page)
{
    uint32_t lo = uint32_t(page) << 8, hi = lo + 256;
    uint32_t b = im.ci->base, e = b + im.ci->size;
    uint32_t from = std::max(lo, b) & ~1u, to = std::min(hi, e);
    for (uint32_t a = from; a < to; a += 2) {
        uint32_t o = a - b;
        uint8_t n = im.len[o >> 1];
        if (n && std::memcmp(&ram[a], &im.bytes[o], n) != 0)
            return false;
    }
    return true;
}

static void revalidate(int page)
{
    page_ok[page] = 0;
    for (int k = 0; k < cand_n[page]; k++)
        if (image_matches_page(images[cand[page][k]], page))
            page_ok[page] |= uint8_t(1 << k);
    page_dirty[page] = 0;
}

static inline Handler lookup(uint32_t pc)
{
    if (pc >= kRamSize)
        return nullptr;
    int page = int(pc >> 8);
    if (page_dirty[page])
        revalidate(page);
    for (int k = 0; k < cand_n[page]; k++) {
        if (!(page_ok[page] & (1 << k)))
            continue;
        const Loaded &im = images[cand[page][k]];
        uint32_t o = pc - im.ci->base;
        if (o < im.ci->size) {
            Handler h = im.table[o >> 1];
            if (h)
                return h;
        }
    }
    return nullptr;
}

bool recomp_init(std::string *note)
{
    images.clear();
    std::memset(cand_n, 0, sizeof cand_n);
    std::memset(code_bits, 0, sizeof code_bits);
    int unusable = 0;
    for (int i = 0; i < code_image_count; i++) {
        const CodeImage &ci = code_images[i];
        Loaded im;
        im.ci = &ci;
        std::string err;
        bool ok = ci.file < 0 ? disk::main_program(im.bytes, &err) : disk::file(ci.file, im.bytes);
        if (!ok || im.bytes.size() != ci.size || disk::crc32(im.bytes.data(), im.bytes.size()) != ci.crc) {
            unusable++;
            continue;
        }
        im.table.assign(ci.size / 2 + 1, nullptr);
        im.len.assign(ci.size / 2 + 1, 0);
        for (size_t e = 0; e < ci.count; e++) {
            const CodeEntry &ce = ci.entries[e];
            uint32_t o = ce.pc - ci.base;
            im.table[o >> 1] = ce.fn;
            im.len[o >> 1] = uint8_t(ce.len);
            for (uint32_t k = 0; k < ce.len; k++) {
                uint32_t a = ce.pc + k;
                code_bits[a >> 3] |= uint8_t(1 << (a & 7));
            }
        }
        int idx = int(images.size());
        images.push_back(std::move(im));
        const Loaded &l = images.back();
        for (size_t e = 0; e < ci.count; e++) {
            int page = int(ci.entries[e].pc >> 8);
            bool have = false;
            for (int k = 0; k < cand_n[page]; k++)
                have |= cand[page][k] == idx;
            if (!have && cand_n[page] < kMaxCand)
                cand[page][cand_n[page]++] = uint8_t(idx);
        }
        (void)l;
    }
    for (int p = 0; p < kPages; p++)
        page_dirty[p] = cand_n[p] ? 1 : 0;
    if (unusable) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "%d of %d translated code images don't match this disk; that code runs interpreted", unusable,
                      code_image_count);
        *note = buf;
    }
    return !images.empty();
}

// ---- the interpreter fallback ------------------------------------------------------------
static void to_musashi()
{
    m68k_set_reg(M68K_REG_SR, get_sr());
    for (int i = 0; i < 8; i++)
        m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), cpu.d[i]);
    for (int i = 0; i < 7; i++)
        m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), cpu.a[i]);
    m68k_set_reg(M68K_REG_A7, cpu.a[7]);
    m68k_set_reg(cpu.s ? M68K_REG_USP : M68K_REG_ISP, cpu.other_sp);
    m68k_set_reg(M68K_REG_PC, cpu.pc);
}

static void from_musashi()
{
    uint32_t sr = m68k_get_reg(nullptr, M68K_REG_SR);
    for (int i = 0; i < 8; i++)
        cpu.d[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
    for (int i = 0; i < 8; i++)
        cpu.a[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_A0 + i));
    cpu.s = (sr >> 13) & 1;
    cpu.t = (sr >> 15) & 1;
    cpu.mask = (sr >> 8) & 7;
    set_ccr(sr);
    cpu.other_sp = m68k_get_reg(nullptr, cpu.s ? M68K_REG_USP : M68K_REG_ISP);
    cpu.pc = m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF;
}

static void interpret_one()
{
    interp_count[cpu.pc]++;
    to_musashi();
    m68k_set_irq(0);
    int used = m68k_execute(1);
    from_musashi();
    cpu.cycles += used;
}

static void take_interrupt(int level)
{
    uint32_t sr = get_sr();
    if (!cpu.s) {
        uint32_t sp = cpu.a[7];
        cpu.a[7] = cpu.other_sp;
        cpu.other_sp = sp;
        cpu.s = 1;
    }
    cpu.t = 0;
    cpu.mask = uint8_t(level);
    push32(cpu.pc);
    cpu.a[7] -= 2;
    wr16(cpu.a[7], sr);
    cpu.pc = rd32(uint32_t(24 + level) * 4) & 0xFFFFFF;
    cpu.stopped = 0;
    cpu.cycles += 44;
}

// ---- CPU backend interface ------------------------------------------------------------------
void cpu_set_hook(uint32_t pc, bool on)
{
    if (pc < kRamSize)
        hook_map[pc >> 1] = on;
}
void cpu_set_hook_handler(HookFn fn) { hook_fn = fn; }
void cpu_enable_coverage(uint8_t *map) { coverage = map; }
uint32_t cpu_instruction_pc() { return cpu.pc; }

uint32_t cpu_reg(Reg r)
{
    if (r <= D7)
        return cpu.d[r];
    if (r <= A7)
        return cpu.a[r - A0];
    if (r == PC)
        return cpu.pc;
    return get_sr();
}

void cpu_set_reg(Reg r, uint32_t v)
{
    if (r <= D7)
        cpu.d[r] = v;
    else if (r <= A7)
        cpu.a[r - A0] = v;
    else if (r == PC)
        cpu.pc = v & 0xFFFFFF;
    else
        set_sr(v);
}

void cpu_return() { cpu.pc = pop32() & 0xFFFFFF; }

void cpu_reset(uint32_t pc, uint32_t sp)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    cpu = Cpu{};
    cpu.s = 1;
    cpu.mask = 7;
    cpu.a[7] = sp;
    cpu.pc = pc;
    std::string note;
    recomp_init(&note);
    if (!note.empty())
        std::fprintf(stderr, "[recomp] %s\n", note.c_str());
}

void cpu_run(int cycles)
{
    line_start = cpu.cycles;
    int64_t end = line_start + cycles;
    while (cpu.cycles < end) {
        cpu.limit = end;
        int level = irq_level();
        if (level > cpu.mask)
            take_interrupt(level);
        if (cpu.stopped) {
            cpu.cycles = end;
            break;
        }
        uint32_t pc = cpu.pc;
        if (pc < kRamSize && hook_map[pc >> 1] && hook_fn) {
            hook_fn(pc);
            if (cpu.pc != pc)
                continue;
        }
        cpu.interp = 0;
        if (Handler h = lookup(pc)) {
            h();
            if (!cpu.interp)
                continue;
        }
        if (coverage && cpu.pc < kRamSize)
            coverage[cpu.pc >> 1] = 1;
        interpret_one();
    }
}

int cpu_cycles_this_line() { return int(cpu.cycles - line_start); }

void cpu_irq_changed() { cpu.limit = cpu.cycles; }

void cpu_report()
{
    if (interp_count.empty())
        return;
    std::vector<std::pair<uint32_t, uint64_t>> v(interp_count.begin(), interp_count.end());
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
    uint64_t total = 0;
    for (auto &e : v)
        total += e.second;
    std::fprintf(stderr, "[recomp] %zu addresses ran interpreted, %llu instructions in all. Most frequent:\n", v.size(),
                 (unsigned long long)total);
    for (size_t i = 0; i < v.size() && i < 12; i++)
        std::fprintf(stderr, "  $%06X  %llu\n", v[i].first, (unsigned long long)v[i].second);
}

} // namespace amiga

extern "C" void jp2_instr_hook(unsigned int) {}
