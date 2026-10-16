"""Family BASIC programs between tapes (.ftap, see src/nes/tape.h) and text listings.

Family BASIC saves to the Data Recorder in the Sharp MZ format: per block a leader of short
periods, a tape mark (long ones, then short ones), a long one, the bytes (each a long start bit
and 8 bits, MSB first; short = 0, long = 1) and a 16 bit checksum (the number of 1 bits, high
byte first). A file is two blocks: a 128 byte header (type, name, size, load address) and the
data. A program (type 2) is the tokenized lines as in memory at $6006:
    length (of the whole line), line number (16 bit), the tokens, 0;  a length of 0 ends it
Keywords are one byte ($80-$FD, the table below, as in the ROM of V3.0; V2 has a subset with the
same values), numbers 0-9 are $01-$0A, larger ones $12 + 16 bit, &H numbers $11 + 16 bit, line
numbers after GOTO, GOSUB, THEN, RESTORE and in their lists $0B + 16 bit. Text in strings,
after REM, ' and DATA stays as typed. Characters outside ASCII are written as {XX} (hex).

    python3 tools/fbasic.py list TAPE.ftap             the programs on a tape (files of other
                                                       types are listed by name and size)
    python3 tools/fbasic.py export TAPE.ftap OUT.bas   the first program as text
    python3 tools/fbasic.py import IN.bas OUT.ftap [NAME]
                                                       a tape with the program (NAME: the file's
                                                       name for LOAD "NAME", else the file name)
With the board (a game with the Famicom keyboard running, see tools/tape.py):
    python3 tools/fbasic.py send IN.bas                puts the program's tape in, types LOAD
                                                       and plays it
    python3 tools/fbasic.py receive OUT.bas [NAME]     records, types SAVE "NAME", and when
                                                       you press Enter (after BASIC's OK) takes
                                                       the tape and writes the listing
"""

import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tape

KEYWORDS = {
    0x80: "GOTO", 0x81: "GOSUB", 0x82: "RUN", 0x83: "RETURN", 0x84: "RESTORE", 0x85: "THEN",
    0x86: "LIST", 0x87: "SYSTEM", 0x88: "TO", 0x89: "STEP", 0x8a: "SPRITE", 0x8b: "PRINT",
    0x8c: "FOR", 0x8d: "NEXT", 0x8e: "PAUSE", 0x8f: "INPUT", 0x90: "LINPUT", 0x91: "DATA",
    0x92: "IF", 0x93: "READ", 0x94: "DIM", 0x95: "REM", 0x96: "STOP", 0x97: "CONT", 0x98: "CLS",
    0x99: "CLEAR", 0x9a: "ON", 0x9b: "OFF", 0x9c: "CUT", 0x9d: "NEW", 0x9e: "POKE", 0x9f: "CGSET",
    0xa0: "VIEW", 0xa1: "MOVE", 0xa2: "END", 0xa3: "PLAY", 0xa4: "BEEP", 0xa5: "LOAD",
    0xa6: "SAVE", 0xa7: "POSITION", 0xa8: "KEY", 0xa9: "COLOR", 0xaa: "DEF", 0xab: "CGEN",
    0xac: "SWAP", 0xad: "CALL", 0xae: "LOCATE", 0xaf: "PALET", 0xb0: "ERA", 0xb1: "TR",
    0xb2: "FIND", 0xb3: "GAME", 0xb4: "BGTOOL", 0xb5: "AUTO", 0xb6: "DELETE", 0xb7: "RENUM",
    0xb8: "FILTER", 0xb9: "CLICK", 0xba: "SCREEN", 0xbb: "BACKUP", 0xbc: "ERROR",
    0xbd: "RESUME", 0xbe: "BGPUT", 0xbf: "BGGET", 0xc0: "CAN",
    0xca: "ABS", 0xcb: "ASC", 0xcc: "STR$", 0xcd: "FRE", 0xce: "LEN", 0xcf: "PEEK", 0xd0: "RND",
    0xd1: "SGN", 0xd2: "SPC", 0xd3: "TAB", 0xd4: "MID$", 0xd5: "STICK", 0xd6: "STRIG",
    0xd7: "XPOS", 0xd8: "YPOS", 0xd9: "VAL", 0xda: "POS", 0xdb: "CSRLIN", 0xdc: "CHR$",
    0xdd: "HEX$", 0xde: "INKEY$", 0xdf: "RIGHT$", 0xe0: "LEFT$", 0xe1: "SCR$", 0xe2: "INSTR",
    0xe3: "CRASH", 0xe4: "ERR", 0xe5: "ERL", 0xe6: "VCT",
    0xef: "XOR", 0xf0: "OR", 0xf1: "AND", 0xf2: "NOT", 0xf3: "<>", 0xf4: ">=", 0xf5: "<=",
    0xf6: "=", 0xf7: ">", 0xf8: "<", 0xf9: "+", 0xfa: "-", 0xfb: "MOD", 0xfc: "/", 0xfd: "*",
}
TOKENS = {name: code for code, name in KEYWORDS.items()}
BY_LENGTH = sorted(TOKENS, key=len, reverse=True)
LINE_NUMBER_AFTER = {"GOTO", "GOSUB", "THEN", "RESTORE"}
RAW_AFTER = {"REM", "DATA"}

