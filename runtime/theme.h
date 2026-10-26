// The look the menu and the first-run screen share.
#pragma once
#include "ui.h"

namespace theme {

const ui::Color kBg = ui::rgb(0x0A101C);
const ui::Color kSurface = ui::rgb(0x141F33);    // panels, at their top
const ui::Color kSurface2 = ui::rgb(0x0F182A);   // ... and their bottom
const ui::Color kRaised = ui::rgb(0x1D2C48);     // the focused row
const ui::Color kInk = ui::rgb(0xEEF2F8);
const ui::Color kDim = ui::rgb(0x98A4BA);
const ui::Color kFaint = ui::rgb(0x5D6982);
const ui::Color kAccent = ui::rgb(0xFFB547);     // focus and "on"
const ui::Color kWarn = ui::rgb(0xFF8A80);
const ui::Color kDeep = ui::rgb(0x0A101C);

constexpr float kMargin = 96;
constexpr float kRadius = 18;

// The one focus: a soft glow, a raised fill and an accent ring.
inline void focus(float x, float y, float w, float h, float alpha = 1)
{
    ui::glow(x, y, w, h, kRadius, 26, ui::alpha(kAccent, 0.20f * alpha));
    ui::rect_v(x, y, w, h, kRadius, ui::alpha(ui::mix(kRaised, kInk, 0.04f), alpha), ui::alpha(kRaised, alpha));
    ui::ring(x, y, w, h, kRadius, 2, ui::alpha(kAccent, alpha));
}

} // namespace theme
