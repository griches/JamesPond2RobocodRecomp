// Recording mode: which code runs, what the loader brings in, and any writes
// into code that has already run. Feeds the recompiler's code discovery.
#include "record.h"

#include "amiga.h"
#include "cpu.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace record {

using namespace amiga;

static bool on;
static std::string out_dir;
static uint8_t coverage[kRamSize >> 1];
static int frame_no;

struct Load {
    int frame, file;
    uint32_t dest, bytes;
    bool packed;
};
static std::vector<Load> loads;

struct CodeWrite {
    uint32_t lo, hi, count;
    int first_frame;
};
static std::map<uint32_t, CodeWrite> code_writes;   // by writing instruction

void start(const char *dir)
{
    on = true;
    out_dir = dir;
    cpu_enable_coverage(coverage);
    code_watch = coverage;
}

bool active() { return on; }

void frame() { frame_no++; }

void load(int file, uint32_t dest, uint32_t bytes, bool packed)
{
    if (on)
        loads.push_back({frame_no, file, dest, bytes, packed});
}

void finish()
{
    if (!on)
        return;
    std::string base = out_dir + "/";
    if (FILE *f = std::fopen((base + "coverage.bin").c_str(), "wb")) {
        std::fwrite(coverage, 1, sizeof coverage, f);
        std::fclose(f);
    }
    if (FILE *f = std::fopen((base + "coverage.txt").c_str(), "w")) {
        size_t words = 0;
        for (uint32_t w = 0; w < (kRamSize >> 1); w++) {
            if (!coverage[w])
                continue;
            uint32_t s = w;
            while (w < (kRamSize >> 1) && coverage[w])
                w++;
            std::fprintf(f, "%06X %06X\n", s * 2, w * 2);
            words += w - s;
        }
        std::fclose(f);
        std::printf("record: %zu instruction words reached\n", words);
    }
    if (FILE *f = std::fopen((base + "loads.txt").c_str(), "w")) {
        for (const Load &l : loads)
            std::fprintf(f, "frame %6d  file %3d  -> $%06X  %6u bytes%s\n", l.frame, l.file, l.dest, l.bytes,
                         l.packed ? "  packed" : "");
        std::fclose(f);
    }
    if (FILE *f = std::fopen((base + "code_writes.txt").c_str(), "w")) {
        for (auto &[pc, w] : code_writes)
            std::fprintf(f, "writer $%06X  wrote $%06X-$%06X  %u times  first frame %d\n", pc, w.lo, w.hi, w.count,
                         w.first_frame);
        std::fclose(f);
        std::printf("record: %zu instructions wrote into code\n", code_writes.size());
    }
}

} // namespace record

namespace amiga {

void code_write_seen(uint32_t addr)
{
    uint32_t pc = cpu_instruction_pc();
    auto it = record::code_writes.find(pc);
    if (it == record::code_writes.end()) {
        record::code_writes[pc] = {addr, addr, 1, record::frame_no};
    } else {
        it->second.lo = addr < it->second.lo ? addr : it->second.lo;
        it->second.hi = addr > it->second.hi ? addr : it->second.hi;
        it->second.count++;
    }
}

} // namespace amiga