TYPE_PROGRAM = 2
LOAD_ADDRESS = 0x6006

# the periods Family BASIC writes, in CPU cycles (NTSC): low half, high half
SHORT = (421, 459)
LONG = (853, 921)
CLOCK = 1789773


###############################################################################
# tape signal <-> blocks

def blocks_of(image):
    """The data blocks on a tape (checksums checked), as a list of bytes."""
    level, clock, runs = tape.parse(image)
    scale = CLOCK / clock
    bits = []
    for a, b, n in runs:
        p = (a + b) * scale
        bits.append(("s" if 700 < p < 1300 else "l" if 1300 < p < 2400 else "x") * min(n, 100000))
    signal = "".join(bits)
    blocks = []
    for m in re.finditer(r"l{15,}s{15,}l", signal):
        i, data = m.end(), bytearray()
        while i + 9 <= len(signal) and signal[i] == "l" and set(signal[i + 1:i + 9]) <= {"s", "l"}:
            data.append(int(signal[i + 1:i + 9].replace("s", "0").replace("l", "1"), 2))
            i += 9
        blocks.append(bytes(data))
    return blocks


def ones(data):
    return sum(bin(b).count("1") for b in data)


def files_of(image):
    """The files on a tape: a list of (header dict, data bytes)."""
    blocks = blocks_of(image)
    files = []
    for k in range(len(blocks) - 1):
        h, d = blocks[k], blocks[k + 1]
        if len(h) < 130 or h[0] == 0 or h[0] > 0x10:
            continue
        if ones(h[:128]) != (h[128] << 8 | h[129]):
            continue
        size, load = struct.unpack_from("<HH", h, 18)
        if len(d) < size + 2 or ones(d[:size]) != (d[size] << 8 | d[size + 1]):
            raise ValueError("file %r: the data block is broken" % name_of(h))
        files.append(({"type": h[0], "name": name_of(h), "size": size, "load": load}, d[:size]))
    return files


def name_of(header):
    name = header[1:17].split(b"\x00")[0]
    return name.decode("latin-1").rstrip()


