// Frame trace for working out how the game draws: every custom register
// write (copper or CPU) and every blit during one chosen frame, written as
// text, plus a memory dump. Development only; the output stays local.
#pragma once
#include <cstdint>

namespace trace {

void arm(int frame, const char *dir);   // trace this frame number into dir
void frame_begin(int frame);
void frame_end();
bool on();

void reg_write(int line, int hpos, bool copper, uint32_t off, uint16_t v, uint32_t pc);
void blit(int line, uint32_t pc);

} // namespace trace
