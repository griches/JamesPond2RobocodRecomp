// Paula audio: four DMA channels stepped on emulated time (so the music
// player's audio interrupts arrive when the game expects them), mixed to
// 48 kHz stereo. Channels 0 and 3 go left, 1 and 2 right.
//
// The output stage follows the machine's: the channels are averaged over
// steps of a quarter of an output sample, then go through the Amiga's
// analogue filters (an A500's fixed 4.4 kHz low-pass, or an A1200's at
// 34 kHz; the "LED" filter when CIA-A switches it on; and the coupling
// capacitor's high-pass), then a low-pass FIR brings them down to 48 kHz.
#include "amiga.h"
#include "cpu.h"
#include "state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

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
// Lines until a channel's first interrupt. Paula raises it only once the
// first word has been fetched, at the channel's DMA slot up to a line after
// DMACON is written, so a player may clear stale requests right after
// starting a channel (James Pond 2's does) without losing it.
static int start_irq_lines[4];
static bool starting;   // inside a DMA start: hold the interrupt back

// Output ring, filled by paula_run_line, drained by the audio callback.
constexpr int kRing = 1 << 15;
static float ring[kRing * 2];
static int ring_w, ring_r;
static std::mutex ring_lock;
static double out_phase;   // Paula clocks into the current step
static double acc[4];      // sample * clocks over the current step

constexpr int kOversample = 4;
constexpr double kStepRate = double(kAudioRate) * kOversample;

static AudioOutput output;

// The analogue stage, one per side.
struct Side {
    double lp, hp;               // one-pole low-pass, the high-pass's low-pass
    double x1, x2, y1, y2;       // LED filter (biquad) history
};
static double lp_a, hp_a;                            // one-pole coefficients
static double led_b0, led_b1, led_b2, led_a1, led_a2;
static bool coeffs_ready;

// Decimation FIR: windowed sinc, cut at 20 kHz.
constexpr int kTaps = 128;
static float fir[kTaps];

static void make_coeffs()
{
    const double pi = 3.14159265358979323846;
    // A500: 360 ohm into 0.1 uF. A1200: 680 ohm into 6800 pF.
    double lp_hz = output.model == AudioOutput::a1200 ? 34419.0 : 4420.97;
    lp_a = output.model == AudioOutput::clean ? 1.0 : 1.0 - std::exp(-2 * pi * lp_hz / kStepRate);
    hp_a = 1.0 - std::exp(-2 * pi * 5.2 / kStepRate);   // 1390 ohm into 22 uF
    // LED filter: Sallen-Key, 10k/10k, 6800p/3900p: 3090 Hz, Q 0.660.
    double w0 = 2 * pi * 3090.5 / kStepRate, q = 0.660;
    double cw = std::cos(w0), alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
    led_b0 = (1 - cw) / 2 / a0;
    led_b1 = (1 - cw) / a0;
    led_b2 = led_b0;
    led_a1 = -2 * cw / a0;
    led_a2 = (1 - alpha) / a0;
    double sum = 0;
    for (int i = 0; i < kTaps; i++) {
        double m = i - (kTaps - 1) / 2.0;
        double x = 2 * 20000.0 / kStepRate;
        double sinc = m == 0 ? x : std::sin(pi * x * m) / (pi * m);
        double win = 0.42 - 0.5 * std::cos(2 * pi * i / (kTaps - 1)) + 0.08 * std::cos(4 * pi * i / (kTaps - 1));
        fir[i] = float(sinc * win);
        sum += sinc * win;
    }
    for (float &c : fir)
        c = float(c / sum);
    coeffs_ready = true;
}

void set_audio_output(const AudioOutput &o)
{
    output = o;
    make_coeffs();
}

static double analogue(Side &st, double x, bool led)
{
    st.lp += lp_a * (x - st.lp);
    double y = st.lp;
    if (led && output.model != AudioOutput::clean) {
        double z = led_b0 * y + led_b1 * st.x1 + led_b2 * st.x2 - led_a1 * st.y1 - led_a2 * st.y2;
        st.x2 = st.x1;
        st.x1 = y;
        st.y2 = st.y1;
        st.y1 = z;
        y = z;
    }
    st.hp += hp_a * (y - st.hp);
    return y - st.hp;
}

