"""Tape traps: games whose Data Recorder saves and loads ESPNES does in memory, without a tape.

The Wii Virtual Console did it so for Mach Rider: its versions call a stub instead of the tape
routines, which writes $96 to $6000 and $C3 to $6001 and waits until the two are equal; the
emulator saves or loads the data in between (Nes::vcCall). For the cart versions the same stub
is patched over the tape loader's entry, and the save's call is pointed at it. The return address
of the call (on the stack when $6001 is written) tells a save from a load.

Detected in the code, not by CRC, so translations and hacks are found too:

    Mach Rider   the save's call site: lda #$03 / sta $0604 / jsr SAVE / jmp MENU, the load's:
                 the other jsr LOAD / jmp MENU. The course is $0600-$063F and $0700-$07FF.

    Family BASIC (V2.0a, V2.1a, V3.0, Playbox BASIC) keeps its own data at $6000, so it calls
                 ESPNES through $4018 instead (Nes::portCall): its five tape routines (write
                 header, write data, read header, read data, verify data) become lda #OP /
                 jmp STUB, and the stub, over the bit writer that follows "write data",
                 is sta $4018 / wait: lda $4018 / bmi wait / cmp #1 / rts (carry set: failed, as
                 the routines return). Header at $0500 (128 bytes), data from its load address.

    python3 tools/tapetrap.py ROM.nes      what is found (and the host harness's trap= line)
"""

import re
import sys

# lda #$96 / sta $6000 / lda #$C3 / sta $6001 / wait: lda $6000 / cmp $6001 / bne wait / rts
STUB = bytes.fromhex("a9968d0060a9c38d0160ad0060cd0160d0f860")
VC_STUB = bytes.fromhex("a9968d0060a9c38d0160")

GAMES = ("machrider", "fbasic")


def prg_of(image):
    """The PRG ROM of an iNES image, or None."""
    if image[:4] != b"NES\x1a":
        return None
    start = 16 + (512 if image[6] & 4 else 0)
    return image[start:start + image[4] * 16384]


def machrider(prg):
    if len(prg) != 32768:                        # NROM-256, at $8000
        return None
    save = re.search(rb"\xa9\x03\x8d\x04\x06\x20(..)\x4c(..)", prg, re.S)
    if not save:
        return None
    saveSite = 0x8000 + save.start() + 5
    saveTarget = int.from_bytes(save.group(1), "little")
    menu = save.group(2)
    loads = [m for m in re.finditer(rb"\x20(..)\x4c" + re.escape(menu), prg, re.S)
             if 0x8000 + m.start() != saveSite]
    if len(loads) != 1:
        return None
    loadSite = 0x8000 + loads[0].start()
    loadTarget = int.from_bytes(loads[0].group(1), "little")
    trap = {
        "game": "machrider",
        "save": saveSite + 2,                     # what the JSR pushes: its last byte's address
        "load": loadSite + 2,
        "blocks": [(0x0600, 0x40), (0x0700, 0x100)],
        "patches": [],
    }
    if saveTarget == loadTarget:
        stub = prg[saveTarget - 0x8000:saveTarget - 0x8000 + 64]
        if VC_STUB not in stub:
            return None
        trap["kind"] = "vc"
        return trap
    # a cart: the tape loader must be there (waits for an edge on $4016 bit 1)
    if bytes.fromhex("ad16402902f0f6") not in prg:
        return None
    trap["kind"] = "cart"
    patches = [(loadTarget + i, b) for i, b in enumerate(STUB)]
    patches += [(saveSite + 1, loadTarget & 0xff), (saveSite + 2, loadTarget >> 8)]
    trap["patches"] = [(a, v, prg[a - 0x8000]) for a, v in patches]
    return trap


def fbasic(prg):
    if len(prg) != 32768:
        return None
    def one(pattern):
        found = list(re.finditer(pattern, prg, re.S))
        return 0x8000 + found[0].start() if len(found) == 1 else None
    writeHeader = one(rb"\x20..\x20..\xa9\x28\x85\x91\xa9\x00\x85\x92\x4c")
    writeData = one(rb"\x20..\x20..\xa9\x14\x85\x91\xa9\x00\x85\x92")
    readHeader = one(rb"\x20..\xa9\x00\x85\xb1\x20..\xa9\x28\x85\x91")
    verify = one(rb"\x20..\xa9\x01\x4c..\x20..\xa9\x00\x85\xb1\xa9\x00\x85\x92\x20..\xa9\x14\x85\x91")
    buffer = re.search(rb"\xa9\x00\x85\xad\xa9\x05\x85\xae\xa9\x80\x85\xaf\xa9\x05\x85\xb0", prg)
    fields = re.search(rb"\xad\x14\x05\x85\xad\x18\x6d\x12\x05", prg)
    if None in (writeHeader, writeData, readHeader, verify) or not buffer or not fields:
        return None
    stub = writeData + 14                         # the bit writer after "write data": dead now
    code = {}
    for entry, op in ((writeHeader, 1), (writeData, 2), (readHeader, 3), (verify + 8, 4), (verify, 5)):
        for i, b in enumerate((0xa9, op, 0x4c, stub & 0xff, stub >> 8)):
            code[entry + i] = b
    # sta $4018 / wait: lda $4018 / bmi wait / cmp #1 / rts
    for i, b in enumerate(bytes.fromhex("8d1840ad184030fbc90160")):
        code[stub + i] = b
    return {
        "game": "fbasic", "kind": "fbasic", "save": 0, "load": 0,
        "blocks": [(0x0500, 0x80)],
        "patches": [(a, v, prg[a - 0x8000]) for a, v in sorted(code.items())],
    }


def find(image, game=None):
    """The trap for a ROM image: a dict (game, kind, save, load, blocks, patches as (address,
    value, original)) or None. game: look only for that one ("machrider"), None: for all."""
    prg = prg_of(image)
    if prg is None:
        return None
    for name in GAMES:
        if game in (None, name):
            trap = globals()[name](prg)
            if trap:
                return trap
    return None


def host_line(trap):
    """The trap for host/nestool's trap= option: save,load;blocks;patches."""
    return "%04x,%04x;%s;%s" % (trap["save"], trap["load"],
        ",".join("%04x:%x" % b for b in trap["blocks"]),
        ",".join("%04x:%02x:%02x" % p for p in trap["patches"]))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    trap = find(open(sys.argv[1], "rb").read())
    if not trap:
        sys.exit("no tape trap for %s" % sys.argv[1])
    if trap["kind"] == "fbasic":
        print("%s: tape routines call ESPNES through $4018, %d patched bytes" % (trap["game"], len(trap["patches"])))
    else:
        print("%s (%s): save call returns to $%04X, load call to $%04X, %d patched bytes"
              % (trap["game"], trap["kind"], trap["save"], trap["load"], len(trap["patches"])))
    print("trap=" + host_line(trap))
