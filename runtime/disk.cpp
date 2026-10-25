#include "disk.h"

#include <cstdio>
#include <cstring>

namespace disk {

static std::vector<uint8_t> image;

static uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool open(const char *adf_path, std::string *error)
{
    FILE *f = std::fopen(adf_path, "rb");
    if (!f) {
        *error = std::string("can't open ") + adf_path;
        return false;
    }
    image.assign(901120, 0);
    size_t got = std::fread(image.data(), 1, image.size(), f);
    std::fclose(f);
    if (got != image.size()) {
        *error = "not a standard 880 KB ADF";
        return false;
    }
    return true;
}

const std::vector<uint8_t> &adf() { return image; }

bool file(int n, std::vector<uint8_t> &out)
{
    if (n < 0 || n >= 128 || image.empty())
        return false;
    const uint8_t *e = image.data() + kDirOffset + n * 8;
    uint32_t start = be32(e) * 512, len = be32(e + 4);
    if (start + len > image.size())
        return false;
    out.assign(image.begin() + start, image.begin() + start + len);
    return true;
}

uint32_t unpacked_size(int n)
{
    std::vector<uint8_t> f;
    if (!file(n, f) || f.size() < 12)
        return 0;
    uint32_t packed = be32(f.data());
    if (packed < 12 || packed > f.size())
        return 0;
    return be32(&f[packed - 4]) >> 8;
}

// PowerPacker: eff_off points at the 4 efficiency bytes, end_off just past
// the trailing size word.
bool pp_decrunch(const uint8_t *buf, size_t eff_off, size_t end_off, std::vector<uint8_t> &out)
{
    const uint8_t *eff = buf + eff_off;
    uint32_t trailer = be32(buf + end_off - 4);
    uint32_t out_len = trailer >> 8;
    int skip = trailer & 0xFF;
    size_t pos = end_off - 4;
    uint32_t bits = 0;
    int count = 0;
    bool bad = false;
    auto read = [&](int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            if (count == 0) {
                if (pos < eff_off + 8) {
                    bad = true;
                    return v;
                }
                pos -= 4;
                bits = be32(buf + pos);
                count = 32;
            }
            v = v << 1 | (bits & 1);
            bits >>= 1;
            count--;
        }
        return v;
    };
    read(skip);
    out.assign(out_len, 0);
    int64_t w = out_len;
    while (w > 0 && !bad) {
        if (read(1) == 0) {
            int n = 1;
            for (;;) {
                int x = int(read(2));
                n += x;
                if (x != 3)
                    break;
            }
            for (int i = 0; i < n && w > 0; i++)
                out[--w] = uint8_t(read(8));
            if (w <= 0)
                break;
        }
        int x = int(read(2));
        int n = x + 2;
        uint32_t off;
        if (x == 3) {
            off = read(read(1) ? eff[3] : 7);
            for (;;) {
                int y = int(read(3));
                n += y;
                if (y != 7)
                    break;
            }
        } else {
            off = read(eff[x]);
        }
        for (int i = 0; i < n && w > 0; i++) {
            w--;
            if (w + off + 1 >= int64_t(out_len)) {
                bad = true;
                break;
            }
            out[w] = out[w + off + 1];
        }
    }
    return !bad;
}

bool main_program(std::vector<uint8_t> &out, std::string *error)
{
    std::vector<uint8_t> f;
    if (!file(0, f) || f.size() < 0x40) {
        *error = "no main program in the disk's file table";
        return false;
    }
    // The file starts with a small unpacker: "lea d16(pc),a0" points at the
    // PP20 block and "move.l #len,d0" gives its length.
    size_t lea = 0, mov = 0;
    for (size_t i = 0; i + 6 < 0x40; i += 2) {
        if (!lea && f[i] == 0x41 && f[i + 1] == 0xFA)
            lea = i;
        if (!mov && f[i] == 0x20 && f[i + 1] == 0x3C)
            mov = i;
    }
    if (!lea || !mov) {
        *error = "main program isn't the expected packed layout";
        return false;
    }
    size_t start = lea + 2 + int16_t(f[lea + 2] << 8 | f[lea + 3]);
    size_t len = be32(&f[mov + 2]);
    if (start + len > f.size() || std::memcmp(&f[start], "PP20", 4) != 0) {
        *error = "packed block not found";
        return false;
    }
    std::vector<uint8_t> unpacked;
    if (!pp_decrunch(f.data(), start + 4, start + len, unpacked) || unpacked.size() < 0x1000) {
        *error = "couldn't unpack the main program";
        return false;
    }
    // Unpacked, it runs at $4000 and copies itself from offset 6 to $300.
    out.assign(unpacked.begin() + 6, unpacked.end());
    return true;
}

uint32_t crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace disk