// The analogue stage, the FIR and the speaker mix, fed one step at a time.
// The game's sound goes through one; the menu's sound preview through another.
struct Stage {
    Side side[2];
    float hist_l[kTaps * 2], hist_r[kTaps * 2];   // doubled so a window never wraps
    int pos, phase;

    // Returns true when a 48 kHz frame is ready in (l, r).
    bool feed(double in_l, double in_r, bool led, float &l, float &r)
    {
        hist_l[pos] = hist_l[pos + kTaps] = float(analogue(side[0], in_l, led));
        hist_r[pos] = hist_r[pos + kTaps] = float(analogue(side[1], in_r, led));
        pos = (pos + 1) % kTaps;
        if (++phase < kOversample)
            return false;
        phase = 0;
        float sl = 0, sr = 0;
        const float *hl = &hist_l[pos], *hr = &hist_r[pos];   // oldest first
        for (int i = 0; i < kTaps; i++) {
            sl += fir[i] * hl[i];
            sr += fir[i] * hr[i];
        }
        // Stereo width: 1 is the Amiga's own (each channel on one side only).
        float w = output.separation, near = (1 + w) * 0.5f, far = (1 - w) * 0.5f;
        l = (sl * near + sr * far) * output.volume;
        r = (sr * near + sl * far) * output.volume;
        return true;
    }
};
static Stage live, preview;

// The last few seconds before the analogue stage, for the menu to replay
// through whatever output settings are being tried.
constexpr int kKeepSteps = int(kStepRate) * 6;
static std::vector<float> kept_l(kKeepSteps), kept_r(kKeepSteps);
static int kept_pos, kept_count;
static std::vector<float> loop_l, loop_r;   // the preview: the kept steps, oldest first
static int loop_pos;

static uint16_t &dmacon_reg() { return regs[0x002 >> 1]; }

static bool audio_log_on();

// Ringing out: many of the game's samples were cut off while still sounding,
// and on the Amiga they simply stop. With AudioOutput::ring_out the last wave
// cycle a channel played carries on, fading over a moment, until the channel
// plays something again.
struct Ring {
    bool on;
    int8_t wave[64];
    int len, idx;
    double ticks, per;   // Paula clocks left on this sample, and per sample
    double env, decay;
    int vol;
};
static Ring ringing[4];
static int8_t recent[4][128];   // the last samples each channel played, oldest first from recent_pos
static int recent_pos[4];

static void ring_out(int n)
{
    Ring &r = ringing[n];
    const Channel &c = ch[n];
    if (!output.ring_out || r.on || c.vol == 0)
        return;
    int8_t last[128];
    for (int i = 0; i < 128; i++)
        last[i] = recent[n][(recent_pos[n] + i) & 127];
    // Only a sound that was still going: something left in its last samples.
    double power = 0;
    for (int i = 112; i < 128; i++)
        power += double(last[i]) * last[i];
    if (power / 16 < 8.0 * 8.0)
        return;
    // Its cycle: how well the last samples match themselves shifted by each
    // lag. Neighbours always look alike, so look for the best match after the
    // match has first fallen away; nothing repeating (noise, a drum) rings no tone.
    double match[65] = {};
    for (int lag = 1; lag <= 64; lag++) {
        double xy = 0, xx = 0, yy = 0;
        for (int i = 128 - 64; i < 128; i++) {
            double a = last[i], b = last[i - lag];
            xy += a * b;
            xx += a * a;
            yy += b * b;
        }
        match[lag] = xy / (std::sqrt(xx * yy) + 1e-9);
    }
    int from = 0;
    for (int lag = 1; lag <= 64 && !from; lag++)
        if (match[lag] < 0.2)
            from = lag;
    int best = 0;
    for (int lag = from ? from : 65; lag <= 64; lag++)
        if (match[lag] > 0.6 && (!best || match[lag] > match[best]))
            best = lag;
    if (!best)
        return;
    r.len = best;
    for (int i = 0; i < best; i++)
        r.wave[i] = last[128 - best + i];
    r.idx = 0;
    r.per = c.per < 124 ? 124 : c.per;
    r.ticks = r.per;
    r.env = 1;
    r.decay = std::exp(-r.per / (0.022 * kPaulaClockPal));   // falls to 1/e in 22 ms
    r.vol = c.vol;
    r.on = true;
    if (audio_log_on())
        std::printf("ring ch%d lc %06X cycle %d vol %d\n", n, c.lc, best, c.vol);
}

