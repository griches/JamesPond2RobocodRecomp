// James Pond 2 recompiler: reads the player's ADF, finds the 68000 code and
// writes it out as C++ for the runtime's recompiled CPU.
//
//   jp2-recomp --adf game.adf --out dir [--coverage cov.bin ...] [--seed hex ...]
//              [--test-out tests.cpp]
//
// Code is found by following control flow from the entry point, the music
// player's jump table and every address the interpreter was seen running
// (--coverage, written by `jp2 --record`). Each run of contiguous code becomes
// one function with an entry label per instruction, so the dispatcher can
// start anywhere a jump, return or interrupt lands.
#include "disk.h"
#include "translate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "m68k.h"
extern unsigned char m68ki_cycles[][0x10000];
}

namespace {

struct Image {
    int id;
    std::string name;      // "main" or "file120"...
    uint32_t base;
    std::vector<uint8_t> bytes;
    std::map<uint32_t, Insn> code;   // by pc
    std::vector<uint32_t> work;

    bool contains(uint32_t a) const { return a >= base && a + 2 <= base + bytes.size(); }
    uint16_t word(uint32_t a) const
    {
        if (!contains(a))
            return 0;
        size_t o = a - base;
        return uint16_t(bytes[o] << 8 | bytes[o + 1]);
    }
};

std::vector<Image> images;
const Image *dasm_image;

// Volatile ranges are rewritten while the game runs (the music player), so
// no chunk may run across their edges.
std::vector<std::pair<uint32_t, uint32_t>> volatile_ranges;

// Addresses the runtime hooks (the loader): always reached through the
// dispatcher, never by a goto or by running on into them.
std::set<uint32_t> breaks;

int volatile_region(uint32_t a)
{
    for (size_t i = 0; i < volatile_ranges.size(); i++)
        if (a >= volatile_ranges[i].first && a < volatile_ranges[i].second)
            return int(i) + 1;
    return 0;
}

std::string disasm(const Image &img, uint32_t pc, unsigned *len)
{
    dasm_image = &img;
    char buf[256];
    *len = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
    return buf;
}

void seed(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a & 1)
        return;
    for (Image &img : images)
        if (img.contains(a))
            img.work.push_back(a);
}

void trace(Image &img)
{
    while (!img.work.empty()) {
        uint32_t pc = img.work.back();
        img.work.pop_back();
        while (img.contains(pc) && !img.code.count(pc)) {
            Insn in = translate(pc, [&](uint32_t a) { return img.word(a); });
            if (in.flow == Insn::INTERP) {
                unsigned len;
                disasm(img, pc, &len);
                in.len = len ? len : 2;
            }
            if (!img.contains(pc + in.len - 2))
                break;   // runs off the end of the image: not code
            img.code[pc] = in;
            uint32_t next = pc + in.len;
            switch (in.flow) {
            case Insn::JUMP:
                seed(in.target);
                if (in.call)
                    seed(next);
                pc = 0xFFFFFFFF;
                break;
            case Insn::BRANCH:
                seed(in.target);
                pc = next;
                break;
            case Insn::DYNAMIC:
                if (in.call)
                    seed(next);
                pc = 0xFFFFFFFF;
                break;
            case Insn::EXCEPT:
                if ((img.word(pc) & 0xFFF0) == 0x4E40)
                    seed(next);   // TRAP comes back with RTE
                pc = 0xFFFFFFFF;
                break;
            default:
                pc = next;
                break;
            }
            if (pc == 0xFFFFFFFF)
                break;
        }
    }
}

struct Chunk {
    const Image *img;
    std::vector<uint32_t> pcs;
    std::string fn;
};

std::vector<Chunk> make_chunks(const Image &img, size_t max_insns)
{
    std::vector<Chunk> out;
    Chunk cur;
    uint32_t expect = 0;
    int region = -1;
    for (auto &[pc, in] : img.code) {
        int r = volatile_region(pc);
        bool cont = !cur.pcs.empty() && pc == expect && r == region && cur.pcs.size() < max_insns && !breaks.count(pc);
        if (!cont && !cur.pcs.empty()) {
            out.push_back(cur);
            cur = Chunk{};
        }
        if (cur.pcs.empty()) {
            cur.img = &img;
            char name[64];
            std::snprintf(name, sizeof name, "c%d_%06X", img.id, pc);
            cur.fn = name;
        }
        cur.pcs.push_back(pc);
        expect = pc + in.len;
        region = r;
    }
    if (!cur.pcs.empty())
        out.push_back(cur);
    return out;
}

