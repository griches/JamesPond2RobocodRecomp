#pragma once
#include <cstdint>

// Write 0xAARRGGBB pixels as an RGB PNG. stride is in pixels.
bool write_png(const char *path, const uint32_t *argb, int w, int h, int stride);