def tape_of(files, leader=2000):
    """A tape image with files [(type, name, data)], written as Family BASIC writes them (with
    a shorter leader: 2000 periods, a second, instead of 9)."""
    runs = []

    def periods(p, n):
        if runs and runs[-1][:2] == p:
            runs[-1][2] += n
        else:
            runs.append([p[0], p[1], n])

    def byte(v):
        periods(LONG, 1)
        for bit in range(7, -1, -1):
            periods(LONG if v >> bit & 1 else SHORT, 1)

    def block(data, mark):
        periods(SHORT, leader)
        periods(LONG, mark)
        periods(SHORT, mark)
        periods(LONG, 1)
        for v in data:
            byte(v)
        c = ones(data)
        byte(c >> 8)
        byte(c & 0xff)
        periods(LONG, 1)

    for kind, name, data in files:
        header = bytearray(128)
        header[0] = kind
        raw = name.encode("latin-1")[:16]
        header[1:17] = raw + (b"\x00" + b" " * 15 if len(raw) < 16 else b"")[:16 - len(raw)]
        struct.pack_into("<HH", header, 18, len(data), LOAD_ADDRESS)
        block(bytes(header), 40)
        block(data, 20)
    return tape.encode(0, CLOCK, runs)


###############################################################################
# program <-> text

def char(c):
    return chr(c) if 0x20 <= c < 0x7f and c != 0x7b else "{%02X}" % c


def detokenize(data):
    lines, i = [], 0
    while i + 3 <= len(data) and data[i]:
        length, number = data[i], data[i + 1] | data[i + 2] << 8
        body, i = data[i + 3:i + length - 1], i + length
        out, k, mode, quoted = [], 0, None, False
        while k < len(body):
            c = body[k]
            k += 1
            if mode == "REM":                    # raw to the line end
                out.append(char(c))
            elif mode == "DATA":                 # raw to a : outside quotes
                out.append(char(c))
                if c == 0x22:
                    quoted = not quoted
                elif c == 0x3a and not quoted:
                    mode = None
            elif c == 0x22:                      # a string, raw to its end
                end = body.find(b"\x22", k)
                end = len(body) if end < 0 else end + 1
                out.append('"' + "".join(char(x) for x in body[k:end]))
                k = end
            elif 0x01 <= c <= 0x0a:
                out.append(str(c - 1))
            elif c in (0x0b, 0x12):
                out.append(str(body[k] | body[k + 1] << 8))
                k += 2
            elif c == 0x11:
                out.append("&H%X" % (body[k] | body[k + 1] << 8))
                k += 2
            elif c in KEYWORDS:
                out.append(KEYWORDS[c])
                if KEYWORDS[c] in RAW_AFTER:
                    mode, quoted = KEYWORDS[c], False
            else:
                out.append(char(c))
                if c == 0x27:                    # ': a remark
                    mode = "REM"
        lines.append("%d %s" % (number, "".join(out)))
    return lines


def tokenize_line(text):
    m = re.match(r"\s*(\d+) ?(.*)$", text)
    if not m:
        raise ValueError("no line number: %r" % text)
    number, rest = int(m.group(1)), m.group(2)
    # {XX} back to bytes
    src = []
    k = 0
    while k < len(rest):
        e = re.match(r"\{([0-9A-Fa-f]{2})\}", rest[k:])
        if e:
            src.append(int(e.group(1), 16))
            k += 4
        else:
            src.append(ord(rest[k]))
            k += 1
    out = bytearray()
    k, raw, quoted, lineNumbers = 0, None, False, False
    while k < len(src):
        c = src[k]
        if raw:                                  # REM, ': to the line end; DATA: to a :
            out.append(c)
            k += 1
            if c == 0x22:
                quoted = not quoted
            elif raw == "DATA" and c == 0x3a and not quoted:
                raw = None
            continue
        if c == 0x22:                            # a string, to its end
            end = k + 1
            while end < len(src) and src[end] != 0x22:
                end += 1
            out += bytes(src[k:end + 1])
            k = end + 1
            continue
        if c == 0x27:
            out.append(c)
            raw = "REM"
            k += 1
            continue
        if c == ord("?"):
            out.append(TOKENS["PRINT"])
            k += 1
            continue
        here = bytes(src[k:]).decode("latin-1")
        word = next((w for w in BY_LENGTH if here.startswith(w)), None)
        if word:
            out.append(TOKENS[word])
            k += len(word)
            if word in RAW_AFTER:
                raw, quoted = word, False
            lineNumbers = word in LINE_NUMBER_AFTER
            continue
        if here.startswith("&H"):
            m = re.match(r"&H([0-9A-F]{1,4})", here)
            out += bytes([0x11]) + struct.pack("<H", int(m.group(1), 16))
            k += len(m.group(0))
            continue
        # a number, unless it is part of a name (A1)
        if chr(c).isdigit() and not (out and out[-1] < 0x80 and (chr(out[-1]).isalpha() or out[-1] == 0x24)):
            m = re.match(r"\d+", here)
            v = int(m.group(0))
            if v > 0xffff:
                raise ValueError("line %d: number %d too large" % (number, v))
            if lineNumbers:
                out += bytes([0x0b]) + struct.pack("<H", v)
            elif len(m.group(0)) == 1:
                out.append(v + 1)
            else:
                out += bytes([0x12]) + struct.pack("<H", v)
            k += len(m.group(0))
            continue
        if c not in (0x20, 0x2c):                # a list of line numbers goes on after , and spaces
            lineNumbers = False
        out.append(c)
        k += 1
    if len(out) + 4 > 255:
        raise ValueError("line %d is too long" % number)
    return bytes([len(out) + 4]) + struct.pack("<H", number) + bytes(out) + b"\x00"


