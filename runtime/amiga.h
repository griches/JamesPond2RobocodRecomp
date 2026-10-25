// Stand-in for the Amiga 500 hardware the game talks to: memory, the custom
// chips (copper, blitter, display, Paula) and the two CIAs. Everything runs
// on emulated time measured in scanlines; the CPU side (interpreter now,
// recompiled code later) is driven from here one line at a time.
#pragma once
#include <cstdint>

namespace amiga {

// 512 KB chip memory followed by the 512 KB block the game asks for through
// its memory list at $200. One flat array keeps every access a plain index.
constexpr uint32_t kChipSize = 0x80000;
constexpr uint32_t kExtBase = 0x80000;
constexpr uint32_t kExtSize = 0x80000;
constexpr uint32_t kRamSize = kExtBase + kExtSize;

constexpr int kLinesPal = 313;
constexpr int kLinesNtsc = 263;
constexpr int kCcksPerLine = 227;           // colour clocks
constexpr int kCpuCyclesPerLine = 454;      // 68000 at 2 cycles per colour clock
constexpr double kPaulaClockPal = 3546895.0;
constexpr double kEClockPal = 709379.0;

// The picture is kept in display-window coordinates: lowres pixel columns
// [kFbX0, kFbX0 + kFbW) and beam lines [kFbY0, kFbY0 + kFbH). That covers the
// normal 320x256 window ($81,$2C) with room for overscan either side.
constexpr int kFbX0 = 0x61;
constexpr int kFbW = 384;
constexpr int kFbY0 = 0x1c;
constexpr int kFbH = 288;

extern uint8_t ram[kRamSize];
extern uint8_t *code_watch;            // see memory.cpp
void code_write_seen(uint32_t addr);   // recording hook, in record.cpp

// Translated-code tracking: a bit per byte that holds compiled instructions,
// and a flag per 256-byte page set when such a byte is written. The
// recompiled CPU re-checks a dirty page before running code from it.
extern uint8_t code_bits[kRamSize >> 3];
extern uint8_t page_dirty[kRamSize >> 8];
inline void touch(uint32_t a)
{
    if (code_bits[a >> 3] & (1u << (a & 7))) {
        page_dirty[a >> 8] = 1;
        page_dirty[((a - 10) & (kRamSize - 1)) >> 8] = 1;   // an instruction may start in the page before
    }
}
void loaded(uint32_t addr, uint32_t bytes);   // bulk write (disk loads)
extern uint32_t framebuffer[kFbW * kFbH];   // 0xAARRGGBB
extern uint8_t index_buffer[kFbW * kFbH];   // raw bitplane colour index per pixel (0 in the border)
// Sprite colour (16-31) where a sprite is shown, else 0; bit 7 set when the
// sprite's segment starts in a HUD band (the score and lives).
extern uint8_t sprite_buffer[kFbW * kFbH];

// What the display used, kept for renderers that redraw the picture: the
// colour registers at the end of every line, and the bitplane pointers and
// scroll value as they stood on the first line bitplanes were fetched.
extern uint16_t line_palette[kFbH][32];
struct PlayfieldStart {
    int line;             // -1 if no bitplanes were shown this frame
    uint32_t bpl[6];
    uint16_t bplcon0, bplcon1, diwstrt, diwstop, ddfstrt, ddfstop;
};
extern PlayfieldStart playfield_start;

// ---- bus -------------------------------------------------------------------
uint8_t read8(uint32_t a);
uint16_t read16(uint32_t a);
uint32_t read32(uint32_t a);
void write8(uint32_t a, uint8_t v);
void write16(uint32_t a, uint16_t v);
void write32(uint32_t a, uint32_t v);

// Chip memory as the DMA side sees it (bitplanes, copper, blitter, sprites,
// audio). Word aligned, wraps inside RAM.
inline uint16_t chip16(uint32_t a)
{
    a &= (kRamSize - 1) & ~1u;
    return uint16_t(ram[a] << 8 | ram[a + 1]);
}
inline void chip16_write(uint32_t a, uint16_t v)
{
    a &= (kRamSize - 1) & ~1u;
    touch(a);
    touch(a + 1);
    ram[a] = uint8_t(v >> 8);
    ram[a + 1] = uint8_t(v);
}

// ---- custom chips --------------------------------------------------------
extern uint16_t regs[0x100];   // last value written to each register, by offset/2

void custom_reset();
uint16_t custom_read(uint32_t off);
void custom_write(uint32_t off, uint16_t v);

// INTREQ bits
enum : uint16_t {
    INT_TBE = 1 << 0, INT_DSKBLK = 1 << 1, INT_SOFT = 1 << 2, INT_PORTS = 1 << 3,
    INT_COPER = 1 << 4, INT_VERTB = 1 << 5, INT_BLIT = 1 << 6, INT_AUD0 = 1 << 7,
    INT_RBF = 1 << 11, INT_DSKSYN = 1 << 12, INT_EXTER = 1 << 13, INT_INTEN = 1 << 14,
};
// DMACON bits
enum : uint16_t {
    DMA_AUD0 = 1 << 0, DMA_DISK = 1 << 4, DMA_SPR = 1 << 5, DMA_BLT = 1 << 6,
    DMA_COP = 1 << 7, DMA_BPL = 1 << 8, DMA_EN = 1 << 9, DMA_BZERO = 1 << 13,
    DMA_BBUSY = 1 << 14,
};

void request_irq(uint16_t bits);   // set bits in INTREQ
int irq_level();                   // 0..6 level the CPU should see

// Beam: the line being run, and how far through it the CPU has got.
int beam_line();
int beam_hpos();
bool pal_mode();
int lines_per_frame();

// ---- per-line hardware steps (custom.cpp drives these) ---------------------
void copper_frame_start();
void copper_jump(int which);                     // COPJMP1/COPJMP2 strobe
void copper_run_line(int line);                 // runs the copper over one line
void display_frame_start();
void display_line_begin(int line);              // sprite DMA for the line
void display_catch_up(int hpos);                // draw the line up to colour clock hpos
void display_line_end();
void display_sprite_reg(uint32_t off, uint16_t v);
void blitter_start();                            // BLTSIZE written
void paula_dma_changed(uint16_t old_dmacon, uint16_t new_dmacon);
void paula_reg_write(uint32_t off, uint16_t v);
void paula_run_line();                           // advances audio by one line
void cia_run_line();
void cia_frame();                                // vertical sync (CIA-A TOD)

// ---- CIAs ------------------------------------------------------------------
uint8_t cia_read(int which, int reg);    // which: 0 = CIA-A ($BFE001), 1 = CIA-B ($BFD000)
void cia_write(int which, int reg, uint8_t v);
void cia_reset();
bool cia_led_filter();                   // CIA-A drives the power LED, and with it the audio filter

// ---- input ------------------------------------------------------------------
struct Joystick {
    bool up, down, left, right, fire, fire2;
};
extern Joystick joy;                     // port 2 (the game's joystick port)
void key_event(int amiga_keycode, bool down);

// ---- audio out ----------------------------------------------------------------
constexpr int kAudioRate = 48000;
// How the four channels reach the speakers.
struct AudioOutput {
    enum Model { a500, a1200, clean } model = a500;   // whose analogue filters
    float separation = 0.7f;                          // 1: hard left/right, 0: mono
    float volume = 1.0f;
};
void set_audio_output(const AudioOutput &o);
// The menu's sound preview: the last few seconds of the game, frozen, then
// played round and round through the current output settings.
void audio_preview_begin();
int audio_preview_take(float *out, int frames);
// Interleaved stereo float samples produced since the last call.
int audio_take(float *out, int max_frames);

// ---- the whole machine ----------------------------------------------------------
void reset();
void run_frame();                        // one PAL/NTSC frame of emulated time

// CPU hooks the frame loop calls; implemented by the CPU backend.
void cpu_run(int cycles);
void cpu_irq_changed();
int cpu_cycles_this_line();

} // namespace amiga
