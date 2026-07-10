#!/usr/bin/env python3
"""Convert the device's 24-bit bottom-up BMP screenshot to PNG. Stdlib only (no Pillow).

Usage: bmp2png.py input.bmp output.png
"""

import struct
import sys
import zlib


def bmp_to_png(bmp_path, png_path):
    with open(bmp_path, "rb") as f:
        data = f.read()

    if data[:2] != b"BM":
        raise SystemExit(f"{bmp_path}: not a BMP file")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    planes, bpp = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if bpp != 24 or compression != 0 or planes != 1:
        raise SystemExit(f"{bmp_path}: expected uncompressed 24-bit BMP, got {bpp}bpp")

    bottom_up = height > 0
    height = abs(height)
    row_bytes = (width * 3 + 3) & ~3  # rows padded to 4 bytes

    # PNG scanlines: filter byte 0 + RGB triplets, top-down.
    raw = bytearray()
    for out_y in range(height):
        src_y = (height - 1 - out_y) if bottom_up else out_y
        row = data[pixel_offset + src_y * row_bytes :][: width * 3]
        raw.append(0)
        for x in range(width):
            b, g, r = row[x * 3 : x * 3 + 3]
            raw += bytes((r, g, b))

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(png_path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip())
    bmp_to_png(sys.argv[1], sys.argv[2])