def tokenize(text):
    lines = [l for l in text.replace("\r", "").split("\n") if l.strip()]
    numbered = sorted((tokenize_line(l) for l in lines), key=lambda b: b[1] | b[2] << 8)
    return b"".join(numbered) + b"\x00"


###############################################################################

def main(args):
    if len(args) == 2 and args[0] == "list":
        for header, data in files_of(open(args[1], "rb").read()):
            print('"%s": type %d, %d bytes' % (header["name"], header["type"], header["size"]))
            if header["type"] == TYPE_PROGRAM:
                print("\n".join(detokenize(data)))
        return
    if len(args) == 3 and args[0] == "export":
        programs = [(h, d) for h, d in files_of(open(args[1], "rb").read()) if h["type"] == TYPE_PROGRAM]
        if not programs:
            sys.exit("no program on %s" % args[1])
        h, d = programs[0]
        open(args[2], "w").write("\n".join(detokenize(d)) + "\n")
        print('wrote %s: "%s", %d bytes' % (args[2], h["name"], h["size"]))
        return
    if len(args) in (3, 4) and args[0] == "import":
        name = args[3] if len(args) == 4 else os.path.splitext(os.path.basename(args[1]))[0].upper()
        data = tokenize(open(args[1]).read())
        open(args[2], "wb").write(tape_of([(TYPE_PROGRAM, name, data)]))
        print('wrote %s: "%s", %d bytes of program' % (args[2], name, len(data)))
        return
    if len(args) == 2 and args[0] == "send":
        name = os.path.splitext(os.path.basename(args[1]))[0].upper()
        image = tape_of([(TYPE_PROGRAM, name, tokenize(open(args[1]).read()))])
        temp = "/tmp/fbasic-send.ftap" if os.path.isdir("/tmp") else "fbasic-send.ftap"
        open(temp, "wb").write(image)
        board = tape.Board("/dev/ttyUSB0", 115200)
        board.put(temp)
        board.send(b"TLOAD|\n")
        time.sleep(1.5)
        board.send(b"P")
        print('playing "%s"; the board says when the tape ends' % name)
        board.echo()
        return
    if len(args) in (2, 3) and args[0] == "receive":
        name = args[2] if len(args) == 3 else os.path.splitext(os.path.basename(args[1]))[0].upper()
        board = tape.Board("/dev/ttyUSB0", 115200)
        board.send(b"R")
        time.sleep(0.3)
        board.send(b'TSAVE "%s"|\n' % name.encode("latin-1"))
        print("saving; press Enter when BASIC says OK")
        temp = "/tmp/fbasic-receive.ftap" if os.path.isdir("/tmp") else "fbasic-receive.ftap"
        board.take(temp, stop_on_enter=True)
        return main(["export", temp, args[1]])
    sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
