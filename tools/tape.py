"""The PC as the tape shelf of ESPNES's Famicom Data Recorder (see src/nes/tape.h).

The board keeps one tape in RAM; this moves tapes between it and files, over the serial console
(while a game runs). Opening the port does not reset the board.

    python3 tools/tape.py record FILE      start recording, Enter stops it and saves the tape
    python3 tools/tape.py take FILE        wait for a tape recorded on the board (F6, F7), save it
    python3 tools/tape.py put FILE         put a tape into the board's recorder
    python3 tools/tape.py play FILE        put it in and play it (F5 on the board does the same)
    python3 tools/tape.py info FILE        what is on a tape
    python3 tools/tape.py wav FILE OUT     the tape as sound (44100 Hz, 8 bit), to listen to or to
                                           record onto a real cassette

Options: --port /dev/ttyUSB0 (default), --baud 115200 (default).
The board's other console lines are shown as they come.
"""

import os
import select
import struct
import sys
import time

HEADER = 10
RECENT = 15


def parse(image):
    """(start level, CPU clock, [(a, b, n), ...]) of a tape image."""
    if len(image) < HEADER or image[:4] != b"FTAP" or image[4] != 1 or image[5] > 1:
        raise ValueError("not a tape image")
    level = image[5]
    clock = struct.unpack_from("<I", image, 6)[0]
    pos = HEADER
    recent = []
    runs = []

    def varint():
        nonlocal pos
        v = shift = 0
        while True:
            c = image[pos]
            pos += 1
            v |= (c & 0x7f) << shift
            shift += 7
            if not c & 0x80:
                return v

    while pos < len(image):
        token = image[pos]
        pos += 1
        i = token >> 4
        if i == RECENT:
            period = (varint(), varint())
        else:
            period = recent.pop(i)
        if i == RECENT and len(recent) == RECENT:
            recent.pop()
        recent.insert(0, period)
        n = (token & 15) + 1
        if n == 16:
            n = varint()
        runs.append(period + (n,))
    return level, clock, runs


def info(path):
    image = open(path, "rb").read()
    level, clock, runs = parse(image)
    cycles = sum((a + b) * n for a, b, n in runs)
    edges = sum(2 * n for a, b, n in runs)
    periods = {}
    for a, b, n in runs:
        periods[(a, b)] = periods.get((a, b), 0) + n
    print("%s: %d bytes, %.1f s (%s, %d Hz), starts at level %d, %d runs, %d level changes"
          % (path, len(image), cycles / clock, "PAL" if clock < 1700000 else "NTSC", clock,
             level, len(runs), edges))
    print("most used periods (cycles low+high, count, frequency):")
    for (a, b), n in sorted(periods.items(), key=lambda p: -p[1])[:8]:
        print("  %6d + %6d  x %6d   %7.1f Hz" % (a, b, n, clock / (a + b) if b else 0))


def wav(path, out, rate=44100):
    level, clock, runs = parse(open(path, "rb").read())
    samples = bytearray()
    t = 0.0                                  # time in cycles
    filled = 0                               # samples written
    def until(cycle, lvl):
        nonlocal filled
        end = int(cycle * rate / clock)
        samples.extend((0xc0 if lvl else 0x40,) * (end - filled))
        filled = max(filled, end)
    for a, b, n in runs:
        for _ in range(n):
            t += a
            until(t, level)
            if b:
                t += b
                until(t, level ^ 1)
    with open(out, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate, 1, 8))
        f.write(b"data" + struct.pack("<I", len(samples)) + samples)
    print("wrote %s: %.1f s" % (out, len(samples) / rate))


class Board:
    def __init__(self, port, baud):
        import serial
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = baud
        self.s.timeout = 0.1
        # Linux raises DTR and RTS when the port opens; both up is "run" for the board's reset
        # circuit, but lowering them one after the other (pyserial's default) resets it
        self.s.dtr = True
        self.s.rts = True
        self.s.open()
        self.buf = b""

    def send(self, data):
        self.s.write(data)

    def lines(self):
        """The console's lines, as they come; None every 0.1 s without one."""
        while True:
            self.buf += self.s.read(4096)
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                yield line.decode("latin-1").rstrip("\r")
            else:
                yield None

    def take(self, path, stop_on_enter=False):
        """Echoes the console until a tape arrives, saves it to path."""
        data = bytearray()
        for line in self.lines():
            if stop_on_enter and select.select([sys.stdin], [], [], 0)[0]:
                sys.stdin.readline()
                self.send(b"X")
                stop_on_enter = False
            if line is None:
                continue
            if line.startswith("Tape: data "):
                data += bytes.fromhex(line[11:])
                continue
            print(line)
            if line.startswith("Tape: end "):
                size = int(line[10:])
                if size != len(data):
                    sys.exit("tape incomplete: %d of %d bytes" % (len(data), size))
                open(path, "wb").write(data)
                print("saved %s" % path)
                info(path)
                return

    def put(self, path):
        image = open(path, "rb").read()
        parse(image)
        self.send(b"U%d\n" % len(image))
        for i in range(0, len(image), 64):
            self.send(image[i:i + 64].hex().encode() + b"\n")
        deadline = time.time() + 5
        for line in self.lines():
            if line is not None:
                print(line)
                if line.startswith("Tape: loaded"):
                    return
                if line.startswith("Tape: ") and not line.startswith("Tape: IN"):
                    sys.exit("the board did not take the tape")
            if time.time() > deadline:
                sys.exit("no answer from the board (is a game running?)")

    def echo(self):
        for line in self.lines():
            if line is not None:
                print(line)
                if line in ("Tape: END", "Tape: STOP"):
                    return


def main(args):
    port, baud = "/dev/ttyUSB0", 115200
    while args and args[0].startswith("--"):
        if args[0] == "--port":
            port = args[1]
        elif args[0] == "--baud":
            baud = int(args[1])
        else:
            sys.exit(__doc__)
        args = args[2:]
    if len(args) == 2 and args[0] == "info":
        return info(args[1])
    if len(args) == 3 and args[0] == "wav":
        return wav(args[1], args[2])
    if len(args) != 2 or args[0] not in ("record", "take", "put", "play"):
        sys.exit(__doc__)
    cmd, path = args
    board = Board(port, baud)
    try:
        if cmd == "record":
            board.send(b"R")
            print("recording, Enter stops")
            board.take(path, stop_on_enter=True)
        elif cmd == "take":
            print("waiting for a tape (F6 records on the board, F7 stops)")
            board.take(path)
        else:
            board.put(path)
            if cmd == "play":
                board.send(b"P")
                print("playing, Ctrl-C leaves (the tape plays on)")
                board.echo()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main(sys.argv[1:])
