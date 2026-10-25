// Paula audio: four DMA channels stepped on emulated time (so the music
// player's audio interrupts arrive when the game expects them), mixed to
// 48 kHz stereo. Channels 0 and 3 go left, 1 and 2 right.
#include "amiga.h"

#include <cstring>
#include <mutex>

namespace amiga {

struct Channel {
    uint32_t lc;        // AUDxLC as written
    uint16_t len, per, vol;
    uint32_t pt;        // DMA position
    uint32_t words_left;
    bool running;
    uint16_t word;      // current sample pair
    int byte;           // 0 = high byte playing, 1 = low byte
    double ticks;       // Paula clocks until the next sample
    int8_t sample;
};
static Channel ch[4];

// Output ring, filled by paula_run_line, drained by the audio callback.
constexpr int kRing = 1 << 15;
static float ring[kRing * 2];
static int ring_w, ring_r;
static std::mutex ring_lock;
static double out_phase;   // Paula clocks into the current output sample
static double acc[4];      // sample * clocks over the current output sample

static uint16_t &dmacon_reg() { return regs[0x002 >> 1]; }

static void start_block(int n)
{
    Channel &c = ch[n];
    c.pt = c.lc & ~1u;
    c.words_left = c.len ? c.len : 0x10000;
    request_irq(uint16_t(INT_AUD0 << n));
}

static void next_word(int n)
{
    Channel &c = ch[n];
    c.word = chip16(c.pt);
    c.pt += 2;
    if (--c.words_left == 0)
        start_block(n);
}

void paula_dma_changed(uint16_t old_dmacon, uint16_t new_dmacon)
{
    for (int n = 0; n < 4; n++) {
        bool was = (old_dmacon & DMA_EN) && (old_dmacon & (1 << n));
        bool now = (new_dmacon & DMA_EN) && (new_dmacon & (1 << n));
        Channel &c = ch[n];
        if (now && !was) {
            c.running = true;
            start_block(n);
            next_word(n);
            c.byte = 0;
            c.ticks = c.per ? c.per : 1;
            c.sample = int8_t(c.word >> 8);
        } else if (!now && was) {
            c.running = false;
            c.sample = 0;
        }
    }
}

void paula_reg_write(uint32_t off, uint16_t v)
{
    int n = (off - 0x0A0) >> 4;
    Channel &c = ch[n];
    switch (off & 0xE) {
    case 0x0: c.lc = (c.lc & 0xFFFF) | uint32_t(v) << 16; break;
    case 0x2: c.lc = (c.lc & 0xFFFF0000u) | v; break;
    case 0x4: c.len = v; break;
    case 0x6: c.per = v; break;
    case 0x8: c.vol = v & 0x40 ? 64 : (v & 0x3F); break;
    case 0xA:
        if (!c.running) {   // CPU-driven output: play the pair once
            c.word = v;
            c.sample = int8_t(v >> 8);
        }
        break;
    }
}

static void emit(float l, float r)
{
    std::lock_guard<std::mutex> g(ring_lock);
    int next = (ring_w + 1) & (kRing - 1);
    if (next == ring_r)
        ring_r = (ring_r + 1) & (kRing - 1);   // drop the oldest if nobody reads
    ring[ring_w * 2] = l;
    ring[ring_w * 2 + 1] = r;
    ring_w = next;
}

// Advance every channel by `clocks` Paula clocks, adding to the running
// average for the current output sample.
static void step(double clocks)
{
    for (int n = 0; n < 4; n++) {
        Channel &c = ch[n];
        double left = clocks;
        if (!c.running) {
            acc[n] += c.sample * c.vol * left;
            continue;
        }
        double per = c.per < 124 ? 124 : c.per;
        while (left > 0) {
            double run = c.ticks < left ? c.ticks : left;
            acc[n] += c.sample * c.vol * run;
            c.ticks -= run;
            left -= run;
            if (c.ticks <= 0) {
                c.ticks += per;
                if (c.byte == 0) {
                    c.byte = 1;
                    c.sample = int8_t(c.word);
                } else {
                    c.byte = 0;
                    next_word(n);
                    c.sample = int8_t(c.word >> 8);
                }
            }
        }
    }
}

void paula_run_line()
{
    double clock = kPaulaClockPal;   // close enough for NTSC too
    double per_line = clock / (pal_mode() ? 50.0 * kLinesPal : 60.0 * kLinesNtsc);
    double per_out = clock / kAudioRate;
    double left = per_line;
    while (left > 0) {
        double run = per_out - out_phase;
        if (run > left)
            run = left;
        step(run);
        out_phase += run;
        left -= run;
        if (out_phase >= per_out) {
            out_phase -= per_out;
            // sample (-128..127) * vol (0..64): full scale is 8192 per channel
            float s[4];
            for (int n = 0; n < 4; n++) {
                s[n] = float(acc[n] / per_out / 8192.0);
                acc[n] = 0;
            }
            float l = (s[0] + s[3]) * 0.5f, r = (s[1] + s[2]) * 0.5f;
            // a little crossfeed: hard-panned Amiga stereo is tiring on headphones
            emit(l * 0.8f + r * 0.2f, r * 0.8f + l * 0.2f);
        }
    }
}

int audio_take(float *out, int max_frames)
{
    std::lock_guard<std::mutex> g(ring_lock);
    int n = 0;
    while (n < max_frames && ring_r != ring_w) {
        out[n * 2] = ring[ring_r * 2];
        out[n * 2 + 1] = ring[ring_r * 2 + 1];
        ring_r = (ring_r + 1) & (kRing - 1);
        n++;
    }
    return n;
}

} // namespace amiga
