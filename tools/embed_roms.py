# PlatformIO pre-build script: builds the games in roms/ into the firmware.
#
# roms/settings.csv lists the games in menu order, one per line:
#   "file name.nes", "name shown in the menu", "picture.png", "manual.txt", options
# (NSF music files, .nsf, are listed the same way.)
# Lines starting with # are comments; a game's line commented out leaves that game out of the build
# (its file can stay in roms/). Options are "key=value" pairs separated by ';' (see
# RomConfig in src/main.cpp: save, region, overclock). A ROM in roms/
# that the CSV does not list is still built in, after the listed ones, named after its file.
#
# Writes into src/generated/:
#   roms.S         .incbin's each ROM into flash rodata
#   romindex.cpp   the table the ROM list reads (see src/romindex.h), with the pictures
#   pics/          converted pictures, a cache so unchanged PNGs are not converted again
# Constant data in flash is mapped into the address space through the cache, so the emulator reads
# the ROMs in place: no RAM, no copying, no flash writes. Files are only rewritten when they
# change, so an unchanged set does not trigger a rebuild.
#
# Pictures: a PNG is scaled to fit 64x64 in NES palette colours (tools/nesimage.py): pixel art
# keeps its own colours, photos are dithered. Stored raw deflate compressed (the ESP32's ROM has
# the decoder), about a tenth of the 4 KB.
# A missing picture is a warning; the menu then shows an empty square.
#
# Fonts: fonts/font8x8.png and fonts/font4x6.png (tools/nesfont.py), drawn in any paint program,
# become src/generated/fonts.cpp.
#
# Manuals: teletext style pages of 32x25 characters with phpBB style tags, converted to a cut-down
# VT100 form for the device by tools/nesmanual.py (the format is described there). A missing
# manual is a warning; the menu then has none to show.
#
# Tape traps (tools/tapetrap.py): games whose Data Recorder saves are done in memory are found in
# their code; the option tape=machrider insists on one (a warning if it is not found), tape=none
# turns it off.
#
# An optional "<rom name>.cfg" next to a ROM (same name, .cfg instead of .nes) holds the same
# settings, one "key = value" per line; it is merged after the CSV's options.

import csv
import zlib
import os
import sys

Import("env")

project = env.subst("$PROJECT_DIR")
romDir = os.path.join(project, "roms")
outDir = os.path.join(project, "src", "generated")
picDir = os.path.join(outDir, "pics")
paletteH = os.path.join(project, "src", "nes", "palette.h")

sys.path.insert(0, os.path.join(project, "tools"))
import nesimage
import nesmanual
import nesfont
import tapetrap


def warn(msg):
    print("embed_roms: WARNING: " + msg)


def cString(s):
    # a C string literal of the UTF-8 bytes; escapes are closed with "" so a following hex digit
    # is not read as part of them
    out = ""
    for b in s.encode("utf-8"):
        ch = chr(b)
        if ch == "\\" or ch == '"':
            out += "\\" + ch
        elif ch == "\n":
            out += "\\n"
        elif b < 32 or b > 126:
            out += '\\x%02x""' % b
        else:
            out += ch
    return '"' + out + '"'


def cBytes(data):
    # a C string literal of raw bytes
    out = ""
    for b in data:
        ch = chr(b)
        if ch == "\\" or ch == '"':
            out += "\\" + ch
        elif ch == "\n":
            out += "\\n"
        elif ch == "\f":
            out += "\\f"
        elif b < 32 or b > 126:
            out += '\\x%02x""' % b
        else:
            out += ch
    return '"' + out + '"'