// True when a block holds nothing but silence (the game's way of ending a sound).
static bool silent_block(uint32_t lc, uint16_t len)
{
    if (len == 0 || len > 64)
        return false;
    for (uint32_t i = 0; i < len; i++)
        if (chip16((lc & ~1u) + i * 2))
            return false;
    return true;
}

static SampleStart start_listener;
void paula_on_start(SampleStart fn) { start_listener = fn; }

// JP2_AUDIOLOG=1: each channel's starts, loops, stops and volume changes, by time.
static const bool audio_log = std::getenv("JP2_AUDIOLOG") != nullptr;
static long lines_run;
static bool audio_log_on() { return audio_log; }
static double log_ms() { return double(lines_run) * 1000.0 / (50.0 * kLinesPal); }

static void start_block(int n)
{
    Channel &c = ch[n];
    c.pt = c.lc & ~1u;
    c.words_left = c.len ? c.len : 0x10000;
    if (!starting) {   // the next block: silence lets the last sound ring out, a sound stops it
        if (silent_block(c.lc, c.len))
            ring_out(n);
        else
            ringing[n].on = false;
    }
    if (audio_log && !starting)
        std::printf("%9.2f ch%d loop  lc %06X len %5u\n", log_ms(), n, c.lc, c.len);
    if (starting) {
        start_irq_lines[n] = 2;   // after the rest of this line and the next
    } else {
        start_irq_lines[n] = 0;
        request_irq(uint16_t(INT_AUD0 << n));
    }
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
        if (audio_log && now != was)
            std::printf("%9.2f ch%d %s lc %06X len %5u per %4u vol %2u  words left %5u  pc %06X\n", log_ms(), n,
                        now ? "START" : "stop ", c.lc, c.len, c.per, c.vol, now ? 0 : c.words_left, cpu_reg(PC));
        if (now && !was) {
            ringing[n].on = false;
            if (start_listener)
                start_listener(n, c.lc, c.len);
            c.running = true;
            starting = true;
            start_block(n);
            next_word(n);
            starting = false;
            c.byte = 0;
            c.ticks = c.per ? c.per : 1;
            c.sample = int8_t(c.word >> 8);
        } else if (!now && was) {
            ring_out(n);
            c.running = false;
            c.sample = 0;
            start_irq_lines[n] = 0;   // stopped before the first fetch: no interrupt
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
    case 0x8:
        if (audio_log && c.running && (v & 0x7F) != c.vol)
            std::printf("%9.2f ch%d vol %2u -> %2u\n", log_ms(), n, c.vol, v & 0x40 ? 64 : (v & 0x3F));
        c.vol = v & 0x40 ? 64 : (v & 0x3F);
        break;
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
        Ring &r = ringing[n];
        double left = clocks;
        if (!c.running && !r.on) {
            acc[n] += c.sample * c.vol * left;
            continue;
        }
        double per = c.per < 124 ? 124 : c.per;
        while (left > 0) {
            double run = left;
            if (c.running && c.ticks < run)
                run = c.ticks;
            if (r.on && r.ticks < run)
                run = r.ticks;
            double out = c.sample * c.vol;
            if (r.on)
                out += r.wave[r.idx] * r.env * r.vol;
            acc[n] += out * run;
            left -= run;
            if (c.running) {
                c.ticks -= run;
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
                    recent[n][recent_pos[n]] = c.sample;
                    recent_pos[n] = (recent_pos[n] + 1) & 127;
                }
            }
            if (r.on) {
                r.ticks -= run;
                if (r.ticks <= 0) {
                    r.ticks += r.per;
                    r.idx = (r.idx + 1) % r.len;
                    r.env *= r.decay;
                    if (r.env < 0.004)
                        r.on = false;
                }
            }
        }
    }
}

