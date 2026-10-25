/* Disassemble part of a 68000 image loaded at a fixed address.
 *
 * usage: dasm image.bin load_addr from to   (addresses in hex)
 */
#include <stdio.h>
#include <stdlib.h>
#include "m68k.h"

static unsigned char *img;
static unsigned int base, size;

static unsigned int rd8(unsigned int a)
{
    a -= base;
    return a < size ? img[a] : 0;
}

unsigned int m68k_read_disassembler_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd8(a) << 8 | rd8(a + 1); }
unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return m68k_read_disassembler_16(a) << 16 | m68k_read_disassembler_16(a + 2);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s image.bin load_addr from to\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc(size);
    fread(img, 1, size, f);
    fclose(f);
    base = strtoul(argv[2], 0, 16);
    unsigned int pc = strtoul(argv[3], 0, 16), end = strtoul(argv[4], 0, 16);
    char buf[256];
    while (pc < end) {
        unsigned int n = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("%06X  ", pc);
        for (unsigned int i = 0; i < 10; i += 2)
            printf(i < n ? "%04X " : "     ", m68k_read_disassembler_16(pc + i));
        printf(" %s\n", buf);
        pc += n;
    }
    return 0;
}
