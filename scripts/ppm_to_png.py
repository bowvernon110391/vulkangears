#!/usr/bin/env python3
"""Convert a binary/ASCII PPM (P6/P3) to PNG using only the standard library.

Handy for turning the output of `vulkangears --headless --out shot.ppm` into
something a viewer (or an AI assistant's image tool) can actually open.

Usage:
    python scripts/ppm_to_png.py shot.ppm [shot.png]
"""

import struct
import sys
import zlib


def _read_token(data, pos):
    """Read the next whitespace-delimited token, skipping '#' comments."""
    while pos < len(data):
        ch = data[pos:pos + 1]
        if ch in b" \t\r\n":
            pos += 1
        elif ch == b"#":
            while pos < len(data) and data[pos:pos + 1] not in b"\r\n":
                pos += 1
        else:
            break
    start = pos
    while pos < len(data) and data[pos:pos + 1] not in b" \t\r\n":
        pos += 1
    return data[start:pos], pos


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()

    magic, pos = _read_token(data, 0)
    if magic not in (b"P6", b"P3"):
        raise ValueError("not a PPM file (expected P6 or P3, got %r)" % magic)

    width, pos = _read_token(data, pos)
    height, pos = _read_token(data, pos)
    maxval, pos = _read_token(data, pos)
    width, height, maxval = int(width), int(height), int(maxval)
    if maxval <= 0 or maxval > 255:
        raise ValueError("only 8-bit PPM is supported (maxval=%d)" % maxval)

    if magic == b"P6":
        pos += 1  # exactly one whitespace byte separates header from pixels
        expected = width * height * 3
        pixels = data[pos:pos + expected]
        if len(pixels) != expected:
            raise ValueError(
                "truncated pixel data: got %d bytes, expected %d" % (len(pixels), expected))
    else:
        values = []
        for _ in range(width * height * 3):
            tok, pos = _read_token(data, pos)
            values.append(int(tok))
        pixels = bytes(min(255, max(0, v * 255 // maxval)) for v in values)

    if maxval != 255 and magic == b"P6":
        pixels = bytes(min(255, v * 255 // maxval) for v in pixels)

    return width, height, pixels


def write_png(path, width, height, rgb):
    """Write an 8-bit RGB PNG. Each scanline is prefixed with filter type 0."""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as fh:
        fh.write(png)


def main(argv):
    if not 2 <= len(argv) <= 3:
        sys.stderr.write(__doc__)
        return 2

    src = argv[1]
    dst = argv[2] if len(argv) == 3 else src.rsplit(".", 1)[0] + ".png"

    width, height, rgb = read_ppm(src)
    write_png(dst, width, height, rgb)
    print("%s -> %s (%dx%d)" % (src, dst, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
