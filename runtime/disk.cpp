#include "disk.h"

extern "C" {
#include "puff.h"
}

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace disk {

static std::vector<uint8_t> image;

static uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Paths are UTF-8 (SDL's), but Windows' fopen reads them in the local code
// page: a disk in C:\Users\José\Downloads wouldn't open. (No SDL here: the
// recompiler reads disks too.)
static FILE *open_file(const std::string &path, const char *mode)
{
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0)
        return nullptr;
    std::vector<wchar_t> wpath(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), n);
    wchar_t wmode[8] = {};
    for (int i = 0; mode[i] && i < 7; i++)
        wmode[i] = wchar_t(mode[i]);
    return _wfopen(wpath.data(), wmode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

static bool read_whole(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = open_file(path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && n < (64L << 20);
    if (ok) {
        out.resize(size_t(n));
        ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    return ok;
}

static uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

// A raw deflate stream into `size` bytes.
static bool inflate(const uint8_t *src, size_t len, size_t size, std::vector<uint8_t> &out)
{
    out.assign(size, 0);
    unsigned long dlen = size, slen = len;
    return puff(out.data(), &dlen, src, &slen) == 0 && dlen == size;
}

// The 128 files a WHDLoad install keeps as data/jp2_000 ... jp2_127, laid out
// as a disk again: the file table at $2C00, the files after it.
// 0: not a WHDLoad folder, 1: read, -1: a WHDLoad folder that didn't read.
static int from_whdload(const std::string &dir, std::string *error)
{
    std::string base;
    for (const char *sub : {"/data/jp2_", "/data/JP2_", "/jp2_", "/JP2_"}) {
        std::vector<uint8_t> probe;
        if (read_whole(dir + sub + "000", probe)) {
            base = dir + sub;
            break;
        }
    }
    if (base.empty())
        return 0;
    image.assign(901120, 0);
    uint32_t block = (kDirOffset + 128 * 8) / 512;
    for (int n = 0; n < 128; n++) {
        char num[8];
        std::snprintf(num, sizeof num, "%03d", n);
        std::vector<uint8_t> f;
        if (!read_whole(base + num, f))
            f.clear();   // the disk has a few empty entries
        uint32_t at = block * 512;
        if (at + f.size() > image.size()) {
            *error = "the WHDLoad files don't fit on one disk";
            return -1;
        }
        std::memcpy(&image[at], f.data(), f.size());
        uint8_t *e = &image[kDirOffset + n * 8];
        for (int k = 0; k < 4; k++) {
            e[k] = uint8_t(block >> (24 - 8 * k));
            e[4 + k] = uint8_t(f.size() >> (24 - 8 * k));
        }
        block += uint32_t((f.size() + 511) / 512);
    }
    return 1;
}

// The bytes of one disk image, whatever it came wrapped in.
static bool unwrap(std::vector<uint8_t> data, int depth, std::string *error)
{
    auto starts = [&](const char *m) { return data.size() >= std::strlen(m) && std::memcmp(data.data(), m, std::strlen(m)) == 0; };
    if (data.size() == 901120) {
        image = std::move(data);
        return true;
    }
    if (depth < 2 && data.size() > 18 && data[0] == 0x1F && data[1] == 0x8B && data[2] == 8) {   // gzip (.adz)
        size_t at = 10;
        uint8_t flags = data[3];
        if (flags & 4)
            at += 2 + le16(&data[at]);
        for (int bit : {8, 16})   // name, comment
            if (flags & bit)
                while (at < data.size() && data[at++]) {
                }
        if (flags & 2)
            at += 2;
        uint32_t size = le32(&data[data.size() - 4]);
        std::vector<uint8_t> out;
        if (at < data.size() && size <= (16u << 20) && inflate(&data[at], data.size() - 8 - at, size, out))
            return unwrap(std::move(out), depth + 1, error);
        *error = "the compressed disk image is damaged";
        return false;
    }
    if (depth < 2 && starts("PK\x03\x04")) {   // zip: the first entry that unpacks to a disk image
        size_t eocd = data.size() >= 22 ? data.size() - 22 : 0;
        while (eocd > 0 && le32(&data[eocd]) != 0x06054B50)
            eocd--;
        if (le32(&data[eocd]) == 0x06054B50) {
            size_t cd = le32(&data[eocd + 16]), count = le16(&data[eocd + 10]);
            for (size_t i = 0; i < count && cd + 46 <= data.size(); i++) {
                if (le32(&data[cd]) != 0x02014B50)
                    break;
                uint16_t method = le16(&data[cd + 10]);
                uint32_t csize = le32(&data[cd + 20]), usize = le32(&data[cd + 24]);
                size_t name_len = le16(&data[cd + 28]), extra = le16(&data[cd + 30]), comment = le16(&data[cd + 32]);
                size_t local = le32(&data[cd + 42]);
                cd += 46 + name_len + extra + comment;
                if (local + 30 > data.size() || usize > (16u << 20) || (method != 0 && method != 8))
                    continue;
                size_t at = local + 30 + le16(&data[local + 26]) + le16(&data[local + 28]);
                if (at + csize > data.size())
                    continue;
                std::vector<uint8_t> out;
                if (method == 0)
                    out.assign(data.begin() + long(at), data.begin() + long(at + csize));
                else if (!inflate(&data[at], csize, usize, out))
                    continue;
                std::string ignore;
                if (unwrap(std::move(out), depth + 1, &ignore))
                    return true;
            }
        }
        *error = "there's no James Pond 2 disk image in that zip";
        return false;
    }
    if (starts("UAE-1ADF") || starts("UAE--ADF"))
        *error = "this is a raw-track image (extended ADF), such as a dump of the original copy-protected disk; "
                 "it isn't supported yet";
    else if (starts("DMS!"))
        *error = "DMS images aren't supported: turn it into an ADF first (xDMS or WinUAE can)";
    else if (starts("CAPS"))
        *error = "IPF images aren't supported";
    else if (data.size() > 7 && data[2] == '-' && data[3] == 'l' && data[4] == 'h')
        *error = "that's an LHA archive: unpack it and choose the folder instead";
    else
        *error = "that isn't an Amiga disk image";
    return false;
}

bool open(const char *path, std::string *error)
{
    image.clear();
    int whd = from_whdload(path, error);
    if (whd < 0) {
        image.clear();
        return false;
    }
    if (!whd) {
        std::vector<uint8_t> data;
        if (!read_whole(path, data)) {
            *error = std::string("can't read ") + path;
            return false;
        }
        if (!unwrap(std::move(data), 0, error))
            return false;
    }
    // A James Pond 2 disk: its main program unpacks.
    std::vector<uint8_t> prog, f;
    if (!main_program(prog, error)) {
        // The original Millennium release (as WHDLoad installs it) wraps its
        // main program differently. Its game code is the same, but it isn't
        // supported yet.
        if (file(0, f) && crc16(f.data(), f.size()) == 0x6E83)
            *error = "this is the original Millennium release, which isn't supported yet. The Kixx re-release, "
                     "its WHDLoad install and the common cracked disks are";
        else
            *error = "this isn't the Amiga (OCS) James Pond 2 disk";
        image.clear();
        return false;
    }
    return true;
}

bool save_image(const std::string &path)
{
    FILE *f = open_file(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(image.data(), 1, image.size(), f) == image.size();
    return std::fclose(f) == 0 && ok;
}

// The CRC-16 WHDLoad tells releases apart with (CRC-16/ARC).
uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = c & 1 ? uint16_t((c >> 1) ^ 0xA001) : uint16_t(c >> 1);
    }
    return c;
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
