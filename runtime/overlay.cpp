// Short on-screen messages ("QUICK SAVED") drawn over the game picture with
// a small built-in 5x7 font.
#include "overlay.h"

#include <cstring>

namespace {

// Rows top to bottom, '#' = lit.
struct Glyph {
    char c;
    const char *rows[7];
};
const Glyph kFont[] = {
    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
    {'C', {" ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####"}},
    {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
    {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
    {'G', {" ####", "#    ", "#    ", "#  ##", "#   #", "#   #", " ####"}},
    {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'J', {"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}},
    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
    {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
    {'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
    {'N', {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}},
    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
    {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
    {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}},
    {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
};

const Glyph *find(char c)
{
    for (const Glyph &g : kFont)
        if (g.c == c)
            return &g;
    return nullptr;
}

} // namespace

void draw_message(uint32_t *pixels, int w, int h, const char *text)
{
    const int scale = 1, adv = 6 * scale;
    int len = int(std::strlen(text));
    int box_w = len * adv + 6, box_h = 7 * scale + 6;
    int x0 = 4, y0 = h - box_h - 4;
    for (int y = y0; y < y0 + box_h && y < h; y++)
        for (int x = x0; x < x0 + box_w && x < w; x++) {
            uint32_t p = pixels[y * w + x];
            pixels[y * w + x] = 0xFF000000u | ((p >> 2) & 0x3F3F3F);   // darken behind the text
        }
    for (int i = 0; i < len; i++) {
        const Glyph *g = find(text[i]);
        if (!g)
            continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (g->rows[r][c] != '#')
                    continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int x = x0 + 3 + i * adv + c * scale + sx, y = y0 + 3 + r * scale + sy;
                        if (x < w && y < h)
                            pixels[y * w + x] = 0xFFFFFFFFu;
                    }
            }
    }
}
