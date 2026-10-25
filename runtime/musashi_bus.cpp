// Musashi's memory callbacks, onto the Amiga bus.
#include "amiga.h"

extern "C" {
#include "m68k.h"
}

using namespace amiga;

extern "C" {

unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { write8(a, uint8_t(v)); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { write16(a, uint16_t(v)); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { write32(a, v); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return a < kRamSize ? ram[a] : 0; }
unsigned int m68k_read_disassembler_16(unsigned int a) { return read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return read32(a); }

}
