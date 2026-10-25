// James Pond 2 specifics: put the program in memory the way the disk's own
// loader would, and stand in for the floppy.
//
// Supported image: the standard-format ADF of the Skid Row crack (also sold as
// the Kixx budget release). Its main program, once unpacked, is the game at
// $500 with a 518-byte crack header in front, loaded at $300.
#include "game.h"

#include "amiga.h"
#include "cpu.h"
#include "disk.h"
#include "record.h"

#include <cstring>
#include <vector>

namespace game {

using namespace amiga;

// Addresses in the crack's program image.
constexpr uint32_t kReadSectors = 0x16E00;   // d0.w block, d1.l bytes, a0 dest
constexpr uint32_t kLoadPacked = 0x16D66;    // d0 file, a0 dest (unpacked via $6F140)
constexpr uint32_t kLoadRaw = 0x16D9A;       // d0 file, a0 dest
constexpr uint32_t kCheatFlag = 0x2552;      // built-in cheat keys (WHDLoad trainer)

static void hook(uint32_t pc)
{
    const std::vector<uint8_t> &adf = disk::adf();
    if (pc == kLoadPacked || pc == kLoadRaw) {
        int file = int(cpu_reg(D0) & 0xFFFF);
        bool packed = pc == kLoadPacked;
        std::vector<uint8_t> raw;
        uint32_t bytes = packed ? disk::unpacked_size(file) : (disk::file(file, raw) ? uint32_t(raw.size()) : 0);
        record::load(file, cpu_reg(A0), bytes, packed);
        return;
    }
    if (pc == kReadSectors) {
        uint32_t block = uint32_t(int32_t(int16_t(cpu_reg(D0))));
        uint32_t bytes = cpu_reg(D1), dest = cpu_reg(A0);
        uint32_t from = block * 512;
        for (uint32_t i = 0; i < bytes; i++)
            ram[(dest + i) & (kRamSize - 1)] = from + i < adf.size() ? adf[from + i] : 0;
        loaded(dest, bytes);
        cpu_set_reg(A6, 0xDFF000);
        cpu_set_reg(A4, 0xBFD000);
        cpu_set_reg(D4, 0);
        cpu_return();
    }
}

bool boot(const char *adf_path, std::string *error)
{
    if (!disk::open(adf_path, error))
        return false;
    std::vector<uint8_t> image;
    if (!disk::main_program(image, error))
        return false;

    reset();
    std::memcpy(&ram[disk::kImageBase], image.data(), image.size());

    // Memory list the game reads at $200: chip memory, then the extra block.
    write32(0x200, 0);
    write32(0x204, kChipSize);
    write32(0x208, kExtBase);
    write32(0x20C, kExtSize);

    cpu_set_hook_handler(hook);
    cpu_set_hook(kReadSectors, true);
    cpu_set_hook(kLoadPacked, true);
    cpu_set_hook(kLoadRaw, true);
    cpu_reset(disk::kEntry, 0x300);
    return true;
}

void enable_cheat() { ram[kCheatFlag] = 0xFF; }

} // namespace game
