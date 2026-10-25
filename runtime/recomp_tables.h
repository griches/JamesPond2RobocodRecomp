// What jp2-recomp generates: the translated code images and, for the
// differential test, one function per instruction.
#pragma once
#include <cstddef>

#include "recomp_rt.h"

namespace jp2 {

struct CodeEntry {
    uint32_t pc;
    uint32_t len;
    Handler fn;   // the chunk function that has an entry label for pc
};

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
