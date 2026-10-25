#!/usr/bin/env python3
"""Pull the numbered game files (JP2_000...) out of a James Pond 2 disk image.

The disk keeps a table at the start of track 2 (byte 0x2C00): up to 128
entries of (start block, length in bytes), blocks being 512 bytes. This is
the same table the WHDLoad imager reads. Works on standard-format ADFs (the
Kixx / Skid Row release and its descendants).

usage: adf_extract.py disk.adf outdir
"""
import os
import struct
import sys

TABLE = 0x2C00
ENTRIES = (0x3000 - 0x2C00) // 8


def main():
    adf_path, out_dir = sys.argv[1], sys.argv[2]
    disk = open(adf_path, "rb").read()
    if len(disk) != 901120:
        sys.exit(f"{adf_path}: {len(disk)} bytes, expected a 901120-byte DD ADF")
    os.makedirs(out_dir, exist_ok=True)
    for n in range(ENTRIES):
        block, length = struct.unpack_from(">II", disk, TABLE + n * 8)
        if block == 0 and length == 0:
            continue
        start = block * 512
        if start + length > len(disk):
            print(f"JP2_{n:03d}: block {block} len {length} runs off the disk, skipped")
            continue
        data = disk[start:start + length]
        tag = data[:4]
        packed = " PP20" if tag == b"PP20" else ""
        with open(os.path.join(out_dir, f"JP2_{n:03d}"), "wb") as f:
            f.write(data)
        print(f"JP2_{n:03d}  block {block:4d}  {length:7d} bytes{packed}")


if __name__ == "__main__":
    main()
