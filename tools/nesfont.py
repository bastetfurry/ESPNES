"""The firmware's fonts as PNG pictures, for drawing them in a paint program (e.g. Aseprite).

A font picture is 16 x 16 glyphs in code page 437 order: glyph n is in row n // 16, column n % 16
(so "A", 65, is in row 4, column 1). Glyphs touch, there is no gap between them; a grid of the
glyph size in the paint program shows them. A pixel is set when it is light (and not transparent);
the exported pictures are black and white with a 2 colour palette, but any colours work.

    fonts/font8x8.png   128 x 128   menu, manuals, key mapper, messages over the game
    fonts/font4x6.png    64 x  96   the small labels on the drawn pad (SELECT, START)

tools/embed_roms.py converts them on every build (src/generated/fonts.cpp).

    python3 tools/nesfont.py export          FabGL's original fonts into fonts/ (overwrites!)
    python3 tools/nesfont.py show font.png   the glyphs 32-127 of a font picture in the terminal
"""

import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nesimage

# name: (glyph width, glyph height, FabGL's font it was made from)
FONTS = {
    "font8x8": (8, 8, "lib/FabGL/src/fonts/font_8x8.h"),
    "font4x6": (4, 6, "lib/FabGL/src/fonts/font_4x6.h"),
}


def size_of(png_path):
    """(glyph width, glyph height) for a font picture, from its name."""
    name = os.path.splitext(os.path.basename(png_path))[0]
    if name not in FONTS:
        raise ValueError("%s: unknown font (known: %s)" % (png_path, ", ".join(FONTS)))
    return FONTS[name][:2]


def from_png(png_path):
    """The font as bytes: per glyph one byte per row, the leftmost pixel in bit 7 (FabGL's form)."""
    w, h = size_of(png_path)
    image = nesimage.read_png(png_path)
    if len(image) != 16 * h or len(image[0]) != 16 * w:
        raise ValueError("%s is %dx%d, a font of %dx%d glyphs must be %dx%d"
                         % (png_path, len(image[0]), len(image), w, h, 16 * w, 16 * h))
    out = bytearray()
    for n in range(256):
        gx, gy = (n % 16) * w, (n // 16) * h
        for y in range(h):
            bits = 0
            for x in range(w):
                r, g, b, a = image[gy + y][gx + x]
                if a >= 128 and r * 299 + g * 587 + b * 114 >= 128 * 1000:
                    bits |= 0x80 >> x
            out.append(bits)
    return bytes(out)


def write_png(path, width, height, pixels, palette):
    """An 8 bit indexed PNG; pixels is a list of rows of palette indices."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\x00" + bytes(row) for row in pixels)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)))
        f.write(chunk(b"PLTE", b"".join(bytes(c) for c in palette)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def to_png(data, w, h, png_path):
    rows = [[0] * (16 * w) for _ in range(16 * h)]
    for n in range(256):
        gx, gy = (n % 16) * w, (n // 16) * h
        for y in range(h):
            bits = data[n * h + y]
            for x in range(w):
                if bits & (0x80 >> x):
                    rows[gy + y][gx + x] = 1
    write_png(png_path, 16 * w, 16 * h, rows, [(0, 0, 0), (255, 255, 255)])


def from_fabgl(header_path, h):
    """The first byte array of a FabGL font header."""
    text = open(header_path).read()
    body = text[text.index("{", text.index("_DATA[]")) + 1:]
    body = body[:body.index("}")]
    data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", body))
    if len(data) != 256 * h:
        raise ValueError("%s: %d bytes, expected %d" % (header_path, len(data), 256 * h))
    return data


def show(png_path):
    w, h = size_of(png_path)
    data = from_png(png_path)
    for first in range(32, 128, 16):
        for y in range(h):
            line = ""
            for n in range(first, first + 16):
                bits = data[n * h + y]
                line += "".join("#" if bits & (0x80 >> x) else "." for x in range(w)) + " "
            print(line)
        print()


if __name__ == "__main__":
    project = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if len(sys.argv) == 2 and sys.argv[1] == "export":
        os.makedirs(os.path.join(project, "fonts"), exist_ok=True)
        for name, (w, h, header) in FONTS.items():
            out = os.path.join(project, "fonts", name + ".png")
            to_png(from_fabgl(os.path.join(project, header), h), w, h, out)
            print("wrote", out)
    elif len(sys.argv) == 3 and sys.argv[1] == "show":
        show(sys.argv[2])
    else:
        sys.exit(__doc__)