std::string transfer(const std::set<uint32_t> &local, uint32_t from, uint32_t to)
{
    char buf[160];
    if (local.count(to)) {
        if (to <= from)
            std::snprintf(buf, sizeof buf,
                          "{ if (cpu.cycles >= cpu.limit) { cpu.pc = 0x%06Xu; return; } goto L_%06X; }", to, to);
        else
            std::snprintf(buf, sizeof buf, "goto L_%06X;", to);
    } else {
        std::snprintf(buf, sizeof buf, "{ cpu.pc = 0x%06Xu; return; }", to);
    }
    return buf;
}

void emit_insn(FILE *f, const Image &img, const Insn &in, const std::set<uint32_t> &local, bool last_in_chunk)
{
    unsigned len;
    std::string text = disasm(img, in.pc, &len);
    uint32_t next = in.pc + in.len;
    int cyc = m68ki_cycles[0][img.word(in.pc)];
    std::fprintf(f, "L_%06X: // %s\n{\n    cpu.cycles += %d;\n", in.pc, text.c_str(), cyc);
    if (in.flow == Insn::INTERP) {
        std::fprintf(f, "    cpu.pc = 0x%06Xu; cpu.interp = 1; return;\n}\n", in.pc);
        return;
    }
    // indent the body
    size_t p = 0;
    while (p < in.body.size()) {
        size_t e = in.body.find('\n', p);
        if (e == std::string::npos)
            e = in.body.size();
        std::fprintf(f, "    %.*s\n", int(e - p), in.body.c_str() + p);
        p = e + 1;
    }
    std::string fall = local.count(next) ? "" : "{ cpu.pc = 0x" + [&] {
        char b[16];
        std::snprintf(b, sizeof b, "%06X", next);
        return std::string(b);
    }() + "u; return; }";
    switch (in.flow) {
    case Insn::FALL:
        if (!fall.empty())
            std::fprintf(f, "    %s\n", fall.c_str());
        break;
    case Insn::SYNC:
        std::fprintf(f, "    cpu.pc = 0x%06Xu; return;\n", next);
        break;
    case Insn::JUMP:
        std::fprintf(f, "    %s\n", transfer(local, in.pc, in.target).c_str());
        break;
    case Insn::BRANCH:
        std::fprintf(f, "    if (%s) %s\n", in.cond.c_str(), transfer(local, in.pc, in.target).c_str());
        if (!fall.empty())
            std::fprintf(f, "    %s\n", fall.c_str());
        break;
    case Insn::DYNAMIC:
        std::fprintf(f, "    cpu.pc = (%s) & 0xFFFFFFu; return;\n", in.dyn.c_str());
        break;
    case Insn::EXCEPT:
        std::fprintf(f, "    return;\n");
        break;
    default:
        break;
    }
    (void)last_in_chunk;
    std::fprintf(f, "}\n");
}

