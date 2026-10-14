"""Game pictures for the ROM list: PNG -> 64x64 in NES palette colours.

Plain Python (zlib only) on purpose: it runs inside PlatformIO's Python, which has no Pillow, and
the build should not need the network. The palette is read from src/nes/palette.h, so the
emulator and the pictures use the same colours.

Preview a conversion:  python3 tools/nesimage.py picture.png preview.ppm
"""

import os
import re
import sys
import zlib

SIZE = 64

_here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else None


def load_palette(palette_h):
    """The 64 NES colours as (r, g, b), from the RGB table in palette.h."""
    text = open(palette_h).read()
    body = text[text.index("NES_PALETTE_RGB"):]
    values = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{6})", body)[:64]]
    if len(values) != 64:
        raise ValueError("palette.h: expected 64 colours, found %d" % len(values))
    return [((v >> 16) & 255, (v >> 8) & 255, v & 255) for v in values]


def read_png(path):
    """Decodes a non-interlaced PNG into rows of (r, g, b, a). Raises ValueError if unsupported."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos, idat, plte, trns = 8, [], None, None
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big")
            depth, ctype, interlace = body[8], body[9], body[12]
        elif kind == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if width is None:
        raise ValueError("no IHDR")
    if interlace:
        raise ValueError("interlaced PNGs are not supported, save it without interlacing")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None:
        raise ValueError("unknown colour type %d" % ctype)

    raw = zlib.decompress(b"".join(idat))
    bits = channels * depth
    stride = (width * bits + 7) // 8
    bpp = max(1, bits // 8)
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(height):
        ftype = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 255
            elif ftype == 2:
                line[i] = (line[i] + b) & 255
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ftype == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line

    def samples(line):
        # the row as integers, one per channel value; 16 bit keeps the high byte
        if depth == 8:
            return list(line)
        if depth == 16:
            return list(line[0::2])
        per = 8 // depth
        out = []
        for byte in line:
            for k in range(per):
                out.append((byte >> (8 - depth * (k + 1))) & ((1 << depth) - 1))
        return out

    maxv = (1 << min(depth, 8)) - 1
    image = []
    for line in rows:
        s = samples(line)
        row = []
        for x in range(width):
            if ctype == 3:
                i = s[x]
                r, g, b = plte[i]
                a = trns[i] if trns and i < len(trns) else 255
            elif ctype in (0, 4):
                v = s[x * channels] * 255 // maxv
                r = g = b = v
                a = s[x * 2 + 1] * 255 // maxv if ctype == 4 else 255
            else:
                r, g, b = s[x * channels:x * channels + 3]
                a = s[x * 4 + 3] if ctype == 6 else 255
            row.append((r, g, b, a))
        image.append(row)
    return image


def fit(image, size=SIZE):
    """Scales into size x size keeping the aspect ratio (box average), centred on black.
    Transparent pixels become black."""
    sh, sw = len(image), len(image[0])
    scale = min(size / sw, size / sh)
    tw, th = max(1, round(sw * scale)), max(1, round(sh * scale))
    ox, oy = (size - tw) // 2, (size - th) // 2
    out = [[(0.0, 0.0, 0.0)] * size for _ in range(size)]
    for ty in range(th):
        y0 = ty * sh // th
        y1 = max(y0 + 1, (ty + 1) * sh // th)
        for tx in range(tw):
            x0 = tx * sw // tw
            x1 = max(x0 + 1, (tx + 1) * sw // tw)
            r = g = b = 0.0
            n = 0
            for y in range(y0, y1):
                row = image[y]
                for x in range(x0, x1):
                    pr, pg, pb, pa = row[x]
                    r += pr * pa / 255
                    g += pg * pa / 255
                    b += pb * pa / 255
                    n += 1
            out[oy + ty][ox + tx] = (r / n, g / n, b / n)
    return out


def quantize(pixels, palette):
    """Floyd-Steinberg dithering to the NES palette. Returns size*size NES colour numbers."""
    # one entry per distinct colour; for black prefer $0F, the colour games use
    choices = {}
    for i, c in enumerate(palette):
        if c not in choices or i == 0x0f:
            choices[c] = i
    colours = list(choices.items())

    size = len(pixels)
    work = [[list(p) for p in row] for row in pixels]
    out = bytearray(size * size)
    for y in range(size):
        for x in range(size):
            r, g, b = work[y][x]
            # distance weighted for how the eye sees it
            best, bestd = None, None
            for (cr, cg, cb), idx in colours:
                d = 2 * (r - cr) ** 2 + 4 * (g - cg) ** 2 + 3 * (b - cb) ** 2
                if bestd is None or d < bestd:
                    best, bestd, col = idx, d, (cr, cg, cb)
            out[y * size + x] = best
            err = (r - col[0], g - col[1], b - col[2])
            for dx, dy, w in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < size and ny < size:
                    t = work[ny][nx]
                    for k in range(3):
                        t[k] += err[k] * w / 16
    return bytes(out)


def convert(png_path, palette):
    return quantize(fit(read_png(png_path)), palette)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: nesimage.py picture.png preview.ppm")
    pal = load_palette(os.path.join(_here, "..", "src", "nes", "palette.h"))
    data = convert(sys.argv[1], pal)
    with open(sys.argv[2], "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (SIZE, SIZE))
        for i in data:
            f.write(bytes(pal[i]))
