#!/usr/bin/env python3
"""PowerPacker (PP20) decruncher for the James Pond 2 files.

Two layouts turn up on the disk:
  - the main program JP2_000: a small 68000 stub, then "PP20" + 4 efficiency
    bytes + packed data, its end given by the stub (see --main);
  - the packed data files: a 4-byte length (counted from the file start),
    4 efficiency bytes, packed data.
Both end with a long: decrunched size (top 24 bits) and bits to skip.

usage: ppdecrunch.py --main JP2_000 out.bin
       ppdecrunch.py JP2_001 out.bin
"""
import struct
import sys


def decrunch(buf, eff_off, end_off):
    eff = buf[eff_off:eff_off + 4]
    trailer = struct.unpack_from(">I", buf, end_off - 4)[0]
    out_len, skip = trailer >> 8, trailer & 0xFF
    pos = end_off - 4
    bits = 0
    count = 0

    def read(n):
        nonlocal pos, bits, count
        v = 0
        for _ in range(n):
            if count == 0:
                pos -= 4
                if pos < eff_off + 4:
                    raise ValueError("ran past the start of the packed data")
                bits = struct.unpack_from(">I", buf, pos)[0]
                count = 32
            v = (v << 1) | (bits & 1)
            bits >>= 1
            count -= 1
        return v

    read(skip)
    out = bytearray(out_len)
    w = out_len
    while w > 0:
        if read(1) == 0:
            n = 1
            while True:
                x = read(2)
                n += x
                if x != 3:
                    break
            for _ in range(n):
                w -= 1
                out[w] = read(8)
            if w <= 0:
                break
        x = read(2)
        n = x + 2
        if x == 3:
            off = read(eff[3] if read(1) else 7)
            while True:
                y = read(3)
                n += y
                if y != 7:
                    break
        else:
            off = read(eff[x])
        for _ in range(n):
            w -= 1
            out[w] = out[w + off + 1]
    return bytes(out)


def main():
    args = sys.argv[1:]
    if args[0] == "--main":
        buf = open(args[1], "rb").read()
        # the stub points a0 at the block with "lea d16(pc),a0" and loads its
        # length with "move.l #len,d0"; its own unpacker also contains a
        # cmp.l #"PP20", so don't search for the tag.
        i = buf.find(b"\x41\xfa")
        start = i + 2 + struct.unpack_from(">h", buf, i + 2)[0]
        i = buf.find(b"\x20\x3c", i)
        length = struct.unpack_from(">I", buf, i + 2)[0]
        if buf[start:start + 4] != b"PP20":
            sys.exit(f"no PP20 block at 0x{start:x}")
        data = decrunch(buf, start + 4, start + length)
        out = args[2]
    else:
        buf = open(args[0], "rb").read()
        length = struct.unpack_from(">I", buf, 0)[0]
        data = decrunch(buf, 4, length)
        out = args[1]
    open(out, "wb").write(data)
    print(f"{out}: {len(data)} bytes")


if __name__ == "__main__":
    main()
