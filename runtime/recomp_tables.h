// What jp2-recomp generates: the translated code images and, for the
// differential test, one function per instruction.
#pragma once
#include <cstddef>

#include "recomp_rt.h"

namespace jp2 {

struct CodeEntry {
    uint32_t pc;
    uint32_t len;
    Handler fn;     // the chunk function that has an entry label for pc
    uint32_t sum;   // code_sum of the bytes it was translated from
};

// A checksum of an instruction's bytes (FNV-1a), so a disk whose code differs
// here and there (another release, another crack) still runs translated
// wherever it matches.
inline uint32_t code_sum(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

struct CodeImage {
    int file;            // -1 = main program, else the disk file it came from
    uint32_t base;
    uint32_t size;
    uint32_t crc;        // of the bytes it was translated from
    const CodeEntry *entries;
    size_t count;
};

extern const CodeImage code_images[];
extern const int code_image_count;

struct TestEntry {
    int image;
    uint32_t pc;
    Handler fn;
};

extern const TestEntry tests[];
extern const int test_count;

} // namespace jp2
