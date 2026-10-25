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
#include "wide.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace game {

using namespace amiga;

// Addresses in the crack's program image.
constexpr uint32_t kReadSectors = 0x16E00;   // d0.w block, d1.l bytes, a0 dest
constexpr uint32_t kLoadPacked = 0x16D66;    // d0 file, a0 dest (unpacked via $6F140)
constexpr uint32_t kLoadRaw = 0x16D9A;       // d0 file, a0 dest
constexpr uint32_t kCheatFlag = 0x2552;      // built-in cheat keys (WHDLoad trainer)
constexpr uint32_t kMakeDrawRecord = 0x13DC6; // a2 frame definition, a1 base, position at $1426
// Items: a whole 16x16 image, no mask. $152B0 points a1 at level tile d0 and
// runs on into $152C2; item code calls $152C2 directly with its own image.
constexpr uint32_t kMakeTileRecord = 0x152C2; // a1 image, position at $1426
constexpr uint32_t kMakeDrawRecord2 = 0x4BA24; // a copy of kMakeDrawRecord the item code uses
constexpr uint32_t kObjectInView = 0x1490C;    // a0 object: on screen? (some static objects)
constexpr uint32_t kCreatureInView = 0x11B18;  // inside the $1440 objects' update: set $1422 if on screen
constexpr uint32_t kCreatureInViewDone = 0x11BB0; // the rts the visible path branches to

// The $1440 objects (enemies, padlocks, ...) only draw themselves when this
// test, part way through their update, finds them inside the game's
// 304x208 view. In widescreen, one that is only outside it but inside the
// wider view takes the visible path ($1422 = 1, on to the rts at $11BB0).
// Everything else runs the original code.
static bool creature_in_wide_view()
{
    int extra = wide::view_extra();
    if (!extra)
        return false;
    uint32_t a0 = cpu_reg(A0), a1 = cpu_reg(A1);
    int x = int16_t(read16(a0 + 2)), y = int16_t(read16(a0 + 6));
    int w = int16_t(read16(a1 + 0x1E)), h = int16_t(read16(a1 + 0x20));
    int cx = int16_t(read16(0xAA4) & 0xFFF0), cy = int16_t(read16(0xAA6));
    int dx = int16_t(uint16_t(x - cx));
    bool x_ok = dx < 0 ? int16_t(uint16_t(-dx - w)) < 0 : int16_t(uint16_t(dx - 0x130)) <= 0;
    if (x_ok)
        return false;   // the game's own answer stands
    int left, right;
    wide::view_extents(cx, &left, &right);
    bool x_wide = dx < 0 ? (-dx - w) < left : (dx - 0x130) <= right;
    if (!x_wide)
        return false;
    int dy = int16_t(uint16_t(y - cy));
    int d0;
    if (dy < 0) {
        d0 = int16_t(uint16_t(-dy - h));
        if (d0 >= 0)
            return false;
    } else {
        d0 = int16_t(uint16_t(dy - 0xD0));
        if (d0 > 0)
            return false;
    }
    write16(0x1422, 1);
    cpu_set_reg(D4, (cpu_reg(D4) & 0xFFFF0000u) | uint16_t(cy));
    cpu_set_reg(D0, (cpu_reg(D0) & 0xFFFF0000u) | uint16_t(d0));
    cpu_set_reg(PC, kCreatureInViewDone);
    return true;
}

// The game's on-screen test for some objects (doors, padlocks). In widescreen
// an object that is only outside the game's 304-pixel view, but inside the
// wider one, takes the test's "visible" exit, exactly as the game would have.
// Everything else runs the original code.
static bool object_in_wide_view()
{
    int extra = wide::view_extra();
    uint32_t a0 = cpu_reg(A0);
    uint32_t obj = read32(a0 + 0x1A);
    if (!extra || !obj)
        return false;
    uint16_t x = read16(obj), y = read16(obj + 2), w = read16(0x1E46), h = read16(0x1E48);
    uint16_t cx = read16(0xAA4), cy = read16(0xAA6);
    bool x_ok = !(x > uint16_t(cx + 0x130)) && !(uint16_t(x + w) < cx);
    if (x_ok)
        return false;
    int xi = int16_t(x), wi = int16_t(w), cxi = int16_t(cx);
    int left, right;
    wide::view_extents(cxi, &left, &right);
    if (xi > cxi + 0x130 + right || xi + wi < cxi - left)
        return false;
    uint16_t y2 = uint16_t(y + h);
    if (y > uint16_t(cy + 0xD0) || y2 < cy)
        return false;
    // The visible exit: st ($26,a0); a6 = object; d4/d0/d5 as the compares
    // left them; flags from the last cmp.w $aa6,d5.
    write8(a0 + 0x26, 0xFF);
    cpu_set_reg(A6, obj);
    cpu_set_reg(D4, (cpu_reg(D4) & 0xFFFF0000u) | uint16_t(x + w));
    cpu_set_reg(D0, (obj & 0xFFFF0000u) | uint16_t(cy + 0xD0));
    cpu_set_reg(D5, (cpu_reg(D5) & 0xFFFF0000u) | y2);
    uint16_t r = uint16_t(y2 - cy);
    uint32_t ccr = (r & 0x8000 ? 8 : 0) | (r == 0 ? 4 : 0) | (((cy ^ y2) & (r ^ y2) & 0x8000) ? 2 : 0) | (cy > y2 ? 1 : 0);
    cpu_set_reg(SR, (cpu_reg(SR) & ~0x0Fu) | ccr);
    cpu_return();
    return true;
}

static void hook(uint32_t pc)
{
    const std::vector<uint8_t> &adf = disk::adf();
    if (pc == kCreatureInView) {
        creature_in_wide_view();
        return;
    }
    if (pc == kObjectInView) {
        bool widened = object_in_wide_view();
        if (std::getenv("JP2_VIEWLOG")) {
            uint32_t obj = read32(cpu_reg(A0) + 0x1A);
            std::fprintf(stderr, "in-view test: obj %06X x %d y %d cam %d,%d%s\n", obj, int16_t(read16(obj)),
                         int16_t(read16(obj + 2)), int16_t(read16(0xAA4)), int16_t(read16(0xAA6)),
                         widened ? "  -> visible (widened)" : "");
        }
        return;
    }
    if (pc == kMakeDrawRecord || pc == kMakeDrawRecord2) {
        wide::object_seen(cpu_reg(A2), cpu_reg(A1));
        return;
    }
    if (pc == kMakeTileRecord) {
        wide::tile_object_seen(cpu_reg(A1));
        return;
    }
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

    // As Kickstart leaves CIA-A: overlay off, power LED lit (an output).
    cia_write(0, 2, 0x03);
    cia_write(0, 0, 0x00);

    // Memory list the game reads at $200: chip memory, then the extra block.
    write32(0x200, 0);
    write32(0x204, kChipSize);
    write32(0x208, kExtBase);
    write32(0x20C, kExtSize);

    cpu_set_hook_handler(hook);
    cpu_set_hook(kReadSectors, true);
    cpu_set_hook(kLoadPacked, true);
    cpu_set_hook(kLoadRaw, true);
    cpu_set_hook(kMakeDrawRecord, true);
    cpu_set_hook(kMakeTileRecord, true);
    cpu_set_hook(kMakeDrawRecord2, true);
    cpu_set_hook(kObjectInView, true);
    cpu_set_hook(kCreatureInView, true);
    cpu_reset(disk::kEntry, 0x300);
    return true;
}

void enable_cheat() { ram[kCheatFlag] = 0xFF; }

} // namespace game
