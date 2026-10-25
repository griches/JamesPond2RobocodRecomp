// Button pictures for the menu and its hints, drawn in the style of the
// controller in use (the PlayStation shapes on a DualSense, letters on a
// Steam Deck or Xbox pad), or as key caps for the keyboard.
#pragma once
#include "ui.h"

namespace glyphs {

enum class Style { deck, xbox, playstation, nintendo };
enum class Button { south, east, west, north, l1, r1, menu, view, dpad, dpad_up, lstick };

// Left edge at x, centred on cy, `size` tall. Each returns the width drawn.
float button(Style s, Button b, float x, float cy, float size, float alpha = 1);
float button_width(Style s, Button b, float size);
float key(const char *label, float x, float cy, float size, float alpha = 1);
float key_width(const char *label, float size);

} // namespace glyphs