def asmString(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def writeIfChanged(path, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    old = None
    if os.path.exists(path):
        with open(path, "rb" if mode == "wb" else "r") as f:
            old = f.read()
    if old != data:
        with open(path, mode) as f:
            f.write(data)


# --- the game list ---

files = sorted((f for f in os.listdir(romDir) if f.lower().endswith((".nes", ".nsf"))) if os.path.isdir(romDir) else [],
               key=str.lower)
games = []           # dicts: file, name, picture, manual, options
listed = set()
commented = set()    # games whose line is commented out: left out, not listed at the end

csvPath = os.path.join(romDir, "settings.csv")
if os.path.exists(csvPath):
    with open(csvPath, newline="", encoding="utf-8") as f:
        for lineNo, row in enumerate(csv.reader(f, skipinitialspace=True), 1):
            if not row or not "".join(row).strip():
                continue
            if row[0].lstrip().startswith("#"):
                # a commented out game line keeps its game out of the build
                name = row[0].lstrip().lstrip("#").strip().strip('"')
                if name.lower().endswith((".nes", ".nsf")):
                    commented.add(name)
                continue
            row = [field.strip() for field in row]
            if len(row) < 2 or not row[0].lower().endswith((".nes", ".nsf")) or any('"' in field for field in row):
                warn("settings.csv line %d looks malformed (a missing quote?): %s" % (lineNo, row))
                continue
            row += [""] * (5 - len(row))
            if row[0] not in files:
                warn("settings.csv line %d: %s is not in roms/, skipped" % (lineNo, row[0]))
                continue
            if row[0] in listed:
                warn("settings.csv line %d: %s is listed twice, skipped" % (lineNo, row[0]))
                continue
            listed.add(row[0])
            games.append({"file": row[0], "name": row[1] or row[0][:-4], "picture": row[2],
                          "manual": row[3], "options": row[4]})
else:
    warn("no roms/settings.csv, listing the ROMs by file name")

for name in files:
    if name in commented and name not in listed:
        print("embed_roms: %s is commented out in settings.csv, left out" % name)
        continue
    if name not in listed:
        if os.path.exists(csvPath):
            warn("%s is not in settings.csv, listed at the end under its file name" % name)
        games.append({"file": name, "name": name[:-4], "picture": "", "manual": "", "options": ""})


# --- pictures, cached in src/generated/pics ---

os.makedirs(picDir, exist_ok=True)
palette = None

def picture(game):
    global palette
    if not game["picture"]:
        warn("%s: no picture given, the menu shows an empty square" % game["file"])
        return None
    png = os.path.join(romDir, game["picture"])
    if not os.path.exists(png):
        warn("%s: picture %s not found, the menu shows an empty square" % (game["file"], game["picture"]))
        return None
    cache = os.path.join(picDir, game["picture"] + ".bin")
    sources = [png, paletteH, os.path.join(project, "tools", "nesimage.py")]
    if os.path.exists(cache) and os.path.getmtime(cache) >= max(os.path.getmtime(f) for f in sources):
        with open(cache, "rb") as f:
            return f.read()
    try:
        if palette is None:
            palette = nesimage.load_palette(paletteH)
        data, used = nesimage.convert(png, palette)
    except Exception as e:
        warn("%s: cannot convert %s (%s), the menu shows an empty square" % (game["file"], game["picture"], e))
        return None
    with open(cache, "wb") as f:
        f.write(data)
    print("embed_roms: converted %s (as %s)" % (game["picture"], used))
    return data


# --- generated sources ---

asm = ["// generated by tools/embed_roms.py from roms/, do not edit", ""]
cpp = ["// generated by tools/embed_roms.py from roms/, do not edit", "",
       '#include "../romindex.h"', ""]
entries = []
total = 0
for i, g in enumerate(games):
    path = os.path.join(romDir, g["file"])
    size = os.path.getsize(path)
    total += size
    asm += ['.section .rodata.nesrom%d, "a"' % i, ".balign 4",
            ".global nesrom_%d" % i, "nesrom_%d:" % i, ".incbin " + asmString(path), ""]
    cpp.append("extern const uint8_t nesrom_%d[];" % i)

    pic = picture(g)
    picRef, picSize = "nullptr", 0
    if pic:
        packer = zlib.compressobj(9, zlib.DEFLATED, -15)      # raw deflate, no zlib header
        pic = packer.compress(pic) + packer.flush()
        picSize = len(pic)
        cpp.append("static const uint8_t picture_%d[%d] = {" % (i, len(pic)))
        for k in range(0, len(pic), 32):
            cpp.append("  " + ",".join(str(b) for b in pic[k:k + 32]) + ",")
        cpp.append("};")
        picRef = "picture_%d" % i

    manualRef = "nullptr"
    if g["manual"]:
        mpath = os.path.join(romDir, g["manual"])
        if os.path.exists(mpath):
            data, pages = nesmanual.convert(mpath, g["manual"], warn)
            if pages:
                manualRef = cBytes(data)
            else:
                warn("%s: manual %s has no pages" % (g["file"], g["manual"]))
        else:
            warn("%s: manual %s not found" % (g["file"], g["manual"]))

    # settings: the CSV's options column ("key=value;key=value") and an optional <rom>.cfg,
    # merged into one "key = value" per line text; the .cfg comes last and so wins
    settings = "\n".join(part.strip() for part in g["options"].split(";") if part.strip())
    cfgPath = os.path.join(romDir, g["file"][:-4] + ".cfg")
    if os.path.exists(cfgPath):
        with open(cfgPath, encoding="utf-8", errors="replace") as f:
            settings += "\n" + f.read()
    cfg = cString(settings) if settings.strip() else "nullptr"

    with open(path, "rb") as f:
        image = f.read()
    crc = zlib.crc32(image)

    # the tape trap: found in the code, unless "tape = none"; "tape = <game>" insists on one
    tapeOption = None
    for line in settings.splitlines():
        key, _, value = line.partition("=")
        if key.strip().lower() == "tape":
            tapeOption = value.split("#")[0].strip().lower()
    trapRef = "nullptr"
    if tapeOption != "none":
        if tapeOption and tapeOption not in tapetrap.GAMES:
            warn("%s: unknown tape=%s (known: none, %s)" % (g["file"], tapeOption, ", ".join(tapetrap.GAMES)))
        trap = tapetrap.find(image, tapeOption if tapeOption in tapetrap.GAMES else None)
        if trap:
            blocks = ", ".join("{ 0x%04x, 0x%x }" % b for b in trap["blocks"])
            patches = ", ".join("{ 0x%04x, 0x%02x, 0x%02x }" % p for p in trap["patches"])
            cpp.append("static const TapeTrap trap_%d = { %s, %s, 0x%04x, 0x%04x, %d, { %s }, %d, { %s } };"
                       % (i, cString(trap["game"]), "TRAP_FBASIC" if trap["kind"] == "fbasic" else "TRAP_VC",
                          trap["save"], trap["load"], len(trap["blocks"]), blocks, len(trap["patches"]), patches))
            trapRef = "&trap_%d" % i
            print("embed_roms: %s: tape trap for %s (%s)" % (g["file"], trap["game"], trap["kind"]))
        elif tapeOption:
            warn("%s: tape=%s, but its tape routines were not found" % (g["file"], tapeOption))

    entries.append("  { %s, %s, nesrom_%d, %d, 0x%08x, %s, %d, %s, %s, %s }," % (cString(g["file"]), cString(g["name"]), i, size, crc, picRef, picSize, manualRef, cfg, trapRef))

cpp += ["", "const RomBlob g_roms[] = {"] + (entries or ["  { nullptr, nullptr, nullptr, 0, 0, nullptr, 0, nullptr, nullptr, nullptr },"])
cpp += ["};", "", "const int g_romCount = %d;" % len(games), ""]

writeIfChanged(os.path.join(outDir, "roms.S"), "\n".join(asm) + "\n")
writeIfChanged(os.path.join(outDir, "romindex.cpp"), "\n".join(cpp))
print("embed_roms: %d games, %d KB of ROMs" % (len(games), total // 1024))


# --- fonts: fonts/*.png (tools/nesfont.py) as FabGL fonts, in src/generated/fonts.cpp ---

fontCpp = ["// generated by tools/embed_roms.py from fonts/*.png, do not edit", "",
           '#include "../fonts.h"', ""]
for name, (w, h, _) in nesfont.FONTS.items():
    png = os.path.join(project, "fonts", name + ".png")
    try:
        data = nesfont.from_png(png)
    except Exception as e:
        sys.exit("embed_roms: font %s: %s" % (png, e))
    fontCpp.append("static const uint8_t %s_data[%d] = {" % (name, len(data)))
    for k in range(0, len(data), 16):
        fontCpp.append("  " + ", ".join("0x%02x" % b for b in data[k:k + 16]) + ",")
    fontCpp += ["};", "",
                "const fabgl::FontInfo FONT_%s = {" % name[4:],
                "  .pointSize = %d, .width = %d, .height = %d, .ascent = %d," % (h, w, h, h - 1),
                "  .inleading = 0, .exleading = 0, .flags = 0, .weight = 400, .charset = 255,",
                "  .data = %s_data, .chptr = nullptr, .codepage = 437," % name,
                "};", ""]
writeIfChanged(os.path.join(outDir, "fonts.cpp"), "\n".join(fontCpp))
