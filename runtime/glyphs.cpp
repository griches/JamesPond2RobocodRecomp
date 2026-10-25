#include "glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace glyphs {

namespace {

const ui::Color kFace = ui::rgb(0xEEF2F8);   // the button
const ui::Color kMark = ui::rgb(0x0B1220);   // what is printed on it

const char *shoulder_label(Style s, Button b)
{
    bool left = b == Button::l1;
    switch (s) {
    case Style::xbox: return left ? "LB" : "RB";
    case Style::nintendo: return left ? "L" : "R";
    default: return left ? "L1" : "R1";   // PlayStation and Steam Deck
    }
}

// The letter on a face button, or null for the PlayStation shapes.
const char *face_letter(Style s, Button b)
{
    if (s == Style::playstation)
        return nullptr;
    if (s == Style::nintendo)   // printed the other way round
        return b == Button::south ? "B" : b == Button::east ? "A" : b == Button::west ? "Y" : "X";
    return b == Button::south ? "A" : b == Button::east ? "B" : b == Button::west ? "X" : "Y";
}

bool is_face(Button b) { return b == Button::south || b == Button::east || b == Button::west || b == Button::north; }

float pill_width(const char *label, float size)
{
    return std::max(size * 1.25f, ui::text_width(ui::semibold, size * 0.4f, label) + size * 0.62f);
}

} // namespace

float button_width(Style s, Button b, float size)
{
    if (b == Button::l1 || b == Button::r1)
        return pill_width(shoulder_label(s, b), size);
    if ((b == Button::menu || b == Button::view) && s == Style::playstation)
        return size * 1.45f;
    return size;
}

float button(Style s, Button b, float x, float cy, float size, float a)
{
    ui::Color face = ui::alpha(kFace, 0.92f * a), mark = ui::alpha(kMark, a);
    float r = size / 2, cx = x + r, w = button_width(s, b, size);
    float t = std::max(1.5f, size * 0.085f);   // stroke for the drawn marks
    if (is_face(b)) {
        ui::rect(x, cy - r, size, size, r, face);
        if (const char *letter = face_letter(s, b)) {
            ui::text(ui::semibold, size * 0.5f, cx, cy, letter, mark, ui::center);
            return w;
        }
        float k = size * 0.22f;
        switch (b) {
        case Button::south:   // cross
            ui::segment(cx - k, cy - k, cx + k, cy + k, t, mark);
            ui::segment(cx - k, cy + k, cx + k, cy - k, t, mark);
            break;
        case Button::east:   // circle
            ui::ring(cx - k * 1.15f, cy - k * 1.15f, k * 2.3f, k * 2.3f, k * 1.15f, t, mark);
            break;
        case Button::west:   // square
            ui::ring(cx - k, cy - k, k * 2, k * 2, 1.5f, t, mark);
            break;
        default: {   // triangle
            float top = cy - k * 1.15f, bot = cy + k * 0.8f, half = k * 1.15f;
            ui::segment(cx, top, cx - half, bot, t, mark);
            ui::segment(cx - half, bot, cx + half, bot, t, mark);
            ui::segment(cx + half, bot, cx, top, t, mark);
            break;
        }
        }
        return w;
    }
    switch (b) {
    case Button::l1:
    case Button::r1:
        ui::rect(x, cy - size * 0.42f, w, size * 0.84f, size * 0.26f, face);
        ui::text(ui::semibold, size * 0.4f, x + w / 2, cy, shoulder_label(s, b), mark, ui::center);
        break;
    case Button::menu:
    case Button::view: {
        // PlayStation: a small upright pill (Options / Create); the others a disc.
        if (s == Style::playstation)
            ui::rect(x + w * 0.2f, cy - r, w * 0.6f, size, size * 0.3f, face);
        else
            ui::rect(x, cy - r, size, size, r, face);
        float mx = x + w / 2;
        if (s == Style::nintendo) {   // + and -
            ui::segment(mx - size * 0.2f, cy, mx + size * 0.2f, cy, t, mark);
            if (b == Button::menu)
                ui::segment(mx, cy - size * 0.2f, mx, cy + size * 0.2f, t, mark);
        } else if (b == Button::menu) {   // three lines
            for (int i = -1; i <= 1; i++)
                ui::segment(mx - size * 0.2f, cy + float(i) * size * 0.16f, mx + size * 0.2f,
                            cy + float(i) * size * 0.16f, t * 0.9f, mark);
        } else {   // two overlapping windows
            float q = size * 0.15f;
            ui::ring(mx - q * 1.6f, cy - q * 1.4f, q * 2.2f, q * 1.9f, 1.5f, t * 0.8f, mark);
            ui::rect(mx - q * 0.6f, cy - q * 0.5f, q * 2.2f, q * 1.9f, 1.5f, mark);
        }
        break;
    }
    case Button::dpad:
    case Button::dpad_up: {
        float arm = size * 0.33f, len = size;
        ui::rect(cx - arm / 2, cy - len / 2, arm, len, size * 0.07f, face);
        ui::rect(x, cy - arm / 2, len, arm, size * 0.07f, face);
        if (b == Button::dpad_up) {   // the up arm marked
            float k = size * 0.1f, top = cy - len / 2 + size * 0.1f;
            ui::segment(cx - k, top + k, cx, top, t * 0.8f, mark);
            ui::segment(cx, top, cx + k, top + k, t * 0.8f, mark);
        }
        break;
    }
    case Button::lstick:
        ui::ring(x, cy - r, size, size, r, t, face);
        ui::rect(cx - r * 0.62f, cy - r * 0.62f, r * 1.24f, r * 1.24f, r * 0.62f, face);
        ui::text(ui::semibold, size * 0.34f, cx, cy, "L", mark, ui::center);
        break;
    default: break;
    }
    return w;
}

float key_width(const char *label, float size)
{
    return std::max(size, ui::text_width(ui::semibold, size * 0.42f, label) + size * 0.6f);
}

float key(const char *label, float x, float cy, float size, float a)
{
    float w = key_width(label, size);
    ui::rect(x, cy - size / 2, w, size, size * 0.22f, ui::alpha(kFace, 0.08f * a));
    ui::ring(x, cy - size / 2, w, size, size * 0.22f, 1.5f, ui::alpha(kFace, 0.55f * a));
    ui::text(ui::semibold, size * 0.42f, x + w / 2, cy, label, ui::alpha(kFace, a), ui::center);
    return w;
}

} // namespace glyphs