void paula_run_line()
{
    if (!coeffs_ready)
        make_coeffs();
    double clock = kPaulaClockPal;   // close enough for NTSC too
    double per_line = clock / (pal_mode() ? 50.0 * kLinesPal : 60.0 * kLinesNtsc);
    double per_step = clock / kStepRate;
    lines_run++;
    for (int n = 0; n < 4; n++)
        if (start_irq_lines[n] && --start_irq_lines[n] == 0)
            request_irq(uint16_t(INT_AUD0 << n));
    if (out_phase >= per_step || out_phase < 0)
        out_phase = 0;   // a save from before the steps were this short
    bool led = cia_led_filter();
    double left = per_line;
    while (left > 0) {
        double run = per_step - out_phase;
        if (run > left)
            run = left;
        step(run);
        out_phase += run;
        left -= run;
        if (out_phase < per_step)
            continue;
        out_phase -= per_step;
        // sample (-128..127) * vol (0..64): full scale is 8192 per channel
        double s[4];
        for (int n = 0; n < 4; n++) {
            s[n] = acc[n] / per_step / 8192.0;
            acc[n] = 0;
        }
        double in_l = (s[0] + s[3]) * 0.5, in_r = (s[1] + s[2]) * 0.5;
        kept_l[size_t(kept_pos)] = float(in_l);
        kept_r[size_t(kept_pos)] = float(in_r);
        kept_pos = (kept_pos + 1) % kKeepSteps;
        kept_count = std::min(kept_count + 1, kKeepSteps);
        float l, r;
        if (live.feed(in_l, in_r, led, l, r))
            emit(l, r);
    }
}

void audio_preview_begin()
{
    loop_l.assign(size_t(kept_count), 0);
    loop_r.assign(size_t(kept_count), 0);
    int first = (kept_pos - kept_count + kKeepSteps) % kKeepSteps;
    for (int i = 0; i < kept_count; i++) {
        loop_l[size_t(i)] = kept_l[size_t((first + i) % kKeepSteps)];
        loop_r[size_t(i)] = kept_r[size_t((first + i) % kKeepSteps)];
    }
    loop_pos = 0;
    preview = Stage{};
}

int audio_preview_take(float *out, int frames)
{
    int n = int(loop_l.size()), fade = std::min(n / 4, int(kStepRate) / 8);   // 125 ms seam
    if (n < kOversample * 64) {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
        return frames;
    }
    bool led = cia_led_filter();
    int done = 0;
    while (done < frames) {
        // Round and round, the end crossfaded into the start so the seam doesn't click.
        double l = loop_l[size_t(loop_pos)], r = loop_r[size_t(loop_pos)];
        int into = loop_pos - (n - fade);
        if (into >= 0) {
            double t = double(into) / fade;
            l = l * (1 - t) + loop_l[size_t(into)] * t;
            r = r * (1 - t) + loop_r[size_t(into)] * t;
        }
        if (++loop_pos >= n)
            loop_pos = fade;
        float ol, orr;
        if (preview.feed(l, r, led, ol, orr)) {
            out[done * 2] = ol;
            out[done * 2 + 1] = orr;
            done++;
        }
    }
    return done;
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

void paula_state(StateIO &s)
{
    s.bytes(ch, sizeof ch);
    s.io(out_phase);
    s.bytes(acc, sizeof acc);
    if (s.loading()) {
        std::memset(start_irq_lines, 0, sizeof start_irq_lines);
        std::memset(ringing, 0, sizeof ringing);
        std::lock_guard<std::mutex> g(ring_lock);
        ring_r = ring_w;
    }
}

} // namespace amiga
