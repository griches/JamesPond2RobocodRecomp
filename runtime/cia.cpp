// The two 8520 CIAs: timers, time-of-day counters, the keyboard serial port
// and the joystick fire button. CIA-A interrupts arrive as PORTS (level 2),
// CIA-B as EXTER (level 6).
#include "amiga.h"
#include "state.h"

#include <deque>
#include <mutex>

namespace amiga {

struct Cia {
    uint8_t pra, prb, ddra, ddrb;
    uint16_t ta, tb, ta_latch, tb_latch;
    uint8_t cra, crb;
    uint8_t icr, mask;
    uint32_t tod, tod_latch;
    bool tod_latched;
    uint8_t sdr;
};
static Cia cia[2];
static double eclock_frac;

struct KeyEvent {
    int code;
    bool down;
};
static std::deque<KeyEvent> keys;
static std::mutex keys_lock;
static int key_wait;   // lines left before the next key may be sent

void key_event(int code, bool down)
{
    std::lock_guard<std::mutex> g(keys_lock);
    keys.push_back({code, down});
}

static void raise(int which, uint8_t bits)
{
    Cia &c = cia[which];
    c.icr |= bits;
    if (c.icr & c.mask)
        request_irq(which == 0 ? INT_PORTS : INT_EXTER);
}

void cia_reset()
{
    for (Cia &c : cia) {
        c = Cia{};
        c.ta = c.tb = c.ta_latch = c.tb_latch = 0xFFFF;
    }
    keys.clear();
    key_wait = 0;
}

// Bit 1 of CIA-A port A drives the power LED low (lit), which also switches
// in the audio filter. As an input (Kickstart sets it as an output) it is
// pulled high.
bool cia_led_filter()
{
    const Cia &c = cia[0];
    return (c.ddra & 2) && !(c.pra & 2);
}

uint8_t cia_read(int which, int reg)
{
    Cia &c = cia[which];
    switch (reg) {
    case 0x0: {
        uint8_t in;
        if (which == 0) {
            in = 0xFC;                   // disk lines idle, both fire buttons up
            if (joy.fire)
                in &= uint8_t(~0x80);
        } else {
            in = 0xFF;
        }
        return uint8_t((c.pra & c.ddra) | (in & ~c.ddra));
    }
    case 0x1:
        return uint8_t((c.prb & c.ddrb) | (0xFF & ~c.ddrb));
    case 0x2: return c.ddra;
    case 0x3: return c.ddrb;
    case 0x4: return uint8_t(c.ta);
    case 0x5: return uint8_t(c.ta >> 8);
    case 0x6: return uint8_t(c.tb);
    case 0x7: return uint8_t(c.tb >> 8);
    case 0x8: {
        uint32_t t = c.tod_latched ? c.tod_latch : c.tod;
        c.tod_latched = false;
        return uint8_t(t);
    }
    case 0x9: return uint8_t((c.tod_latched ? c.tod_latch : c.tod) >> 8);
    case 0xA:
        c.tod_latch = c.tod;
        c.tod_latched = true;
        return uint8_t(c.tod >> 16);
    case 0xC: return c.sdr;
    case 0xD: {
        uint8_t v = c.icr;
        if (v & c.mask)
            v |= 0x80;
        c.icr = 0;
        return v;
    }
    case 0xE: return c.cra;
    case 0xF: return c.crb;
    default: return 0xFF;
    }
}

void cia_write(int which, int reg, uint8_t v)
{
    Cia &c = cia[which];
    switch (reg) {
    case 0x0: c.pra = v; break;
    case 0x1: c.prb = v; break;
    case 0x2: c.ddra = v; break;
    case 0x3: c.ddrb = v; break;
    case 0x4: c.ta_latch = (c.ta_latch & 0xFF00) | v; break;
    case 0x5:
        c.ta_latch = uint16_t((c.ta_latch & 0xFF) | v << 8);
        if (!(c.cra & 1))
            c.ta = c.ta_latch;
        if (c.cra & 8)
            c.cra |= 1;   // one-shot timers start when the high byte is written
        break;
    case 0x6: c.tb_latch = (c.tb_latch & 0xFF00) | v; break;
    case 0x7:
        c.tb_latch = uint16_t((c.tb_latch & 0xFF) | v << 8);
        if (!(c.crb & 1))
            c.tb = c.tb_latch;
        if (c.crb & 8)
            c.crb |= 1;
        break;
    case 0x8: c.tod = (c.tod & 0xFFFF00) | v; break;
    case 0x9: c.tod = (c.tod & 0xFF00FF) | uint32_t(v) << 8; break;
    case 0xA: c.tod = (c.tod & 0x00FFFF) | uint32_t(v) << 16; break;
    case 0xC: c.sdr = v; break;
    case 0xD:
        if (v & 0x80)
            c.mask |= v & 0x1F;
        else
            c.mask &= uint8_t(~v);
        if (c.icr & c.mask)
            request_irq(which == 0 ? INT_PORTS : INT_EXTER);
        break;
    case 0xE:
        if (which == 0 && (c.cra & 0x40) && !(v & 0x40) && key_wait > 2)
            key_wait = 2;   // keyboard handshake done: next key can follow soon
        if (v & 0x10)
            c.ta = c.ta_latch;
        c.cra = v & ~0x10;
        break;
    case 0xF:
        if (v & 0x10)
            c.tb = c.tb_latch;
        c.crb = v & ~0x10;
        break;
    }
}

// Count a timer down by n ticks; returns the number of underflows.
static int count(uint16_t &t, uint16_t latch, uint8_t &cr, int n)
{
    int under = 0;
    while (n > 0 && (cr & 1)) {
        if (n <= t) {
            t = uint16_t(t - n);
            break;
        }
        n -= t + 1;
        t = latch;
        under++;
        if (cr & 8) {
            cr &= uint8_t(~1);
            break;
        }
    }
    return under;
}

void cia_run_line()
{
    double per_line = kEClockPal / (pal_mode() ? 50.0 * kLinesPal : 60.0 * kLinesNtsc);
    eclock_frac += per_line;
    int ticks = int(eclock_frac);
    eclock_frac -= ticks;
    for (int w = 0; w < 2; w++) {
        Cia &c = cia[w];
        int ua = 0;
        if ((c.cra & 1) && !(c.cra & 0x20))
            ua = count(c.ta, c.ta_latch, c.cra, ticks);
        if (ua)
            raise(w, 1);
        int mode = (c.crb >> 5) & 3;
        int ub = 0;
        if (c.crb & 1) {
            if (mode == 0)
                ub = count(c.tb, c.tb_latch, c.crb, ticks);
            else if (mode == 2 && ua)
                ub = count(c.tb, c.tb_latch, c.crb, ua);
        }
        if (ub)
            raise(w, 2);
    }
    cia[1].tod = (cia[1].tod + 1) & 0xFFFFFF;   // CIA-B counts lines

    if (key_wait > 0) {
        key_wait--;
        return;
    }
    std::lock_guard<std::mutex> g(keys_lock);
    if (keys.empty())
        return;
    KeyEvent k = keys.front();
    keys.pop_front();
    cia[0].sdr = uint8_t(~((k.code << 1) | (k.down ? 0 : 1)));
    raise(0, 8);
    key_wait = 40;   // falls back to a short gap if the game never hands back
}

void cia_frame()
{
    cia[0].tod = (cia[0].tod + 1) & 0xFFFFFF;   // CIA-A counts frames
}

void cia_state(StateIO &s)
{
    s.bytes(cia, sizeof cia);
    s.io(eclock_frac);
    s.io(key_wait);
    if (s.loading()) {
        std::lock_guard<std::mutex> g(keys_lock);
        keys.clear();
    }
}

} // namespace amiga
