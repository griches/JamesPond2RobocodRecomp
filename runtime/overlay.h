#pragma once
#include <cstdint>

// Draw a short message (capitals and spaces) in the bottom-left corner.
void draw_message(uint32_t *pixels, int w, int h, const char *text);
