// Widescreen: during play the picture is redrawn from the game's own state
// (level map, parallax layer, palette) wider than the Amiga's 288-pixel
// window. Stage one keeps the game's picture in the middle and draws the
// extra columns either side.
#pragma once
#include <cstdint>

namespace amiga {
class StateIO;
}

namespace wide {

constexpr int kHeight = 208;          // the gameplay window
constexpr int kMaxWidth = 512;

void blit_seen();                      // from the blitter, before each blit runs
void object_seen(uint32_t frame_def, uint32_t rel_base);   // the game is about to make a draw record
void tile_object_seen(uint32_t image);                     // ... for a 16x16 item (no mask)
void frame_done();                     // after each emulated frame

// How many pixels the view reaches past the game's own on each side. Objects
// that test whether they are on screen use it (0 = the original game).
void set_view_extra(int pixels);
int view_extra();
// The same, split for a camera at cam_x: near the level's ends the wide view
// stops at the edge, so it reaches further on the other side.
void view_extents(int cam_x, int *left, int *right);

// Draws the frame just shown at `width` (>= 288). Returns false outside
// play, when the normal picture should be used. With `full`, everything is
// redrawn from game state and `alpha` (0..1) places the camera and objects
// between the previous tick and this one; otherwise the middle is the game's
// own picture.
bool render(uint32_t *out, int width, float alpha = 1.0f, bool full = false);

// The renderer's memory of the parallax layer, kept in quick saves.
void save_state(amiga::StateIO &s);

// True while the game is showing a level (not titles, maps or menus).
bool playing();

// Development check: how far the redrawn middle strays from the real picture.
void self_check();
void flicker_check();

} // namespace wide
