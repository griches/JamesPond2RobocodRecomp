#include "state.h"

#include "amiga.h"
#include "disk.h"

namespace amiga {

namespace {

constexpr char kMagic[8] = {'J', 'P', '2', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t kVersion = 1;

// Which game the save belongs to: the main program as unpacked from the disk.
uint32_t game_id()
{
    std::vector<uint8_t> prog;
    std::string err;
    if (!disk::main_program(prog, &err))
        return 0;
    return disk::crc32(prog.data(), prog.size());
}

void machine(StateIO &s)
{
    s.bytes(ram, kRamSize);
    custom_state(s);
    display_state(s);
    paula_state(s);
    cia_state(s);
    cpu_state(s);
}

} // namespace

bool save_state(std::vector<uint8_t> &out)
{
    out.clear();
    StateIO s(&out);
    char magic[8];
    std::memcpy(magic, kMagic, 8);
    uint32_t version = kVersion, id = game_id();
    s.bytes(magic, 8);
    s.io(version);
    s.io(id);
    machine(s);
    return true;
}

bool load_state(const std::vector<uint8_t> &in, std::string *error)
{
    StateIO s(in.data(), in.size());
    char magic[8];
    uint32_t version = 0, id = 0;
    s.bytes(magic, 8);
    s.io(version);
    s.io(id);
    if (!s.ok() || std::memcmp(magic, kMagic, 8) != 0 || version != kVersion) {
        *error = "not a save this version can read";
        return false;
    }
    if (id != game_id()) {
        *error = "the save is from a different disk";
        return false;
    }
    // Check it all reads before touching the running machine.
    std::vector<uint8_t> backup;
    save_state(backup);
    machine(s);
    if (!s.ok()) {
        StateIO undo(backup.data(), backup.size());
        char m[8];
        uint32_t v, i;
        undo.bytes(m, 8);
        undo.io(v);
        undo.io(i);
        machine(undo);
        *error = "the save is cut short";
        return false;
    }
    // Memory changed wholesale: translated code must be re-checked.
    std::memset(page_dirty, 1, sizeof page_dirty);
    return true;
}

} // namespace amiga