void write_chunks(const std::string &dir, const std::vector<Chunk> &chunks, int files)
{
    // Always exactly `files` outputs, so the build knows their names up front.
    size_t per_file = (chunks.size() + files - 1) / files;
    if (!per_file)
        per_file = 1;
    for (int file_no = 0; file_no < files; file_no++) {
        size_t i = file_no * per_file;
        char path[512];
        std::snprintf(path, sizeof path, "%s/code_%d.cpp", dir.c_str(), file_no);
        FILE *f = std::fopen(path, "w");
        std::fprintf(f, "// Generated by jp2-recomp from the player's disk. Do not edit or distribute.\n");
        std::fprintf(f, "#include \"recomp_rt.h\"\n\nnamespace jp2 {\n\n");
        for (size_t c = i; c < chunks.size() && c < i + per_file; c++) {
            const Chunk &ch = chunks[c];
            std::set<uint32_t> local;
            for (uint32_t pc : ch.pcs)
                if (!breaks.count(pc))
                    local.insert(pc);
            std::fprintf(f, "void %s()\n{\n    switch (cpu.pc) {\n", ch.fn.c_str());
            for (uint32_t pc : ch.pcs)
                std::fprintf(f, "    case 0x%06X: goto L_%06X;\n", pc, pc);
            std::fprintf(f, "    default: cpu.interp = 1; return;\n    }\n");
            for (size_t k = 0; k < ch.pcs.size(); k++)
                emit_insn(f, *ch.img, ch.img->code.at(ch.pcs[k]), local, k + 1 == ch.pcs.size());
            std::fprintf(f, "}\n\n");
        }
        std::fprintf(f, "} // namespace jp2\n");
        std::fclose(f);
    }
}

void write_tables(const std::string &dir, const std::vector<Chunk> &chunks)
{
    FILE *f = std::fopen((dir + "/tables.cpp").c_str(), "w");
    std::fprintf(f, "// Generated by jp2-recomp. Do not edit or distribute.\n");
    std::fprintf(f, "#include \"recomp_tables.h\"\n\nnamespace jp2 {\n\n");
    for (const Chunk &ch : chunks)
        std::fprintf(f, "void %s();\n", ch.fn.c_str());
    for (const Image &img : images) {
        std::fprintf(f, "\nstatic const CodeEntry entries_%d[] = {\n", img.id);
        for (const Chunk &ch : chunks) {
            if (ch.img != &img)
                continue;
            for (uint32_t pc : ch.pcs)
                std::fprintf(f, "    {0x%06Xu, %u, %s},\n", pc, img.code.at(pc).len, ch.fn.c_str());
        }
        std::fprintf(f, "};\n");
    }
    std::fprintf(f, "\nconst CodeImage code_images[] = {\n");
    for (const Image &img : images) {
        int file = img.name == "main" ? -1 : std::atoi(img.name.c_str() + 4);
        std::fprintf(f, "    {%d, 0x%06Xu, %zuu, 0x%08Xu, entries_%d, sizeof entries_%d / sizeof entries_%d[0]},\n", file,
                     img.base, img.bytes.size(), disk::crc32(img.bytes.data(), img.bytes.size()), img.id, img.id, img.id);
    }
    std::fprintf(f, "};\nconst int code_image_count = %zu;\n\n} // namespace jp2\n", images.size());
    std::fclose(f);
}

// One function per instruction, every exit going back to the caller: used by
// the differential test against Musashi.
void write_tests(const std::string &path, size_t limit)
{
    FILE *f = std::fopen(path.c_str(), "w");
    std::fprintf(f, "// Generated by jp2-recomp --test-out. Do not edit or distribute.\n");
    std::fprintf(f, "#include \"recomp_rt.h\"\n#include \"recomp_tables.h\"\n\nnamespace jp2 {\n\n");
    std::vector<std::pair<int, uint32_t>> all;
    for (const Image &img : images)
        for (auto &[pc, in] : img.code)
            if (in.flow != Insn::INTERP)
                all.push_back({img.id, pc});
    if (all.size() > limit)
        all.resize(limit);
    std::set<uint32_t> none;
    for (auto &[id, pc] : all) {
        const Image &img = images[id];
        std::fprintf(f, "static void t%d_%06X()\n{\n", id, pc);
        emit_insn(f, img, img.code.at(pc), none, true);
        std::fprintf(f, "}\n");
    }
    std::fprintf(f, "\nconst TestEntry tests[] = {\n");
    for (auto &[id, pc] : all)
        std::fprintf(f, "    {%d, 0x%06Xu, t%d_%06X},\n", id, pc, id, pc);
    std::fprintf(f, "};\nconst int test_count = %zu;\n\n} // namespace jp2\n", all.size());
    std::fclose(f);
}

} // namespace

