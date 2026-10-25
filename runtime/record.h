#pragma once
#include <cstdint>

namespace record {

void start(const char *dir);   // write results into dir at finish()
bool active();
void frame();
void load(int file, uint32_t dest, uint32_t bytes, bool packed);
void flush();    // write what has been recorded so far
void finish();

} // namespace record
