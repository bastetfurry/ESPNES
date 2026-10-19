#!/usr/bin/env python3
"""A screenshot of ESPNES over the serial console, as a PNG of the 256x240 picture.

    tools/screenshot.py [out.png] [--port /dev/ttyUSB0] [--keys "ddds"]

Sends the keys first (the serial console's letters, e.g. d = Down, s = Start), each 0.3 s apart,
then 'Z'; the board answers with 240 lines "Screen: " + 256 characters, '0' + the raw pixel's
colour bits (RGB222: red in bits 0-1, green 2-3, blue 4-5). Works in the menus and in games.
"""

import argparse
import time
import zlib
import struct

import serial


def write_png(path, width, height, rgb_rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + bytes(row) for row in rgb_rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="screen.png")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--keys", default="")
    args = ap.parse_args()

    s = serial.Serial()
    s.port = args.port
    s.baudrate = 115200
    s.timeout = 0.5
    # both up: opening the port must not pulse the board's reset (see tools/tape.py)
    s.dtr = True
    s.rts = True
    s.open()
    time.sleep(0.2)
    for k in args.keys:
        s.write(k.encode())
        time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b"Z")

    rows = []
    deadline = time.time() + 20
    while len(rows) < 240 and time.time() < deadline:
        line = s.readline().decode("latin-1").strip()
        if line.startswith("Screen: "):
            px = line[8:]
            if len(px) != 256:
                raise SystemExit("bad line %d" % len(rows))
            row = []
            for c in px:
                v = ord(c) - 48
                for shift in (0, 2, 4):
                    row.append(((v >> shift) & 3) * 85)
            rows.append(row)
    if len(rows) < 240:
        raise SystemExit("only %d lines" % len(rows))
    write_png(args.out, 256, 240, rows)
    print(args.out)


if __name__ == "__main__":
    main()