extern "C" {
unsigned int m68k_read_disassembler_8(unsigned int a)
{
    const Image &i = *dasm_image;
    return i.contains(a & ~1u) ? i.bytes[a - i.base] : 0;
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return dasm_image->word(a); }
unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return uint32_t(dasm_image->word(a)) << 16 | dasm_image->word(a + 2);
}
// Musashi wants these linked even though nothing is executed here.
unsigned int m68k_read_memory_8(unsigned int) { return 0; }
unsigned int m68k_read_memory_16(unsigned int) { return 0; }
unsigned int m68k_read_memory_32(unsigned int) { return 0; }
void m68k_write_memory_8(unsigned int, unsigned int) {}
void m68k_write_memory_16(unsigned int, unsigned int) {}
void m68k_write_memory_32(unsigned int, unsigned int) {}
void jp2_instr_hook(unsigned int) {}
}

int main(int argc, char **argv)
{
    const char *adf = nullptr;
    std::string out, test_out;
    std::vector<std::string> coverage;
    std::vector<uint32_t> seeds;
    int files = 32;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--adf") && i + 1 < argc)
            adf = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--coverage") && i + 1 < argc)
            coverage.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            seeds.push_back(uint32_t(std::strtoul(argv[++i], nullptr, 16)));
        else if (!std::strcmp(argv[i], "--test-out") && i + 1 < argc)
            test_out = argv[++i];
        else if (!std::strcmp(argv[i], "--break") && i + 1 < argc)
            breaks.insert(uint32_t(std::strtoul(argv[++i], nullptr, 16)));
        else if (!std::strcmp(argv[i], "--files") && i + 1 < argc)
            files = std::atoi(argv[++i]);
    }
    if (!adf || out.empty()) {
        std::fprintf(stderr, "usage: %s --adf game.adf --out dir [--coverage cov.bin] [--seed hex] [--test-out f]\n",
                     argv[0]);
        return 1;
    }
    std::string err;
    std::vector<uint8_t> main_prog;
    if (!disk::open(adf, &err) || !disk::main_program(main_prog, &err)) {
        std::fprintf(stderr, "%s: %s\n", adf, err.c_str());
        return 1;
    }
    m68k_init();

    images.push_back({0, "main", disk::kImageBase, main_prog, {}, {}});
    uint32_t music_end = disk::kMusicBase;
    for (int n = disk::kMusicFirst; n <= disk::kMusicLast; n++) {
        Image img{int(images.size()), "file" + std::to_string(n), disk::kMusicBase, {}, {}, {}};
        if (!disk::file(n, img.bytes)) {
            std::fprintf(stderr, "missing music file %d\n", n);
            return 1;
        }
        music_end = std::max<uint32_t>(music_end, disk::kMusicBase + uint32_t(img.bytes.size()));
        images.push_back(img);
    }
    volatile_ranges.push_back({disk::kMusicBase, music_end});

    seed(disk::kEntry);
    for (uint32_t s : seeds)
        seed(s);
    for (int e = 0; e < 5; e++)   // music player jump table
        seed(disk::kMusicBase + e * 6);
    for (const std::string &path : coverage) {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "can't read %s\n", path.c_str());
            return 1;
        }
        std::vector<uint8_t> cov(0x80000);
        size_t n = std::fread(cov.data(), 1, cov.size(), f);
        std::fclose(f);
        for (size_t w = 0; w < n; w++)
            if (cov[w])
                seed(uint32_t(w * 2));
    }
    size_t total = 0, interp = 0;
    std::vector<Chunk> chunks;
    for (Image &img : images) {
        trace(img);
        for (auto &[pc, in] : img.code)
            interp += in.flow == Insn::INTERP;
        total += img.code.size();
        std::vector<Chunk> c = make_chunks(img, 400);
        chunks.insert(chunks.end(), c.begin(), c.end());
        std::printf("%-8s $%06X  %6zu instructions\n", img.name.c_str(), img.base, img.code.size());
    }
    std::printf("total %zu instructions in %zu functions, %zu left to the interpreter\n", total, chunks.size(), interp);
    write_chunks(out, chunks, files);
    write_tables(out, chunks);
    if (!test_out.empty())
        write_tests(test_out, 400000);
    return 0;
}
