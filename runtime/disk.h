// Reading the player's James Pond 2 disk image: the file table, PowerPacker
// unpacking and the main program. No hardware state, so the build tools use
// it as well as the game.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace disk {

constexpr uint32_t kDirOffset = 0x2C00;    // file table: (block, length) pairs
constexpr uint32_t kImageBase = 0x300;     // where the main program lives
constexpr uint32_t kEntry = 0x500;
constexpr uint32_t kMusicBase = 0x34410;   // music player + tune, swapped per level
constexpr int kMusicFirst = 120, kMusicLast = 124;

// A disk image (.adf, or .adz / .zip holding one) or a WHDLoad install
// folder. On failure `error` says why in words for the player.
bool open(const char *path, std::string *error);
bool save_image(const std::string &path);   // the disk as a plain .adf
uint16_t crc16(const uint8_t *p, size_t n);
const std::vector<uint8_t> &adf();

bool file(int n, std::vector<uint8_t> &out);
uint32_t unpacked_size(int n);   // for files in the packed format, else 0

// The main program as it sits in memory from kImageBase.
bool main_program(std::vector<uint8_t> &out, std::string *error);

bool pp_decrunch(const uint8_t *buf, size_t eff_off, size_t end_off, std::vector<uint8_t> &out);

uint32_t crc32(const uint8_t *p, size_t n);

} // namespace disk
